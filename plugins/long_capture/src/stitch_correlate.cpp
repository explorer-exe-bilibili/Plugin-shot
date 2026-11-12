// correlate 算法：全宽 2D 相关（64 列块均值矩阵上的归一化互相关）。
//
// 为什么默认用它：编辑器里相似行、重复代码块很常见，逐行哈希会把"差几行但内容
// 很像"的位移也判成高分（或把高频行全过滤掉导致缺口）；块均值 + NCC 看的是整块
// 像素的灰度结构，局部相似但整体对不齐的位移拿不到高分。
//
// 签名：每帧降采样成 64 列 × H 行的块均值矩阵（每格是若干像素的平均灰度，横向
// 压缩但纵向保持逐行，保证垂直位移是整数行）。每帧只构建一次，缓存在 StitchSignature。
//
// 打分：重叠区全部块上的归一化互相关
//     ncc = cov(A, B) / sqrt(var(A) * var(B))
// 内容反色不可能来自滚动，负相关没有意义，所以 clamp 到 0..1 后与 kMinMatchScore
// 直接比较（0.90 ≈ 重叠区灰度结构基本重合）。
//
// 耗时：全量候选是 O(|d| × H × 64)。为控制耗时，先在一份"垂直 4 行合并"的粗粒度
// 矩阵上按像素步长 4 粗搜（粗位移 k 对应 4k 像素），再在候选的 ±4 像素邻域里精搜。
// 为什么粗搜必须用合并后的矩阵：逐像素随机/高频文本的相关峰只有 1 像素宽，直接按
// 步长 4 抽样会整个跳过真峰；垂直合并 4 行会把峰拉宽到 ±1 个粗格，粗搜才可靠。
//
// 为什么要保留一组粗搜候选而不是只留第一名：周期/重复内容会在粗粒度上产生多个
// 近乎等高的别名峰，真位移又常常不是 4 的倍数（粗搜对不齐，只能拿到 0.7 左右），
// 只取第一名会稳定选中别名。这里按"粗分数不低于最优一半"筛出一小撮候选，再在
// 全分辨率上逐一精搜，用"谁的重叠区更吻合"区分真位移和周期别名。
//
// 复杂度：粗搜 O((maxShift/4) × (H/4) × 64)，精搜 O(候选数 × 9 × H × 64)
// （候选数上限 24；普通内容只有真位移附近能过 0.5 的入围线，通常只精搜 1 个；
// 800 行视口普通内容约 3M 次乘加，远低于逐像素 NCC，也低于 hash 的全量搜索）。

