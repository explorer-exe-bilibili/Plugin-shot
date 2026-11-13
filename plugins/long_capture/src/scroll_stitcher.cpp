// 滚动拼接：画布坐标、固定条锁定、去重、缺口判定；位移估计交给可插拔函数表。
//
// 三种算法（hash / correlate / phase）实现在 stitch_hash.cpp / stitch_correlate.cpp /
// stitch_phase.cpp，共同的签名、候选范围与分派在 stitch_estimator.h。本文件只保留
// 与算法无关的拼接语义，保证换算法不会改变坐标/固定条/缺口行为：
//
//   1. 去重：把两帧各缩到 1/4 灰度缩略图，平均灰度差 <= kDuplicateMeanDiff 就丢弃新帧。
//   2. 帧签名：每帧只构建一次（逐行哈希由本文件统一做，算法专属表示由 prepare 回调做），
//      参考帧的签名缓存在 m_referenceSignature，下一帧直接复用，不重复降采样。
//   3. 固定条：两帧"同一行号且哈希相同"的顶部/底部连续条带（标题栏、状态栏、横向
//      滚动条）不随内容滚动，搜索与打分时必须排除。判定只在第一次出现非零位移时
//      发生一次，之后锁死（改动会整体错位，原因见 updateReference 附近注释）。
//   4. 缺口：位移搜索/打分由算法完成，本文件只按 score、informative 与重叠行数
//      决定"匹配/重复/缺口"，缺口时重设基准（否则连续滚动会永远恢复不了）。
//   5. 画布坐标：m_refTop / m_canvasTop 的内容坐标模型保持不变；向下滚回已拍过的
//      区域不追加也不前插，避免"下滑再上滑"把内容重复贴一遍。

#include "scroll_stitcher.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "stitch_canvas.h"

namespace long_capture {
namespace {

/** @brief 亮度权重（BT.601 定点）：与 stb / 宿主其它插件用的灰度公式一致即可。 */
constexpr int kRedWeight = 77;
constexpr int kGreenWeight = 150;
constexpr int kBlueWeight = 29;

/** @brief 顶部/底部固定条带的最短行数：短于此长度的"相同行"当作内容，不当固定条。 */
constexpr int kMinStaticBand = 16;

/** @brief 单像素转灰度（0..255）。 */
int grayOf(const std::uint8_t* pixel) noexcept {
    return (pixel[0] * kRedWeight + pixel[1] * kGreenWeight + pixel[2] * kBlueWeight) >> 8;
}

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
 * 用逐行哈希而不是灰度差：同一行在两帧里是逐像素一致的（会话层只在画面静止时送帧），
 * 哈希相等是精确判据；三种算法共用同一份哈希，固定条行为因此完全一致。
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
    // 先标记"同一行号且哈希相同"的行，再按 90% 覆盖率找顶部/底部条带。
    // 为什么不能要求逐行严格连续：标题栏/状态栏里可能有一两个会闪的元素（光标、
    // 进度点），只要有 3 行不同就把整条固定条判成 0，后面会退化成"不上不下"的
    // 错误坐标。允许少量不匹配、但要求整段覆盖率 >= 90%。
    std::vector<std::uint8_t> same(static_cast<std::size_t>(height), 0);
    for (int i = 0; i < height; ++i) {
        if (current[static_cast<std::size_t>(i)] == previous[static_cast<std::size_t>(i)]) {
            same[static_cast<std::size_t>(i)] = 1;
        }
    }

    const auto coverRatio = [&](int begin, int end) {
        int count = 0;
        for (int i = begin; i < end; ++i) {
            count += same[static_cast<std::size_t>(i)];
        }
        return (end > begin) ? static_cast<double>(count) / static_cast<double>(end - begin) : 0.0;
    };

    // 顶部：从第 0 行往下扩展，最多允许 2 行连续"不同"，覆盖率不足 90% 就回退。
    int top = 0;
    int gapRun = 0;
    for (int i = 0; i < height / 2; ++i) {
        if (same[static_cast<std::size_t>(i)]) {
            top = i + 1;
            gapRun = 0;
        } else if (++gapRun > 2) {
            break;
        }
    }
    if (top >= kMinStaticBand && coverRatio(0, top) < 0.90) {
        top = 0;
    }
    // 底部：对称处理。
    int bottom = 0;
    gapRun = 0;
    for (int i = height - 1; i >= height / 2; --i) {
        if (same[static_cast<std::size_t>(i)]) {
            bottom = height - i;
            gapRun = 0;
        } else if (++gapRun > 2) {
            break;
        }
    }
    if (bottom >= kMinStaticBand && coverRatio(height - bottom, height) < 0.90) {
        bottom = 0;
    }

    bands.top = (top >= kMinStaticBand) ? top : 0;
    bands.bottom = (bottom >= kMinStaticBand) ? bottom : 0;
    return bands;
}

}  // namespace

ScrollStitcher::ScrollStitcher(StitchAlgorithm algorithm)
    : m_algorithm(algorithm), m_estimator(estimatorOps(algorithm)) {}

