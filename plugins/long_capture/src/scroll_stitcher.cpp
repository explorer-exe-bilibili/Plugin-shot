// 滚动位移估计：逐行 64 位哈希 + 高频行过滤 + 全量位移搜索。
//
// 算法（参数都写在 scroll_stitcher.h 的 public 常量里）：
//   1. 去重：把两帧各缩到 1/4 灰度缩略图，平均灰度差 <= kDuplicateMeanDiff 就丢弃新帧。
//   2. 行哈希：每行在 kRowSamples 个等距列上取 24 位颜色做 FNV-1a 哈希。
//      会话层只在"画面静止"时送帧，所以同一个内容行在两帧里逐像素一致，哈希相等
//      是精确判据——比原来的"32 列灰度差 <= 容差"可靠得多，后者会把差一两行的
//      文本行也判成匹配，把位移估小，拼出来就是一串重复行。
//   3. 高频行过滤：空白行、分隔线、大面积纯色行在任意位移下都相等。先统计两帧里
//      出现次数超过行数 1/8 的哈希，打分时跳过这些行，否则错误位移会被刷到满分。
//   4. 位移 d 的匹配分数 = 重叠区里"信息行"相等的比例。重叠区定义：
//        新帧第 y 行对应上一帧第 y + d 行；
//        d > 0（内容上移）时新帧 [0, H-d) 对上一帧 [d, H)；
//        d < 0（内容下移）时新帧 [-d, H) 对上一帧 [0, H+d)。
//   5. 搜索：d 的合法范围是 |d| <= H - max(8, 30% × H)（重叠不足 30% 直接不搜），
//      逐位移全量比较；822 行视口下约 47 万次哈希比较，静止帧触发频率低，可以接受。
//   6. 分数 < kMinMatchScore、信息行 < kMinInformativeRows，或最佳 d == 0 且分数不够
//      -> 缺口/重复，不硬拼。
//
// 为什么缺口时仍然更新基准帧：连续快速滚动会让"上一保留帧"越来越旧，不重设基准
// 就永远匹配不上新内容，缺口会无限延续。重设基准 = 从当前画面继续拼。

#include "scroll_stitcher.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "stitch_canvas.h"