#include "stitch_estimator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace long_capture {
namespace {

/** @brief 块均值的列数：横向降采样到 64 列，足够保留文本行的横向结构。 */
constexpr int kBlockColumns = 64;

/** @brief 粗搜矩阵的垂直合并行数：4 行压成 1 格，对应粗搜步长 4 像素。 */
constexpr int kCoarseRowSpan = 4;

/** @brief 粗搜步长（像素）与精搜半径（像素）。 */
constexpr int kCoarseStep = kCoarseRowSpan;
constexpr int kFineRadius = 4;

/** @brief 精搜候选上限：低纹理内容的粗分数普遍很低，截断避免退化成全量搜索。 */
constexpr int kMaxFineCandidates = 24;

/** @brief 粗搜候选入围比例：粗分数 >= 最优粗分数 × 它的候选才进精搜。 */
constexpr double kCoarseAcceptRatio = 0.5;

/** @brief 亮度权重（BT.601 定点）：与画布/去重用的灰度公式一致。 */
constexpr int kRedWeight = 77;
constexpr int kGreenWeight = 150;
constexpr int kBlueWeight = 29;

/** @brief 候选分数的近似相等阈值：浮点累加误差远小于它，周期内容的并列峰远大于它。 */
constexpr double kScoreEpsilon = 1e-6;

/** @brief 单像素转灰度（0..255）。 */
int grayOf(const std::uint8_t* pixel) noexcept {
    return (pixel[0] * kRedWeight + pixel[1] * kGreenWeight + pixel[2] * kBlueWeight) >> 8;
}

/**
 * @brief 候选比较：分数优先；分数几乎相同时优先位移更小的（重叠区更大、证据更多），
 *        再优先正位移（向下滚动更常见）。
 *
 * 为什么不学 hash 的"相同分数取大 |d|"：NCC 已经做了归一化，不会因为重叠变小而虚高；
 * 小位移保留更多已确认内容，错选远端别名时重复的代价更大。
 *
 * @param[in] score     候选分数。
 * @param[in] delta     候选位移。
 * @param[in] bestScore 当前最佳分数（< 0 表示还没有候选）。
 * @param[in] bestDelta 当前最佳位移。
 * @return true = 候选更好。
 */
bool betterCandidate(double score, int delta, double bestScore, int bestDelta) {
    if (bestScore < 0.0 || score > bestScore + kScoreEpsilon) {
        return true;
    }
    if (score < bestScore - kScoreEpsilon) {
        return false;
    }
    if (std::abs(delta) != std::abs(bestDelta)) {
        return std::abs(delta) < std::abs(bestDelta);
    }
    return delta > bestDelta;
}

/** @brief 一个粗搜候选：像素位移 + 粗粒度 NCC 分数。 */
struct CoarseCandidate {
    int delta = 0;       /**< 像素位移（粗位移 × 步长）。 */
    double score = 0.0;  /**< 粗粒度 NCC 分数 0..1。 */
};

/**
 * @brief 计算行优先块均值矩阵在位移 delta 下的重叠区 NCC。
 * @param[in]  current     当前帧矩阵（height × columns）。
 * @param[in]  reference   参考帧矩阵（height × columns）。
 * @param[in]  height      矩阵行数。
 * @param[in]  columns     矩阵列数。
 * @param[in]  delta       带符号位移（行）。
 * @param[in]  topStatic   顶部固定条行数。
 * @param[in]  bottomStatic 底部固定条行数。
 * @param[out] informative 参与打分的重叠行数。
 * @return 0..1 的 NCC；没有重叠或重叠区没有方差（全白/全黑）时返回 0。
 */
double nccAt(const std::uint8_t* current, const std::uint8_t* reference, int height, int columns,
             int delta, int topStatic, int bottomStatic, int& informative) {
    informative = 0;
    const int movingEnd = height - bottomStatic;
    const int startNew = std::max({topStatic, topStatic - delta, 0});
    const int endNew = std::min(movingEnd, movingEnd - delta);
    informative = endNew - startNew;
    if (informative <= 0) {
        return 0.0;
    }

    // 先累加一阶/二阶和，再统一算均值、方差与协方差：比逐元素两遍扫描省一半内存访问。
    double sumA = 0.0;
    double sumB = 0.0;
    double sumAA = 0.0;
    double sumBB = 0.0;
    double sumAB = 0.0;
    for (int y = startNew; y < endNew; ++y) {
        const std::uint8_t* rowA = current + static_cast<std::size_t>(y) * columns;
        const std::uint8_t* rowB = reference + static_cast<std::size_t>(y + delta) * columns;
        for (int c = 0; c < columns; ++c) {
            const double a = static_cast<double>(rowA[c]);
            const double b = static_cast<double>(rowB[c]);
            sumA += a;
            sumB += b;
            sumAA += a * a;
            sumBB += b * b;
            sumAB += a * b;
        }
    }
    const double count = static_cast<double>(informative) * static_cast<double>(columns);
    const double meanA = sumA / count;
    const double meanB = sumB / count;
    const double varianceA = sumAA / count - meanA * meanA;
    const double varianceB = sumBB / count - meanB * meanB;
    const double variance = varianceA * varianceB;
    if (!(variance > 1e-12)) {
        return 0.0;  // 纯色/空白：NCC 没有定义，交给缺口判据
    }
    const double covariance = sumAB / count - meanA * meanB;
    const double ncc = covariance / std::sqrt(variance);
    return std::clamp(ncc, 0.0, 1.0);
}

/**
 * @brief 把一帧压成 64 列块均值矩阵，并额外生成一份垂直 4 行合并的粗粒度矩阵。
 * @param[in]  frame     当前帧。
 * @param[out] signature 目标签名；覆盖 blockMeans / blockMeansCoarse。
 */
void prepareCorrelateSignature(const CaptureFrame& frame, StitchSignature& signature) {
    const int height = frame.height;
    const int width = frame.width;
    const int columns = kBlockColumns;
    signature.blockMeans.resize(static_cast<std::size_t>(height) * columns);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = frame.row(y);
        std::uint8_t* target =
            signature.blockMeans.data() + static_cast<std::size_t>(y) * columns;
        for (int c = 0; c < columns; ++c) {
            // 列区间按比例切分并夹住边界：宽度小于 64 时也不越界（真实窗口远宽于 64）。
            int x0 = static_cast<int>(static_cast<long long>(width) * c / columns);
            int x1 = static_cast<int>(static_cast<long long>(width) * (c + 1) / columns);
            x0 = std::clamp(x0, 0, std::max(0, width - 1));
            x1 = std::clamp(x1, x0 + 1, std::max(1, width));
            long long sum = 0;
            for (int x = x0; x < x1; ++x) {
                sum += grayOf(row + static_cast<std::size_t>(x) * 4);
            }
            target[c] = static_cast<std::uint8_t>(sum / (x1 - x0));
        }
    }

