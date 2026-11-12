// 滚动位移估计：灰度行指纹 + 粗搜/精搜两级匹配。
//
// 算法（参数都写在 scroll_stitcher.h 的 public 常量里）：
//   1. 去重：把两帧各缩到 1/4 灰度缩略图，平均灰度差 <= kDuplicateMeanDiff 就丢弃新帧。
//   2. 行指纹：每行在固定列上取 32 个灰度值；行与行比较 = 32 个字节的平均绝对差，
//      <= kRowTolerance 记为"这一行对上"。
//   3. 位移 d 的匹配分数 = 重叠区里"对上的行"占比。重叠区定义：
//        新帧第 y 行对应上一帧第 y + d 行；
//        d > 0（内容上移）时新帧 [0, H-d) 对上一帧 [d, H)；
//        d < 0（内容下移）时新帧 [-d, H) 对上一帧 [0, H+d)。
//   4. 搜索：d 的合法范围是 |d| <= H - max(8, 30% × H)（重叠不足 30% 直接不搜）；
//      先用最多 24 行做粗搜选出最佳 d，再在 d±2 内用全部重叠行精搜。
//      这样每帧大约 1400 × 24 × 32 次字节比较，60ms 一帧完全够用。
//   5. 分数 < kMinMatchScore 或最佳 d == 0 且分数不够 -> 缺口/重复，不硬拼。
//
// 为什么缺口时仍然更新基准帧：连续快速滚动会让"上一保留帧"越来越旧，不重设基准
// 就永远匹配不上新内容，缺口会无限延续。重设基准 = 从当前画面继续拼。

#include "scroll_stitcher.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "stitch_canvas.h"