void ScrollStitcher::reset() {
    m_width = 0;
    m_height = 0;
    m_thumb.clear();
    m_referenceSignature = StitchSignature{};
    m_currentSignature = StitchSignature{};
    m_refTop = 0;
    m_canvasTop = 0;
    m_bandsLocked = false;
    m_staticTop = 0;
    m_staticBottom = 0;
    clearAltReference();
    m_refTrusted = false;
    m_altDropped = false;
}

ScrollMatch ScrollStitcher::addFrame(const CaptureFrame& frame, StitchCanvas& canvas) {
    ScrollMatch match;
    if (frame.empty() || canvas.width() != frame.width) {
        return match;  // 尺寸不对：交给调用方按失败处理，但不要动画布
    }

    // 备用基准只保留 kAltRefMaxAge / kAltRefMaxFrames：超时后锚点已无意义，留着
    // 只会让有效期之后每帧多算一次匹配。丢没丢由会话层取一次性通知打日志。
    expireAltReference();

    std::vector<std::uint8_t> thumb;
    int thumbWidth = 0;
    int thumbHeight = 0;
    buildThumb(frame, thumb, thumbWidth, thumbHeight);

    // 每帧只做一次预处理：逐行哈希（固定条判定 + hash 算法共用）与当前算法专属表示。
    // m_currentSignature 的缓冲与参考帧轮换复用，steady 状态下不会逐帧重新分配。
    buildRowHashes(frame, m_currentSignature.rowHashes);
    m_currentSignature.height = frame.height;
    m_estimator.prepare(frame, m_currentSignature);

    // 首帧：整帧进画布。固定条要等第二帧才能识别（比较两帧同一行是否相同），
    // 识别出来后会把首帧里的固定条裁掉，所以这里先不区分。
    if (m_width == 0) {
        if (!canvas.append(frame, 0, frame.height)) {
            match.heightLimited = true;
            return match;
        }
        m_canvasTop = 0;
        m_refTop = 0;
        m_bandsLocked = false;
        m_staticTop = 0;
        m_staticBottom = 0;
        clearAltReference();
        updateReference(frame, thumb);
        m_refTrusted = true;
        match.matched = true;
        return match;
    }

    // 客户区尺寸变了（窗口被缩放/移动、DPI 变化）：硬拼没有意义，按缺口重设基准。
    if (frame.width != m_width || frame.height != m_height) {
        m_canvasTop = 0;
        m_refTop = 0;
        m_bandsLocked = false;
        m_staticTop = 0;
        m_staticBottom = 0;
        clearAltReference();
        updateReference(frame, thumb);
        m_refTrusted = false;
        return match;
    }

    if (looksIdentical(thumb)) {
        match.duplicate = true;
        return match;
    }

    const StaticBands detected = detectStaticBands(m_currentSignature.rowHashes,
                                                   m_referenceSignature.rowHashes);

    // 已锁定的固定条优先：一旦判定过，后续帧都按同一套条带搜索，避免"这一帧
    // 恰好能对上 30 行"之类的偶然检测改变搜索区间、把位移带偏。
    const int searchTop = m_bandsLocked ? m_staticTop : detected.top;
    const int searchBottom = m_bandsLocked ? m_staticBottom : detected.bottom;
    const StitchEstimate direct =
        m_estimator.estimate(m_referenceSignature, m_currentSignature, searchTop, searchBottom);
    const bool directMatched =
        direct.score >= kMinMatchScore && direct.informative >= kMinInformativeRows;

    // 固定条只在"第一次出现非零位移"时判定一次，之后锁死：内容滚动之后再判定，
    // 会把恰好对上号的若干内容行误当固定条，裁掉画布 → 坐标整体错位（历史上表现为
    // 画布突然变矮、之后怎么滚都不再增长、图片里成段重复）。
    if (!m_bandsLocked && direct.delta != 0) {
        m_bandsLocked = true;
        m_staticTop = detected.top;
        m_staticBottom = detected.bottom;
        if (m_staticTop > 0) {
            canvas.trimTop(m_staticTop);
        }
        if (m_staticBottom > 0) {
            canvas.trimBottom(m_staticBottom);
        }
        m_canvasTop = 0;
        m_refTop = 0;
        // 备用基准若是在固定条锁定前保存的，它记录的条带是 0；锁定后画布顶部被裁掉、
        // 坐标归零（此时锚点只可能是首帧，refTop 本来就是 0），匹配时改用锁定后的条带。
        if (m_alt.valid) {
            m_alt.bandsTop = m_staticTop;
            m_alt.bandsBottom = m_staticBottom;
        }
    }
    const int bandsTop = m_bandsLocked ? m_staticTop : 0;
    const int bandsBottom = m_bandsLocked ? m_staticBottom : 0;
    const int movingHeight = frame.height - bandsTop - bandsBottom;

    // 备用基准在有效期内每帧都试。为什么不只在当前基准失配时试：缺口把基准坐标
    // 改成了"接在画布末尾"的估计值，用户小步回滚时直接匹配仍会成功（两帧之间本来
    // 就有重叠），但算出来的坐标是错的、会把已有内容重复贴一遍；备用基准保存的是
    // 缺口前的真实坐标，只要它过门槛就优先采纳。有效期见 kAltRefMaxAge。
    StitchEstimate estimate = direct;
    bool recovered = false;
    // 恢复时 m_refTop 会提前换成备用基准的坐标；若本帧最终因画布高度上限没有贴进去，
    // 要还原它，保持"m_refTop 对应当前基准帧"的不变量（会话随后会停止拼帧）。
    const std::int64_t refTopBeforeAlt = m_refTop;
    if (m_alt.valid) {
        const StitchEstimate altEstimate =
            m_estimator.estimate(m_alt.signature, m_currentSignature, m_alt.bandsTop,
                                 m_alt.bandsBottom);
        if (altEstimate.score >= kMinMatchScore &&
            altEstimate.informative >= kMinInformativeRows) {
            estimate = altEstimate;
            recovered = true;
            match.recovered = true;
            // 采纳备用基准：m_refTop 换成缺口前的可信坐标，本帧因此按"回到已知内容"
            // 处理，后面的坐标逻辑只在超出画布范围时才追加/前插。
            m_refTop = m_alt.refTop;
            m_refTrusted = true;
            clearAltReference();
        }
    }
    match.score = estimate.score;
    const std::int32_t delta = estimate.delta;

    if (!directMatched && !recovered) {
        // 缺口：不硬拼。先把坐标可信的当前基准留作备用基准（用户回滚时靠它恢复），
        // 再让基准前进到当前帧，内容坐标按"接在画布末尾"估一个值，让下一次匹配仍能
        // 算增量（否则坐标会永远停在上一次成功的位置）。
        rememberAltReference(bandsTop, bandsBottom);
        m_refTop = m_canvasTop + canvas.height();
        m_refTrusted = false;
        updateReference(frame, thumb);
        return match;
    }
    if (!recovered && delta == 0) {
        match.duplicate = true;  // 几乎没有新内容
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
            rememberAltReference(bandsTop, bandsBottom);
            m_refTop = m_canvasTop + canvas.height();
            m_refTrusted = false;
            updateReference(frame, thumb);
            return match;
        }
        if (!canvas.prepend(frame, bandsTop, rows)) {
            match.heightLimited = true;
            if (recovered) {
                m_refTop = refTopBeforeAlt;
            }
            return match;
        }
        m_canvasTop = newRefTop;
    } else if (newRefTop + movingHeight > canvasBottom) {
        const int rows = static_cast<int>(newRefTop + movingHeight - canvasBottom);
        if (rows > movingHeight) {
            rememberAltReference(bandsTop, bandsBottom);
            m_refTop = m_canvasTop + canvas.height();
            m_refTrusted = false;
            updateReference(frame, thumb);
            return match;
        }
        if (!canvas.append(frame, frame.height - bandsBottom - rows, rows)) {
            match.heightLimited = true;
            if (recovered) {
                m_refTop = refTopBeforeAlt;
            }
            return match;
        }
    }

    m_refTop = newRefTop;
    m_refTrusted = true;
    updateReference(frame, thumb);
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