    // 粗粒度矩阵：每 kCoarseRowSpan 行合并成一格。合并让相关峰变宽（±1 粗格），
    // 步长 4 的粗搜才不会跳过只有 1 像素宽的真峰；最后一行不足 4 行时按实际行数平均。
    const int coarseHeight = std::max(1, (height + kCoarseRowSpan - 1) / kCoarseRowSpan);
    signature.blockMeansCoarse.resize(static_cast<std::size_t>(coarseHeight) * columns);
    for (int coarseY = 0; coarseY < coarseHeight; ++coarseY) {
        const int y0 = coarseY * kCoarseRowSpan;
        const int y1 = std::min(height, y0 + kCoarseRowSpan);
        std::uint8_t* target =
            signature.blockMeansCoarse.data() + static_cast<std::size_t>(coarseY) * columns;
        if (y1 <= y0) {
            std::fill(target, target + columns, static_cast<std::uint8_t>(0));
            continue;
        }
        for (int c = 0; c < columns; ++c) {
            int sum = 0;
            for (int y = y0; y < y1; ++y) {
                sum += signature.blockMeans[static_cast<std::size_t>(y) * columns + c];
            }
            target[c] = static_cast<std::uint8_t>(sum / (y1 - y0));
        }
    }
}

/**
 * @brief 见文件头：粗搜（步长 4）筛出候选，再在各候选的全分辨率 ±4 邻域精搜。
 * @param[in] reference 参考帧签名。
 * @param[in] current   当前帧签名。
 * @param[in] topStatic 顶部固定条行数。
 * @param[in] bottomStatic 底部固定条行数。
 * @return 最佳位移、NCC 分数、重叠行数。
 */