namespace long_capture {
namespace {

/** @brief 粗搜时每个候选位移最多比较的行数；精搜用全部重叠行。 */
constexpr int kCoarseRows = 24;

/** @brief 精搜在粗搜最佳位移附近搜的半径（行）。 */
constexpr int kRefineRadius = 2;

/** @brief 亮度权重（BT.601 定点）：与 stb / 宿主其它插件用的灰度公式一致即可。 */
constexpr int kRedWeight = 77;
constexpr int kGreenWeight = 150;
constexpr int kBlueWeight = 29;

/** @brief 单像素转灰度（0..255）。 */
int grayOf(const std::uint8_t* pixel) noexcept {
    return (pixel[0] * kRedWeight + pixel[1] * kGreenWeight + pixel[2] * kBlueWeight) >> 8;
}

}  // namespace

ScrollStitcher::ScrollStitcher() = default;

void ScrollStitcher::reset() {
    m_width = 0;
    m_height = 0;
    m_thumb.clear();
    m_rows.clear();
}

ScrollMatch ScrollStitcher::addFrame(const CaptureFrame& frame, StitchCanvas& canvas) {
    ScrollMatch match;
    if (frame.empty() || canvas.width() != frame.width) {
        return match;  // 尺寸不对：交给调用方按失败处理，但不要动画布
    }

    std::vector<std::uint8_t> thumb;
    int thumbWidth = 0;
    int thumbHeight = 0;
    buildThumb(frame, thumb, thumbWidth, thumbHeight);

    // 首帧：直接铺满整张画布，作为后续匹配的基准。
    if (m_width == 0) {
        if (!canvas.append(frame, frame.height)) {
            match.heightLimited = true;
            return match;
        }
        buildRowSamples(frame, m_rows);
        updateReference(frame, thumb, m_rows);
        match.matched = true;
        match.delta = 0;
        return match;
    }

    // 客户区尺寸变了（窗口被缩放/移动、DPI 变化）：硬拼没有意义，按缺口重设基准。
    if (frame.width != m_width || frame.height != m_height) {
        buildRowSamples(frame, m_rows);
        updateReference(frame, thumb, m_rows);
        return match;
    }

    if (looksIdentical(thumb)) {
        match.duplicate = true;
        return match;
    }

    std::vector<std::uint8_t> rows;
    buildRowSamples(frame, rows);
    double score = 0.0;
    const std::int32_t delta = findDelta(rows, score);
    match.score = score;

    if (delta == 0) {
        if (score >= kMinMatchScore) {
            match.duplicate = true;  // 几乎没有新内容
        } else {
            // 位移为 0 但行指纹差很多：可能是光标闪烁/局部动画，按缺口处理并重设基准。
            updateReference(frame, thumb, rows);
        }
        return match;
    }
    if (score < kMinMatchScore) {
        match.duplicate = false;  // 明确是缺口：不硬拼，但基准前进到当前帧
        updateReference(frame, thumb, rows);
        return match;
    }

    // 正位移（内容上移）追加帧尾新内容；负位移（内容下移）把帧头新内容前插。
    const bool stitched =
        (delta > 0) ? canvas.append(frame, delta) : canvas.prepend(frame, -delta);
    if (!stitched) {
        match.heightLimited = true;
        return match;  // 已达高度上限：不更新基准，会话会停止继续拼接
    }
    updateReference(frame, thumb, rows);
    match.matched = true;
    match.delta = delta;
    match.score = score;
    return match;
}

void ScrollStitcher::buildThumb(const CaptureFrame& frame, std::vector<std::uint8_t>& out, int& w,
                                int& h) {
    w = std::max(1, frame.width / kThumbScale);
    h = std::max(1, frame.height / kThumbScale);
    out.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    for (int ty = 0; ty < h; ++ty) {
        const int y0 = ty * kThumbScale;
        const int y1 = std::min(frame.height, y0 + kThumbScale);
        for (int tx = 0; tx < w; ++tx) {
            const int x0 = tx * kThumbScale;
            const int x1 = std::min(frame.width, x0 + kThumbScale);
            int sum = 0;
            int count = 0;
            for (int y = y0; y < y1; ++y) {
                const std::uint8_t* srcRow = frame.row(y);
                for (int x = x0; x < x1; ++x) {
                    sum += grayOf(srcRow + static_cast<std::size_t>(x) * 4);
                    ++count;
                }
            }
            out[static_cast<std::size_t>(ty) * static_cast<std::size_t>(w) +
                static_cast<std::size_t>(tx)] =
                (count > 0) ? static_cast<std::uint8_t>(sum / count) : 0;
        }
    }
}

void ScrollStitcher::buildRowSamples(const CaptureFrame& frame, std::vector<std::uint8_t>& out) {
    out.resize(static_cast<std::size_t>(frame.height) * static_cast<std::size_t>(kRowSamples));
    const int lastX = frame.width - 1;
    for (int y = 0; y < frame.height; ++y) {
        const std::uint8_t* srcRow = frame.row(y);
        std::uint8_t* dst =
            out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(kRowSamples);
        for (int k = 0; k < kRowSamples; ++k) {
            const int x = static_cast<int>(static_cast<long long>(k) * lastX /
                                           static_cast<long long>(kRowSamples - 1));
            dst[k] = static_cast<std::uint8_t>(grayOf(srcRow + static_cast<std::size_t>(x) * 4));
        }
    }
}

bool ScrollStitcher::looksIdentical(const std::vector<std::uint8_t>& thumb) const {
    if (m_thumb.empty() || thumb.size() != m_thumb.size()) {
        return false;
    }
    long long difference = 0;
    for (std::size_t i = 0; i < thumb.size(); ++i) {
        difference += std::abs(static_cast<int>(thumb[i]) - static_cast<int>(m_thumb[i]));
    }
    return static_cast<double>(difference) / static_cast<double>(thumb.size()) <=
           kDuplicateMeanDiff;
}

double ScrollStitcher::scoreDelta(const std::vector<std::uint8_t>& rows, std::int32_t delta,
                                  int sampleCap) const {
    // 重叠区：新帧 [startNew, endNew) 与上一帧 [startNew + delta, endNew + delta)。
    const int startNew = std::max(0, -delta);
    const int endNew = std::min(m_height, m_height - delta);
    const int count = endNew - startNew;
    if (count <= 0) {
        return 0.0;
    }

    int samples = count;
    if (sampleCap > 0 && count > sampleCap) {
        samples = sampleCap;
    }

    int matched = 0;
    for (int i = 0; i < samples; ++i) {
        const int yNew = (samples == 1)
                             ? startNew
                             : startNew + static_cast<int>(static_cast<long long>(i) * (count - 1) /
                                                           (samples - 1));
        const int yPrev = yNew + delta;
        const std::uint8_t* current =
            rows.data() + static_cast<std::size_t>(yNew) * static_cast<std::size_t>(kRowSamples);
        const std::uint8_t* previous =
            m_rows.data() + static_cast<std::size_t>(yPrev) * static_cast<std::size_t>(kRowSamples);
        int difference = 0;
        for (int k = 0; k < kRowSamples; ++k) {
            difference += std::abs(static_cast<int>(current[k]) - static_cast<int>(previous[k]));
        }
        if (static_cast<double>(difference) / static_cast<double>(kRowSamples) <= kRowTolerance) {
            ++matched;
        }
    }
    return static_cast<double>(matched) / static_cast<double>(samples);
}

std::int32_t ScrollStitcher::findDelta(const std::vector<std::uint8_t>& rows, double& score) const {
    score = 0.0;
    if (m_height <= 0) {
        return 0;
    }
    const int minOverlap = std::max(8, m_height * kMinOverlapPercent / 100);
    const int maxShift = m_height - minOverlap;
    if (maxShift <= 0) {
        return 0;  // 视口太矮，连 30% 重叠都凑不出来：不拼
    }

    // 粗搜：每个候选只看少量行，先把候选收敛到 1~2 个位置。
    std::int32_t best = 0;
    double bestScore = -1.0;
    for (int delta = -maxShift; delta <= maxShift; ++delta) {
        const double candidate = scoreDelta(rows, delta, kCoarseRows);
        if (candidate > bestScore ||
            (candidate == bestScore && std::abs(delta) < std::abs(best))) {
            best = delta;
            bestScore = candidate;
        }
    }

    // 精搜：在粗搜结果附近用全部重叠行重算，避免抽样偏差把位移定错一行。
    std::int32_t refined = best;
    double refinedScore = -1.0;
    const int from = std::max(-maxShift, best - kRefineRadius);
    const int to = std::min(maxShift, best + kRefineRadius);
    for (int delta = from; delta <= to; ++delta) {
        const double candidate = scoreDelta(rows, delta, 0);
        if (candidate > refinedScore ||
            (candidate == refinedScore && std::abs(delta) < std::abs(refined))) {
            refined = delta;
            refinedScore = candidate;
        }
    }
    score = std::max(0.0, refinedScore);
    return refined;
}

void ScrollStitcher::updateReference(const CaptureFrame& frame,
                                     const std::vector<std::uint8_t>& thumb,
                                     const std::vector<std::uint8_t>& rows) {
    m_width = frame.width;
    m_height = frame.height;
    m_thumb = thumb;
    m_rows = rows;
}

}  // namespace long_capture