void ScrollStitcher::updateReference(const CaptureFrame& frame,
                                     const std::vector<std::uint8_t>& thumb) {
    m_width = frame.width;
    m_height = frame.height;
    m_thumb = thumb;
    commitSignature();
}

void ScrollStitcher::commitSignature() noexcept {
    // 交换而不是拷贝：当前帧签名升级为参考帧，旧参考缓冲留给下一帧复用，
    // 避免每帧为几万字节的块均值矩阵重新分配内存。
    std::swap(m_referenceSignature, m_currentSignature);
}

bool ScrollStitcher::expireAltReference() noexcept {
    if (!m_alt.valid) {
        return false;
    }
    ++m_alt.frames;
    const bool tooOld = std::chrono::steady_clock::now() - m_alt.savedAt >= kAltRefMaxAge;
    if (!tooOld && m_alt.frames <= kAltRefMaxFrames) {
        return false;
    }
    clearAltReference();
    m_altDropped = true;
    return true;
}

bool ScrollStitcher::consumeAltReferenceDrop() noexcept {
    const bool dropped = m_altDropped;
    m_altDropped = false;
    return dropped;
}

void ScrollStitcher::rememberAltReference(int bandsTop, int bandsBottom) {
    if (!m_refTrusted) {
        // 当前基准的坐标本身就是缺口猜出来的：把它存成回滚锚点只会恢复出错坐标。
        // 已有备用基准时保留旧的那份——连续多个缺口时，它才是唯一带真实坐标的锚点。
        return;
    }
    m_alt.signature = m_referenceSignature;  // 缺口不常发生，这里复制一次可以接受。
    m_alt.refTop = m_refTop;
    m_alt.bandsTop = bandsTop;
    m_alt.bandsBottom = bandsBottom;
    m_alt.valid = true;
    m_alt.savedAt = std::chrono::steady_clock::now();
    m_alt.frames = 0;
}

void ScrollStitcher::clearAltReference() noexcept {
    m_alt.valid = false;
    m_alt.frames = 0;
}

}  // namespace long_capture