StitchEstimate estimateCorrelateDelta(const StitchSignature& reference,
                                      const StitchSignature& current, int topStatic,
                                      int bottomStatic) {
    StitchEstimate estimate;
    const int height = reference.height;
    const int columns = kBlockColumns;
    if (height <= 0 || current.height != height) {
        return estimate;
    }
    const std::size_t fullBytes = static_cast<std::size_t>(height) * columns;
    if (reference.blockMeans.size() < fullBytes || current.blockMeans.size() < fullBytes) {
        return estimate;
    }
    const int movingHeight = height - topStatic - bottomStatic;
    const int maxShift = maxStitchShift(movingHeight);
    if (maxShift <= 0) {
        return estimate;
    }

    // 粗搜只在完整落在移动区里的粗格上比较，固定条不参与。
    const int coarseHeight = std::max(1, (height + kCoarseRowSpan - 1) / kCoarseRowSpan);
    const std::size_t coarseBytes = static_cast<std::size_t>(coarseHeight) * columns;
    const int coarseTop = (topStatic + kCoarseRowSpan - 1) / kCoarseRowSpan;
    const int coarseEnd = (height - bottomStatic) / kCoarseRowSpan;
    const int coarseBottom = coarseHeight - coarseEnd;
    const bool coarseUsable = reference.blockMeansCoarse.size() >= coarseBytes &&
                              current.blockMeansCoarse.size() >= coarseBytes &&
                              coarseEnd > coarseTop;

    // 粗搜：收集所有粗格候选（像素位移 + 粗分数），精搜再挑其中最强的一小撮复验。
    std::vector<CoarseCandidate> candidates;
    if (coarseUsable) {
        const int coarseMax = maxShift / kCoarseStep;
        candidates.reserve(static_cast<std::size_t>(coarseMax * 2 + 1));
        for (int coarseDelta = -coarseMax; coarseDelta <= coarseMax; ++coarseDelta) {
            int informative = 0;
            const double score =
                nccAt(current.blockMeansCoarse.data(), reference.blockMeansCoarse.data(),
                      coarseHeight, columns, coarseDelta, coarseTop, coarseBottom, informative);
            if (informative <= 0) {
                continue;
            }
            candidates.push_back(CoarseCandidate{coarseDelta * kCoarseStep, score});
        }
    }

    // 精搜：在中心候选的 ±radius 像素里逐像素比较全分辨率矩阵，用最终 NCC 定胜负。
    // 为什么不用粗分数直接定胜负：粗粒度合并会把"平移 32/33 行"这类周期别名磨成
    // 满分，而真位移（37 行这种非 4 倍数的位移）只能拿到 0.75 左右；只有回到全
    // 分辨率按重叠区灰度结构比较，才能区分真位移和别名。
    std::int32_t bestDelta = 0;
    double bestScore = -1.0;
    int bestInformative = 0;
    const auto fineSearch = [&](int centerDelta, int radius) {
        const int start = std::clamp(centerDelta - radius, -maxShift, maxShift);
        const int end = std::clamp(centerDelta + radius, -maxShift, maxShift);
        for (int delta = start; delta <= end; ++delta) {
            int informative = 0;
            const double score = nccAt(current.blockMeans.data(), reference.blockMeans.data(),
                                       height, columns, delta, topStatic, bottomStatic, informative);
            if (informative < kMinInformativeRows) {
                continue;
            }
            if (betterCandidate(score, delta, bestScore, bestDelta)) {
                bestDelta = delta;
                bestScore = score;
                bestInformative = informative;
            }
        }
    };

    int evaluated = 0;
    if (candidates.empty()) {
        // 粗搜矩阵不可用（极矮视口/尺寸异常）：退回全范围精搜，正确性优先。
        fineSearch(0, maxShift);
    } else {
        std::sort(candidates.begin(), candidates.end(),
                  [](const CoarseCandidate& left, const CoarseCandidate& right) {
                      return left.score > right.score;
                  });
        const double threshold = candidates.front().score * kCoarseAcceptRatio;
        for (const CoarseCandidate& candidate : candidates) {
            if (candidate.score < threshold || evaluated >= kMaxFineCandidates) {
                break;  // 已按分数降序：后面的只会更弱
            }
            fineSearch(candidate.delta, kFineRadius);
            ++evaluated;
        }
    }
    if (bestScore < 0.0) {
        return estimate;
    }
    estimate.delta = bestDelta;
    estimate.score = std::max(0.0, bestScore);
    estimate.informative = bestInformative;
    return estimate;
}

}  // namespace

StitchEstimatorOps correlateEstimatorOps() noexcept {
    StitchEstimatorOps ops;
    ops.name = stitchAlgorithmName(StitchAlgorithm::Correlate);
    ops.prepare = &prepareCorrelateSignature;
    ops.estimate = &estimateCorrelateDelta;
    return ops;
}

}  // namespace long_capture
