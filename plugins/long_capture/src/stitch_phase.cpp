// phase 算法：逐行全宽平均亮度的 1D 相位相关。
//
// 为什么再加一种：行哈希要求"逐像素相等"，块均值 NCC 要遍历候选位移；相位相关只
// 看"哪一行有多亮"的纵向结构，互功率谱只保留相位、对整体亮度和轻微压缩噪声不敏感，
// 而且 FFT 的复杂度是 O(N log N)，比前两者都便宜。
//
// 流程：
//   1. 每行求全宽平均亮度（每帧一次，缓存在签名里，不按候选位移重复计算）；
//   2. 长度补到 >= H + maxShift 的 2 的幂（避免循环相关把边界绕回来）；
//   3. 去均值、去掉固定条行（标题栏/状态栏会在 tau=0 形成压倒性峰）后做 FFT，
//      互功率谱 C·conj(R) 归一化成单位幅度（相位相关）；
//   4. 逆变换后在合法位移范围里取整数峰（不做亚像素），峰值位置换回带符号位移；
//   5. score 用重叠区的 Pearson 相关系数 0..1：互功率谱峰高受重叠率影响，不适合
//      直接和 0.90 比较；相关系数对三种算法的含义一致。
//
// 已知弱点：严格重复的内容（周期图案）在相位相关里会产生一串近乎等高的别名峰，
// 而"哪一行有多亮"这种一维信号又不足以区分别名，所以 phase 在重复内容上比
// hash/correlate 更容易选到周期别名（见插件 README 的算法选择表）。
//
// 复杂度：O(N log N)，N 是补零后的长度（800 行视口约 2048 点），远低于 hash 的
// 全量搜索。FFT 是匿名命名空间里的迭代 radix-2 实现，刻意不写成宏。

#include "stitch_estimator.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

