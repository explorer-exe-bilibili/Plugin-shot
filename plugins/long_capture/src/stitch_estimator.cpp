// 位移估计的公共部分：逐行哈希（三种算法共用的帧指纹）、候选范围、算法分派。
//
// 为什么行哈希放在公共层而不是 hash 算法里：固定条判定（ScrollStitcher）和 hash
// 算法都需要逐行哈希，放在这里才能保证一帧只采样一次；correlate / phase 也复用
// 同一份 rowHashes 判固定条，三种算法的条带行为因此完全一致。
//
// 本文件只用标准库，不包含任何平台头。

#include "stitch_estimator.h"

#include <algorithm>
#include <cstring>

namespace long_capture {
namespace {

/** @brief FNV-1a 64 位哈希的偏移基准与质数。 */
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

/** @brief 配置值 / 日志里的算法名与 "auto" 预留值（只在这里出现一次）。 */
const char kHashName[] = "hash";
const char kCorrelateName[] = "correlate";
const char kPhaseName[] = "phase";
const char kAutoName[] = "auto";

}  // namespace

void buildRowHashes(const CaptureFrame& frame, std::vector<std::uint64_t>& out) {
    out.resize(static_cast<std::size_t>(frame.height));
    const int lastX = frame.width - 1;
    for (int y = 0; y < frame.height; ++y) {
        const std::uint8_t* srcRow = frame.row(y);
        std::uint64_t hash = kFnvOffset;
        for (int k = 0; k < kRowSamples; ++k) {
            const int x = (kRowSamples == 1)
                              ? 0
                              : static_cast<int>(static_cast<long long>(k) * lastX /
                                                 static_cast<long long>(kRowSamples - 1));
            const std::uint8_t* pixel = srcRow + static_cast<std::size_t>(x) * 4;
            hash ^= static_cast<std::uint64_t>(pixel[0]) |
                    (static_cast<std::uint64_t>(pixel[1]) << 8) |
                    (static_cast<std::uint64_t>(pixel[2]) << 16);
            hash *= kFnvPrime;
        }
        out[static_cast<std::size_t>(y)] = hash;
    }
}

int maxStitchShift(int movingHeight) noexcept {
    if (movingHeight <= 0) {
        return 0;
    }
    const int minOverlap = std::max(8, movingHeight * kMinOverlapPercent / 100);
    const int maxShift = movingHeight - minOverlap;
    return (maxShift > 0) ? maxShift : 0;
}

StitchEstimatorOps estimatorOps(StitchAlgorithm algorithm) noexcept {
    switch (algorithm) {
        case StitchAlgorithm::Hash:
            return hashEstimatorOps();
        case StitchAlgorithm::Phase:
            return phaseEstimatorOps();
        case StitchAlgorithm::Correlate:
            return correlateEstimatorOps();
    }
    // 兜底：未知枚举按默认算法走，不把异常/失败推给拼接流程。
    return correlateEstimatorOps();
}

const char* stitchAlgorithmName(StitchAlgorithm algorithm) noexcept {
    switch (algorithm) {
        case StitchAlgorithm::Hash:
            return kHashName;
        case StitchAlgorithm::Phase:
            return kPhaseName;
        case StitchAlgorithm::Correlate:
            return kCorrelateName;
    }
    return kCorrelateName;
}

bool parseStitchAlgorithm(const char* text, StitchAlgorithm& out) noexcept {
    if (text == nullptr) {
        return false;
    }
    if (std::strcmp(text, kHashName) == 0) {
        out = StitchAlgorithm::Hash;
        return true;
    }
    if (std::strcmp(text, kCorrelateName) == 0) {
        out = StitchAlgorithm::Correlate;
        return true;
    }
    if (std::strcmp(text, kPhaseName) == 0) {
        out = StitchAlgorithm::Phase;
        return true;
    }
    if (std::strcmp(text, kAutoName) == 0) {
        // TODO: auto 是多算法融合（按内容特征自动选/投票）的预留值，当前只回落到
        //       默认的 correlate；多算法融合留待后续，不写进配置描述符的 options，
        //       设置页不展示。
        out = StitchAlgorithm::Correlate;
        return true;
    }
    return false;
}

}  // namespace long_capture