namespace long_capture {
namespace {

/** @brief 亮度权重（BT.601 定点）：与 stb / 宿主其它插件用的灰度公式一致即可。 */
constexpr int kRedWeight = 77;
constexpr int kGreenWeight = 150;
constexpr int kBlueWeight = 29;

/** @brief 高频行的判定比例：出现次数超过行数的 1/8 就当作空白/重复行，不参与打分。 */
constexpr int kFrequentRowDivisor = 8;

/** @brief 单像素转灰度（0..255）。 */
int grayOf(const std::uint8_t* pixel) noexcept {
    return (pixel[0] * kRedWeight + pixel[1] * kGreenWeight + pixel[2] * kBlueWeight) >> 8;
}

/** @brief FNV-1a 64 位哈希的偏移基准与质数。 */
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

/** @brief 顶部/底部固定条带的最短行数：短于此长度的"相同行"当作内容，不当固定条。 */
constexpr int kMinStaticBand = 16;

/** @brief 一帧里不随滚动移动的顶部/底部条带（标题栏、状态栏、横向滚动条）。 */
struct StaticBands {
    int top = 0;     /**< 顶部固定的行数。 */
    int bottom = 0;  /**< 底部固定的行数。 */
};

/**
 * @brief 找出两帧里"同一行号且哈希相同"的顶部/底部连续条带。
 *
 * 为什么必须识别：窗口标题栏、编辑器状态栏、浏览器地址栏这类固定条不会随内容滚动。
 * 不排除它们的话，任何非零位移都会把这些行拿去和内容行比较，分数被拉低到阈值以下，
 * 于是每一帧都被判成缺口（长图高度不再增长）；硬拼时又会把固定条一行行重复贴进画布。
 *
 * @param[in] current  新帧行哈希。
 * @param[in] previous 上一帧行哈希。
 * @return 顶部/底部固定条行数；没有达到 kMinStaticBand 的条带记为 0。
 */
StaticBands detectStaticBands(const std::vector<std::uint64_t>& current,
                              const std::vector<std::uint64_t>& previous) {
    StaticBands bands;
    const int height = static_cast<int>(std::min(current.size(), previous.size()));
    if (height <= 1) {
        return bands;
    }
    int top = 0;
    while (top < height && current[static_cast<std::size_t>(top)] ==
                               previous[static_cast<std::size_t>(top)]) {
        ++top;
    }
    int bottom = 0;
    while (bottom < height - top &&
           current[static_cast<std::size_t>(height - 1 - bottom)] ==
               previous[static_cast<std::size_t>(height - 1 - bottom)]) {
        ++bottom;
    }
    bands.top = (top >= kMinStaticBand) ? top : 0;
    bands.bottom = (bottom >= kMinStaticBand) ? bottom : 0;
    return bands;
}

}  // namespace

ScrollStitcher::ScrollStitcher() = default;

void ScrollStitcher::reset() {
    m_width = 0;
    m_height = 0;
    m_thumb.clear();
    m_rowHashes.clear();
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

    std::vector<std::uint64_t> hashes;
    buildRowHashes(frame, hashes);

    // 首帧：整帧进画布。固定条要等第二帧才能识别（比较两帧同一行是否相同），
    // 识别出来后会把首帧里的固定条裁掉，所以这里先不区分。
    if (m_width == 0) {
        if (!canvas.append(frame, 0, frame.height)) {
            match.heightLimited = true;
            return match;
        }
        m_canvasTop = 0;
        m_refTop = 0;
        m_trimmed = false;
        updateReference(frame, thumb, hashes);
        match.matched = true;
        return match;
    }

    // 客户区尺寸变了（窗口被缩放/移动、DPI 变化）：硬拼没有意义，按缺口重设基准。
    if (frame.width != m_width || frame.height != m_height) {
        m_canvasTop = 0;
        m_refTop = 0;
        m_trimmed = false;
        updateReference(frame, thumb, hashes);
        return match;
    }

    if (looksIdentical(thumb)) {
        match.duplicate = true;
        return match;
    }

    const StaticBands bands = detectStaticBands(hashes, m_rowHashes);
    if (!m_trimmed && (bands.top > 0 || bands.bottom > 0)) {
        // 首帧里同样的固定条也一起裁掉：内容坐标从首帧移动区的第一行开始。
        canvas.trimTop(bands.top);
        canvas.trimBottom(bands.bottom);
        m_canvasTop = 0;
        m_refTop = 0;
        m_trimmed = true;
    }
    const int movingHeight = frame.height - bands.top - bands.bottom;

    double score = 0.0;
    int informative = 0;
    const std::int32_t delta =
        findDelta(hashes, bands.top, bands.bottom, score, informative);
    match.score = score;

    if (delta == 0) {
        if (score >= kMinMatchScore && informative >= kMinInformativeRows) {
            match.duplicate = true;  // 几乎没有新内容
        } else {
            // 位移为 0 但信息行对不上：光标闪烁/局部动画，按缺口处理并重设基准。
            m_refTop = m_canvasTop + canvas.height();
            updateReference(frame, thumb, hashes);
        }
        return match;
    }
    if (score < kMinMatchScore || informative < kMinInformativeRows) {
        // 缺口：不硬拼。基准前进到当前帧，内容坐标按"接在画布末尾"估一个值，
        // 让下一次匹配仍能算增量（否则坐标会永远停在上一次成功的位置）。
        m_refTop = m_canvasTop + canvas.height();
        updateReference(frame, thumb, hashes);
        return match;
    }

    // 内容坐标模型：画布只存"移动区"内容，固定条不进画布。
    //   m_refTop   = 上一帧移动区顶部的内容坐标
    //   m_canvasTop= 画布第 0 行的内容坐标
    // 只有当新视口超出画布已有内容范围时，超出的那部分是真正的新内容；
    // 向下滚回已拍过的区域（newRefTop 落在画布范围内）不追加也不前插——
    // 否则"下滑再上滑"会把已经拼过的内容再贴一遍，表现为重复/重影。
    const std::int64_t newRefTop = m_refTop + delta;
    const std::int64_t canvasBottom = m_canvasTop + canvas.height();

    if (newRefTop < m_canvasTop) {
        const int rows = static_cast<int>(m_canvasTop - newRefTop);
        if (rows > movingHeight) {
            // 视口与画布完全不重叠（用户一次跳过了整屏）：这是缺口，不是高度上限。
            // 以前这里会误报 "已达高度上限 <当前高度>"，用户只滚一屏就以为满了。
            m_refTop = m_canvasTop + canvas.height();
            updateReference(frame, thumb, hashes);
            return match;
        }
        if (!canvas.prepend(frame, bands.top, rows)) {
            match.heightLimited = true;
            return match;
        }
        m_canvasTop = newRefTop;
    } else if (newRefTop + movingHeight > canvasBottom) {
        const int rows = static_cast<int>(newRefTop + movingHeight - canvasBottom);
        if (rows > movingHeight) {
            m_refTop = m_canvasTop + canvas.height();
            updateReference(frame, thumb, hashes);
            return match;
        }
        if (!canvas.append(frame, frame.height - bands.bottom - rows, rows)) {
            match.heightLimited = true;
            return match;
        }
    }

    m_refTop = newRefTop;
    updateReference(frame, thumb, hashes);
    match.matched = true;
    match.delta = delta;
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

void ScrollStitcher::buildRowHashes(const CaptureFrame& frame,
                                    std::vector<std::uint64_t>& out) {
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

template <typename FrequentPredicate>
double ScrollStitcher::scoreDelta(const std::vector<std::uint64_t>& hashes, std::int32_t delta,
                                  int topStatic, int bottomStatic, FrequentPredicate isFrequent,
                                  int& informative) const {
    informative = 0;
    // 只比较移动区 [topStatic, H - bottomStatic)，且配对行也必须落在移动区：
    //   yNew ∈ [topStatic, movingEnd) 且 yNew + delta ∈ [topStatic, movingEnd)
    const int movingEnd = m_height - bottomStatic;
    const int startNew = std::max({topStatic, topStatic - delta, 0});
    const int endNew = std::min(movingEnd, movingEnd - delta);
    if (endNew <= startNew) {
        return 0.0;
    }
    int matched = 0;
    for (int y = startNew; y < endNew; ++y) {
        const std::uint64_t current = hashes[static_cast<std::size_t>(y)];
        const std::uint64_t previous = m_rowHashes[static_cast<std::size_t>(y + delta)];
        if (isFrequent(current) || isFrequent(previous)) {
            continue;  // 空白/重复行不携带位移信息
        }
        ++informative;
        if (current == previous) {
            ++matched;
        }
    }
    if (informative == 0) {
        return 0.0;
    }
    return static_cast<double>(matched) / static_cast<double>(informative);
}

std::int32_t ScrollStitcher::findDelta(const std::vector<std::uint64_t>& hashes, int topStatic,
                                       int bottomStatic, double& score, int& informative) const {
    score = 0.0;
    informative = 0;
    const int movingHeight = m_height - topStatic - bottomStatic;
    if (m_height <= 0 || movingHeight <= 0) {
        return 0;
    }
    const int minOverlap = std::max(8, movingHeight * kMinOverlapPercent / 100);
    const int maxShift = movingHeight - minOverlap;
    if (maxShift <= 0) {
        return 0;  // 视口太矮，连 30% 重叠都凑不出来：不拼
    }

    // 统计两帧里的高频哈希（空白行、分隔线、大面积纯色）。两帧都要看：新帧的空白行
    // 和旧帧的空白行未必在同一行号上，只过滤一边仍会让另一边参与凑分。
    std::unordered_map<std::uint64_t, int> frequency;
    frequency.reserve(static_cast<std::size_t>(hashes.size()) * 2);
    for (const std::uint64_t hash : hashes) {
        ++frequency[hash];
    }
    for (const std::uint64_t hash : m_rowHashes) {
        ++frequency[hash];
    }
    const int frequentThreshold =
        std::max(4, (m_height + static_cast<int>(hashes.size())) / kFrequentRowDivisor);
    const auto isFrequent = [&](std::uint64_t hash) {
        const auto it = frequency.find(hash);
        return it != frequency.end() && it->second > frequentThreshold;
    };

    std::int32_t best = 0;
    double bestScore = -1.0;
    int bestInformative = 0;
    for (int delta = -maxShift; delta <= maxShift; ++delta) {
        int candidateInformative = 0;
        const double candidate =
            scoreDelta(hashes, delta, topStatic, bottomStatic, isFrequent, candidateInformative);
        if (candidateInformative < kMinInformativeRows) {
            continue;
        }
        // 分数优先；分数相同时优先更大的 |d|：位移估小会让新拼进来的行与已有内容
        // 重复（重复行是好一点的失败模式吗？不，重复行会污染长图），所以反过来
        // 宁可选中真正滚过的那一段。相同 |d| 时取正位移（向下滚动更常见）。
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
        return 0;
    }
    score = bestScore;
    informative = bestInformative;
    return best;
}

void ScrollStitcher::updateReference(const CaptureFrame& frame,
                                     const std::vector<std::uint8_t>& thumb,
                                     const std::vector<std::uint64_t>& hashes) {
    m_width = frame.width;
    m_height = frame.height;
    m_thumb = thumb;
    m_rowHashes = hashes;
}

}  // namespace long_capture