namespace long_capture {
namespace {

/** @brief 亮度权重（BT.601 定点）：与画布/去重/correlate 用的灰度公式一致。 */
constexpr int kRedWeight = 77;
constexpr int kGreenWeight = 150;
constexpr int kBlueWeight = 29;

/** @brief 候选分数的近似相等阈值：浮点累加误差远小于它。 */
constexpr double kScoreEpsilon = 1e-6;

/** @brief 单像素转灰度（0..255）。 */
int grayOf(const std::uint8_t* pixel) noexcept {
    return (pixel[0] * kRedWeight + pixel[1] * kGreenWeight + pixel[2] * kBlueWeight) >> 8;
}

/** @brief 取 >= value 的最小 2 的幂（radix-2 FFT 要求长度是 2 的幂）。 */
std::size_t nextPowerOfTwo(std::size_t value) noexcept {
    std::size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

/**
 * @brief 迭代 radix-2 快速傅里叶变换（原地）。
 *
 * 位反转置换 + 逐级蝶形是教科书式实现，拆成循环而不是宏，便于单步调试。
 * 逆变换用正角度旋转因子，最后统一除以 N 让 IFFT(FFT(x)) == x。
 *
 * @param[in,out] values  复数序列；长度必须是 2 的幂。
 * @param[in]     inverse true = 逆变换（带 1/N 缩放），false = 正变换。
 */
void fft(std::vector<std::complex<double>>& values, bool inverse) {
    const std::size_t size = values.size();
    if (size <= 1) {
        return;
    }
    // 位反转置换：把下标按二进制逆序重排，之后每一级蝶形都只处理相邻块。
    for (std::size_t index = 1, reversed = 0; index < size; ++index) {
        std::size_t bit = size >> 1;
        for (; (reversed & bit) != 0; bit >>= 1) {
            reversed ^= bit;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }
    const double direction = inverse ? 1.0 : -1.0;
    for (std::size_t length = 2; length <= size; length <<= 1) {
        const double angle =
            direction * 2.0 * std::numbers::pi / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        const std::size_t half = length / 2;
        for (std::size_t start = 0; start < size; start += length) {
            std::complex<double> factor(1.0, 0.0);
            for (std::size_t offset = 0; offset < half; ++offset) {
                const std::complex<double> even = values[start + offset];
                const std::complex<double> odd = values[start + offset + half] * factor;
                values[start + offset] = even + odd;
                values[start + offset + half] = even - odd;
                factor *= step;
            }
        }
    }
    if (inverse) {
        const double scale = 1.0 / static_cast<double>(size);
        for (std::complex<double>& value : values) {
            value *= scale;
        }
    }
}

/**
 * @brief 每行算全宽平均亮度（phase 的帧签名）。
 * @param[in]  frame     当前帧。
 * @param[out] signature 目标签名；覆盖 rowMeans。
 */
void preparePhaseSignature(const CaptureFrame& frame, StitchSignature& signature) {
    signature.rowMeans.resize(static_cast<std::size_t>(frame.height));
    for (int y = 0; y < frame.height; ++y) {
        const std::uint8_t* row = frame.row(y);
        long long sum = 0;
        for (int x = 0; x < frame.width; ++x) {
            sum += grayOf(row + static_cast<std::size_t>(x) * 4);
        }
        signature.rowMeans[static_cast<std::size_t>(y)] =
            static_cast<float>(sum) / static_cast<float>(frame.width);
    }
}

/**
 * @brief 位移 delta 下重叠区行均值的 Pearson 相关系数（0..1）。
 * @param[in]  current     当前帧行均值。
 * @param[in]  reference   参考帧行均值。
 * @param[in]  height      帧高。
 * @param[in]  delta       带符号位移。
 * @param[in]  topStatic   顶部固定条行数。
 * @param[in]  bottomStatic 底部固定条行数。
 * @param[out] informative 参与打分的重叠行数。
 * @return 0..1 的相关系数；重叠区没有方差时返回 0。
 */
double overlapCorrelation(const std::vector<float>& current, const std::vector<float>& reference,
                          int height, std::int32_t delta, int topStatic, int bottomStatic,
                          int& informative) {
    informative = 0;
    const int movingEnd = height - bottomStatic;
    const int startNew = std::max({topStatic, topStatic - delta, 0});
    const int endNew = std::min(movingEnd, movingEnd - delta);
    informative = endNew - startNew;
    if (informative <= 0) {
        return 0.0;
    }
    double sumA = 0.0;
    double sumB = 0.0;
    double sumAA = 0.0;
    double sumBB = 0.0;
    double sumAB = 0.0;
    for (int y = startNew; y < endNew; ++y) {
        const double a = static_cast<double>(current[static_cast<std::size_t>(y)]);
        const double b = static_cast<double>(reference[static_cast<std::size_t>(y + delta)]);
        sumA += a;
        sumB += b;
        sumAA += a * a;
        sumBB += b * b;
        sumAB += a * b;
    }
    const double count = static_cast<double>(informative);
    const double meanA = sumA / count;
    const double meanB = sumB / count;
    const double varianceA = sumAA / count - meanA * meanA;
    const double varianceB = sumBB / count - meanB * meanB;
    const double variance = varianceA * varianceB;
    if (!(variance > 1e-12)) {
        return 0.0;  // 整屏纯色：行均值没有起伏，位移不可信
    }
    const double covariance = sumAB / count - meanA * meanB;
    return std::clamp(covariance / std::sqrt(variance), 0.0, 1.0);
}

/**
 * @brief 见文件头：FFT 互功率谱 → 整数峰 → 重叠区相关系数打分。
 * @param[in] reference 参考帧签名。
 * @param[in] current   当前帧签名。
 * @param[in] topStatic 顶部固定条行数。
 * @param[in] bottomStatic 底部固定条行数。
 * @return 最佳位移、相关系数分数、重叠行数。
 */
StitchEstimate estimatePhaseDelta(const StitchSignature& reference, const StitchSignature& current,
                                  int topStatic, int bottomStatic) {
    StitchEstimate estimate;
    const int height = reference.height;
    if (height <= 0 || current.height != height) {
        return estimate;
    }
    if (static_cast<int>(reference.rowMeans.size()) < height ||
        static_cast<int>(current.rowMeans.size()) < height) {
        return estimate;
    }
    const int movingHeight = height - topStatic - bottomStatic;
    const int maxShift = maxStitchShift(movingHeight);
    if (maxShift <= 0) {
        return estimate;
    }

    // 长度必须 >= H + maxShift：循环相关的位移超过 N - H 会绕回另一端，在合法位移
    // 范围边缘造出假峰。补到 2 的幂（通常是 2H 附近）后交给 radix-2 FFT。
    const std::size_t length = nextPowerOfTwo(static_cast<std::size_t>(height) + maxShift);

    // 固定条（标题栏/状态栏）每帧都在同一行号上，而且亮度对比通常远大于内容行：
    // 不排除的话互功率谱会在 tau=0 形成强峰，把位移锁死到 0（实测 800 行视口里
    // 40+30 行固定条的能量能压过 730 行内容）。固定条不随内容滚动，这里按画布层
    // 给出的条带范围把它们从信号里去掉（置 0 等价于"等于移动区均值"）。
    const int movingStart = std::min(std::max(topStatic, 0), height);
    const int movingEnd = std::min(std::max(height - bottomStatic, movingStart), height);
    if (movingEnd - movingStart < kMinInformativeRows) {
        return estimate;
    }

    // 去均值（只对移动区）：文本页面的绝对亮度（白底/深色主题）会在零频处形成巨大
    // 峰值，不减去均值时互功率谱几乎等于常数，逆变换的峰被零频主导。
    double referenceMean = 0.0;
    double currentMean = 0.0;
    for (int y = movingStart; y < movingEnd; ++y) {
        referenceMean += static_cast<double>(reference.rowMeans[static_cast<std::size_t>(y)]);
        currentMean += static_cast<double>(current.rowMeans[static_cast<std::size_t>(y)]);
    }
    const double movingRows = static_cast<double>(movingEnd - movingStart);
    referenceMean /= movingRows;
    currentMean /= movingRows;

    std::vector<std::complex<double>> referenceSpectrum(length, 0.0);
    std::vector<std::complex<double>> currentSpectrum(length, 0.0);
    for (int y = movingStart; y < movingEnd; ++y) {
        referenceSpectrum[static_cast<std::size_t>(y)] =
            static_cast<double>(reference.rowMeans[static_cast<std::size_t>(y)]) - referenceMean;
        currentSpectrum[static_cast<std::size_t>(y)] =
            static_cast<double>(current.rowMeans[static_cast<std::size_t>(y)]) - currentMean;
    }
    fft(referenceSpectrum, false);
    fft(currentSpectrum, false);

    // 互功率谱只保留相位（幅度归一化）：整体亮度变化/压缩噪声不影响相位关系。
    std::vector<std::complex<double>> cross(length);
    for (std::size_t k = 0; k < length; ++k) {
        const std::complex<double> product = currentSpectrum[k] * std::conj(referenceSpectrum[k]);
        const double magnitude = std::abs(product);
        cross[k] = (magnitude > 1e-12) ? product / magnitude : std::complex<double>(0.0, 0.0);
    }
    fft(cross, true);

    // 互相关 r[τ] 的峰满足 τ = (-d) mod N：对 c[y] = r[y + d]，r[τ] = Σ c[y]·r[y - τ]
    // 在 y - τ = y + d（即 τ = -d）处最大。所以正位移对应 N - d，负位移对应 -d。
    std::int32_t bestDelta = 0;
    double bestPeak = -1.0;
    for (int delta = -maxShift; delta <= maxShift; ++delta) {
        const int tau = (delta > 0) ? static_cast<int>(length) - delta : -delta;
        const double peak = cross[static_cast<std::size_t>(tau)].real();
        // 峰值几乎并列时优先小 |d|（重叠更大、更保守），再优先正位移。
        const bool better = (bestPeak < 0.0) || (peak > bestPeak + kScoreEpsilon) ||
                            (std::abs(peak - bestPeak) <= kScoreEpsilon &&
                             (std::abs(delta) < std::abs(bestDelta) ||
                              (std::abs(delta) == std::abs(bestDelta) && delta > bestDelta)));
        if (better) {
            bestDelta = delta;
            bestPeak = peak;
        }
    }

    // 相位相关只负责找位移；score 用重叠区相关系数：峰高受重叠率影响，不适合直接
    // 和 0.90 比较；相关系数对三种算法的含义一致。
    int informative = 0;
    const double score = overlapCorrelation(current.rowMeans, reference.rowMeans, height, bestDelta,
                                            topStatic, bottomStatic, informative);
    if (informative < kMinInformativeRows) {
        return estimate;
    }
    estimate.delta = bestDelta;
    estimate.score = std::max(0.0, score);
    estimate.informative = informative;
    return estimate;
}

}  // namespace

StitchEstimatorOps phaseEstimatorOps() noexcept {
    StitchEstimatorOps ops;
    ops.name = stitchAlgorithmName(StitchAlgorithm::Phase);
    ops.prepare = &preparePhaseSignature;
    ops.estimate = &estimatePhaseDelta;
    return ops;
}

}  // namespace long_capture
