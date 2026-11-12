// hash 算法：逐行 64 位哈希 + 高频行过滤 + 带符号全量搜索（旧版基线，保留对照）。
//
// 为什么保留：它是"逐像素相等"的精确判据，在普通文本/代码页面上又快又准；但遇到
// 相似行、重复代码块时会给出多个接近满分的错误候选（下面 tie-break 会选更大的
// |d|），所以默认算法已经换成 correlate。
//
// 打分：重叠区里"信息行"相等的比例。信息行 = 两帧里出现次数不超过总行数 1/8 的
// 哈希（空白行、分隔线、大面积纯色这类高频行在任意位移下都相等，不排除会把错误
// 位移刷到满分）。分数天然在 0..1，与 kMinMatchScore 直接比较。
//
// 复杂度：O(H²) 次行比较（H = 视口行数）；800 行视口约 50 万次哈希比较。只在
// "画面静止"时触发，可以接受，所以这里保持旧版的全量带符号搜索不改。

#include "stitch_estimator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace long_capture {
namespace {

/** @brief 高频行的判定比例：出现次数超过行数的 1/8 就当作空白/重复行。 */
constexpr int kFrequentRowDivisor = 8;

/**
 * @brief 统计位移 delta 下重叠区里"信息行"的匹配分数。
 *
 * 只比较移动区 [topStatic, H - bottomStatic)，且配对行也必须落在移动区：
 *   yNew ∈ [topStatic, movingEnd) 且 yNew + delta ∈ [topStatic, movingEnd)。
 *
 * @param[in]  reference    参考帧行哈希。
 * @param[in]  current      当前帧行哈希。
 * @param[in]  delta        带符号位移。
 * @param[in]  topStatic    顶部固定条行数。
 * @param[in]  bottomStatic 底部固定条行数。
 * @param[in]  isFrequent   高频哈希判定（下标是哈希本身的值）。
 * @param[out] informative  参与打分的信息行数。
 * @return 0..1 的匹配分数；没有信息行时返回 0。
 */
template <typename FrequentPredicate>
double scoreDelta(const std::vector<std::uint64_t>& reference,
                  const std::vector<std::uint64_t>& current, std::int32_t delta, int topStatic,
                  int bottomStatic, FrequentPredicate isFrequent, int& informative) {
    const int height = static_cast<int>(current.size());
    informative = 0;
    const int movingEnd = height - bottomStatic;
    const int startNew = std::max({topStatic, topStatic - delta, 0});
    const int endNew = std::min(movingEnd, movingEnd - delta);
    if (endNew <= startNew) {
        return 0.0;
    }
    int matched = 0;
    for (int y = startNew; y < endNew; ++y) {
        const std::uint64_t currentHash = current[static_cast<std::size_t>(y)];
        const std::uint64_t referenceHash = reference[static_cast<std::size_t>(y + delta)];
        if (isFrequent(currentHash) || isFrequent(referenceHash)) {
            continue;  // 空白/重复行不携带位移信息
        }
        ++informative;
        if (currentHash == referenceHash) {
            ++matched;
        }
    }
    if (informative == 0) {
        return 0.0;
    }
    return static_cast<double>(matched) / static_cast<double>(informative);
}

/**
 * @brief hash 算法不需要额外预处理：行哈希是画布层统一构建的共享指纹。
 * @param[in] frame     当前帧（未使用）。
 * @param[in] signature 已填好 rowHashes 的签名（未修改）。
 */
void prepareHashSignature(const CaptureFrame& /*frame*/, StitchSignature& /*signature*/) {
    // 有意留空：避免每帧为同一份逐行哈希再采样一次。
}

/**
 * @brief 见文件头；保持旧版 ScrollStitcher::findDelta 的搜索与 tie-break 语义。
 * @param[in] reference 参考帧签名。
 * @param[in] current   当前帧签名。
 * @param[in] topStatic 顶部固定条行数。
 * @param[in] bottomStatic 底部固定条行数。
 * @return 最佳位移、信息行匹配比例、信息行数。
 */
StitchEstimate estimateHashDelta(const StitchSignature& reference, const StitchSignature& current,
                                 int topStatic, int bottomStatic) {
    StitchEstimate estimate;
    const int height = reference.height;
    if (height <= 0 || current.height != height) {
        return estimate;
    }
    if (static_cast<int>(reference.rowHashes.size()) != height ||
        static_cast<int>(current.rowHashes.size()) != height) {
        return estimate;
    }
    const int movingHeight = height - topStatic - bottomStatic;
    const int maxShift = maxStitchShift(movingHeight);
    if (maxShift <= 0) {
        return estimate;
    }

    // 统计两帧里的高频哈希（空白行、分隔线、大面积纯色）。两帧都要看：新帧的空白行
    // 和旧帧的空白行未必在同一行号上，只过滤一边仍会让另一边参与凑分。
    std::unordered_map<std::uint64_t, int> frequency;
    frequency.reserve(static_cast<std::size_t>(height) * 2);
    for (const std::uint64_t hash : current.rowHashes) {
        ++frequency[hash];
    }
    for (const std::uint64_t hash : reference.rowHashes) {
        ++frequency[hash];
    }
    const int frequentThreshold = std::max(4, (height + height) / kFrequentRowDivisor);
    const auto isFrequent = [&](std::uint64_t hash) {
        const auto it = frequency.find(hash);
        return it != frequency.end() && it->second > frequentThreshold;
    };

    std::int32_t best = 0;
    double bestScore = -1.0;
    int bestInformative = 0;
    for (int delta = -maxShift; delta <= maxShift; ++delta) {
        int candidateInformative = 0;
        const double candidate = scoreDelta(reference.rowHashes, current.rowHashes, delta, topStatic,
                                            bottomStatic, isFrequent, candidateInformative);
        if (candidateInformative < kMinInformativeRows) {
            continue;
        }
        // 分数优先；分数相同时优先更大的 |d|：位移估小会让新拼进来的行与已有内容
        // 重复（重复行会污染长图），所以宁可选中真正滚过的那一段。相同 |d| 时取
        // 正位移（向下滚动更常见）。这段 tie-break 是旧版行为，原样保留作基线。
        const bool better = (candidate > bestScore + 1e-9) ||
                            (std::abs(candidate - bestScore) <= 1e-9 &&
                             (std::abs(delta) > std::abs(best) ||
                              (std::abs(delta) == std::abs(best) && delta > best)));
        if (bestScore < 0.0 || better) {
            best = delta;
            bestScore = candidate;
            bestInformative = candidateInformative;
        }
    }

    if (bestScore < 0.0) {
        return estimate;
    }
    estimate.delta = best;
    estimate.score = bestScore;
    estimate.informative = bestInformative;
    return estimate;
}

}  // namespace

StitchEstimatorOps hashEstimatorOps() noexcept {
    StitchEstimatorOps ops;
    ops.name = stitchAlgorithmName(StitchAlgorithm::Hash);
    ops.prepare = &prepareHashSignature;
    ops.estimate = &estimateHashDelta;
    return ops;
}

}  // namespace long_capture
