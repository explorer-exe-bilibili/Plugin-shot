// 一次长截图会话：等用户选目标 → HUD/气泡 → 自带采集 → 在线拼接 → 保存或取消。
//
// 采集路径（本版核心变化）：
//   用户先在 region_selector 里点选窗口/拖出区域，本插件收到 region.selected 后
//   用 ScreenGrabber 直接 BitBlt 目标区域。帧数据只进 ScrollStitcher/StitchCanvas，
//   **不再每帧发 capture.request、不再每帧落 PNG、不再每帧发 capture.done**；
//   只有最终保存时 saveCanvas() 才写一张 PNG 并发布一条 capture.done。
//
// 串行节流：相邻两帧至少间隔 kFrameIntervalMs（60ms）。这个间隔足够 5Hz HUD 刷新与
// 正常滚动速度，同时避免把 CPU 花在抓屏/拼接上。
//
// 线程：本文件所有函数都只在 LongCapture 的会话线程上执行；HUD/按键/事件回调都只
// 置标志 + 唤醒，不在这里直接跨线程调用窗口 API。

#include "long_capture.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "capture_windows.h"
#include "global_keys.h"
#include "hud.h"
#include "scroll_stitcher.h"
#include "shot_plugin_helpers.h"
#include "stitch_canvas.h"

namespace long_capture {
namespace {

// 只服务本文件的辅助函数与常量（STYLE.md §6.7）。

/** @brief 连续抓帧失败次数上限：达到后结束会话并提示。 */
constexpr int kMaxCaptureFailures = 3;

/** @brief 相邻两帧的最小间隔（毫秒）：串行节流，满足 5Hz HUD 与正常滚动速度。 */
constexpr int kFrameIntervalMs = 60;

/** @brief HUD 缩略图刷新间隔（毫秒）：需求是 5Hz。 */
constexpr int kHudRefreshMs = 200;

/** @brief 即使画面没静止，也至少按这个间隔送一次帧（毫秒）：避免快速滚动跨出重叠区。 */
constexpr int kMaxFeedIntervalMs = 100;

/** @brief 帧签名尺寸：48×48 灰度足够判断"这一帧变没变"，成本可以忽略。 */
constexpr int kSignatureWidth = 48;
constexpr int kSignatureHeight = 48;

/** @brief 两帧签名平均灰度差不超过它就算"画面没动"。 */
constexpr double kSignatureSameDiff = 1.5;

/**
 * @brief 把一帧缩成 48×48 灰度签名，用于判断画面是否静止。
 * @param[in] frame 已抓到的帧。
 * @return 签名；frame 为空时返回空 vector。
 */
std::vector<std::uint8_t> buildSignature(const CaptureFrame& frame) {
    std::vector<std::uint8_t> signature;
    if (frame.empty()) {
        return signature;
    }
    signature.resize(static_cast<std::size_t>(kSignatureWidth) *
                     static_cast<std::size_t>(kSignatureHeight));
    for (int sy = 0; sy < kSignatureHeight; ++sy) {
        const int y0 = sy * frame.height / kSignatureHeight;
        const int y1 = std::max(y0 + 1, (sy + 1) * frame.height / kSignatureHeight);
        for (int sx = 0; sx < kSignatureWidth; ++sx) {
            const int x0 = sx * frame.width / kSignatureWidth;
            const int x1 = std::max(x0 + 1, (sx + 1) * frame.width / kSignatureWidth);
            long long sum = 0;
            int count = 0;
            for (int y = y0; y < y1; ++y) {
                const std::uint8_t* row = frame.row(y);
                for (int x = x0; x < x1; ++x) {
                    const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
                    sum += (pixel[0] * 77 + pixel[1] * 150 + pixel[2] * 29) >> 8;
                    ++count;
                }
            }
            signature[static_cast<std::size_t>(sy) * kSignatureWidth +
                      static_cast<std::size_t>(sx)] =
                (count > 0) ? static_cast<std::uint8_t>(sum / count) : 0;
        }
    }
    return signature;
}

/**
 * @brief 两个签名是否表示"画面没动"。
 * @param[in] a 签名 A。
 * @param[in] b 签名 B；尺寸不同直接判不同。
 * @return true = 平均灰度差 <= kSignatureSameDiff。
 */
bool signaturesEqual(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) {
        return false;
    }
    long long difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    }
    return static_cast<double>(difference) / static_cast<double>(a.size()) <= kSignatureSameDiff;
}

/** @brief 把模板里的一个占位符替换成文本。 */
std::string replaceToken(std::string text, const char* token, const std::string& value) {
    if (token == nullptr || token[0] == '\0') {
        return text;
    }
    const std::string key(token);
    std::size_t position = text.find(key);
    while (position != std::string::npos) {
        text.replace(position, key.size(), value);
        position = text.find(key, position + value.size());
    }
    return text;
}

/**
 * @brief 会话期间的按键 RAII：任何提前返回/异常都要注销 Enter/Esc。
 *
 * 不用 std::scope_exit 是为了不引入 C++23 之外的库；结构很小，够用即可。
 */
struct SessionKeysGuard {
    GlobalKeys* keys = nullptr; /**< 可能为 nullptr（会话按键初始化失败）。 */

    ~SessionKeysGuard() {
        if (keys != nullptr) {
            keys->endSession();
        }
    }
};

}  // namespace

void LongCapture::runSession() {
    const LongCaptureConfig::Snapshot config = m_config.snapshot();
    const shot::Logger log(m_host);
    std::string error;

    ScreenRect content;
    if (!awaitSelection(content, error)) {
        // 超时/取消的提示已经在 awaitSelection 里发过；宿主退出时只记日志。
        log.info("[long_capture] 未开始会话：" + error);
        return;
    }
    log.info("[long_capture] 开始会话：内容区 " + std::to_string(content.width) + "x" +
             std::to_string(content.height) + " @" + std::to_string(content.x) + "," +
             std::to_string(content.y));
    // 先记下本次会话选中的算法：后续逐帧日志都带 alg=，用户贴日志即可判断是哪种
    // 判据出的问题（算法只在会话开始时取快照，会话中途改配置不影响本次）。
    const char* algorithmName = stitchAlgorithmName(config.algorithm);
    log.info(std::string("[long_capture] 位移估计算法：") + algorithmName +
             "（可选 hash|correlate|phase，见插件 README）");

    Hud hud;
    // targetHwnd = 0：选择结果只有区域，没有窗口句柄；HUD 的滚轮转发会自动跳过。
    // HUD 仍摆在内容区之外，放不下就不显示（由 Hud::start 内部判断）。
    const bool hudReady = hud.start(m_host, m_i18n.get(), content, 0, &LongCapture::onHudSave,
                                    &LongCapture::onHudCancel, this);
    if (hudReady) {
        hud.setStatus(localized("long.hud.waiting", "等待画面…"), "");
    } else {
        log.warn("[long_capture] HUD 不可用（内容区四周放不下或建窗失败），"
                 "降级为托盘气泡提示");
        showTrayHint("long.tray.hint_title", "长截图已开始", "long.tray.hint_text",
                     "请滚动窗口内容；Enter 保存 / Esc 取消");
    }

    if (m_keys) {
        m_keys->beginSession(&LongCapture::onSaveKey, &LongCapture::onCancelKey, this);
    }
    SessionKeysGuard keysGuard{m_keys.get()};

    std::unique_ptr<StitchCanvas> canvas;
    ScrollStitcher stitcher(config.algorithm);
    int frameCount = 0;
    int feedCount = 0;
    int gapCount = 0;
    int duplicateCount = 0;
    int failureCount = 0;
    // 静止判据：只在"当前帧与上一帧几乎一致、且与上次送拼的帧不同"时才拼接。
    std::vector<std::uint8_t> previousSignature;
    std::vector<std::uint8_t> lastFedSignature;
    auto lastFeedTime = std::chrono::steady_clock::now();
    bool failed = false;
    bool limitReached = false;
    auto lastHudRefresh = std::chrono::steady_clock::now() - std::chrono::milliseconds(kHudRefreshMs);

    const auto updateHud = [&]() {
        if (!hudReady || !canvas) {
            return;
        }
        std::string status =
            replaceToken(localized("long.hud.status", "帧 {frames} · 高度 {height} px"), "{frames}",
                         std::to_string(frameCount));
        status = replaceToken(status, "{height}", std::to_string(canvas->height()));
        std::string warning;
        if (limitReached) {
            warning = replaceToken(localized("long.hud.limit", "已达高度上限 {height} px，Enter 保存"),
                                   "{height}", std::to_string(canvas->height()));
        } else if (gapCount > 0) {
            warning = replaceToken(localized("long.hud.gaps", "缺口 {gaps} 处"), "{gaps}",
                                   std::to_string(gapCount));
        }
        if (duplicateCount > 0) {
            const std::string duplicates =
                replaceToken(localized("long.hud.duplicates", "重复帧 {duplicates}"),
                             "{duplicates}", std::to_string(duplicateCount));
            if (!warning.empty()) {
                warning += " · ";
            }
            warning += duplicates;
        }
        hud.setStatus(status, warning);
        // 缩略图来自自带采集拼出的画布（不再读流水线落盘的 PNG）；5Hz 刷新。
        std::vector<std::uint8_t> preview;
        int previewWidth = 0;
        int previewHeight = 0;
        if (canvas->buildPreview(240, 160, preview, previewWidth, previewHeight)) {
            hud.setThumbnail(preview.data(), previewWidth, previewHeight);
        }
    };

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_saveRequested || m_cancelRequested || !m_running) {
                break;
            }
        }

        const auto frameStart = std::chrono::steady_clock::now();
        CaptureFrame frame;
        std::string captureError;
        if (!ScreenGrabber::capture(content, frame, captureError)) {
            frame.release();
            ++failureCount;
            log.warn("[long_capture] 抓帧失败（" + std::to_string(failureCount) + "/" +
                     std::to_string(kMaxCaptureFailures) + "）：" + captureError);
            if (failureCount >= kMaxCaptureFailures) {
                error = "连续抓帧失败：" + captureError;
                failed = true;
                break;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - frameStart);
            const int remaining = kFrameIntervalMs - static_cast<int>(elapsed.count());
            if (remaining > 0 && waitCommand(remaining)) {
                break;
            }
            continue;
        }
        failureCount = 0;
        ++frameCount;

        if (!canvas) {
            canvas = std::make_unique<StitchCanvas>(frame.width, config.maxHeight);
        }
        if (frame.width != canvas->width()) {
            error = "目标区域宽度变化（" + std::to_string(canvas->width()) + " → " +
                    std::to_string(frame.width) + "），长截图已停止";
            failed = true;
            frame.release();
            break;
        }

        // 只在画面静止时送拼：平滑滚动的中间帧带亚像素抖动，位移估计会偏一两行，
        // 拼出来就是重复行/重影。等两帧几乎一致再送，位移是"停到停"的整数跳变，
        // 行哈希可以逐行精确比较。
        const std::vector<std::uint8_t> signature = buildSignature(frame);
        const bool settled =
            !previousSignature.empty() && signaturesEqual(signature, previousSignature);
        const bool changedSinceFed =
            lastFedSignature.empty() || !signaturesEqual(signature, lastFedSignature);
        const bool firstFrame = lastFedSignature.empty();
        // 快速滚动时"停到停"之间可能跨过整屏（缺口就是这么来的）：画面没静止但已经
        // 和上次送拼的帧不同，且超过 kMaxFeedIntervalMs，就按当前帧送一次。
        // 行哈希是精确判据，中间帧只要位移能对上就仍能拼；只有完全不动才跳过。
        const auto sinceFeed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastFeedTime);
        const bool forced = changedSinceFed &&
                            sinceFeed.count() >= static_cast<long long>(kMaxFeedIntervalMs);
        previousSignature = signature;
        if (!firstFrame && !((settled || forced) && changedSinceFed)) {
            frame.release();  // 画面还在动：这一轮不拼，等它停下来
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - frameStart);
            const int remaining = kFrameIntervalMs - static_cast<int>(elapsed.count());
            if (remaining > 0 && waitCommand(remaining)) {
                break;
            }
            continue;
        }
        lastFedSignature = signature;
        lastFeedTime = std::chrono::steady_clock::now();
        ++feedCount;

        const ScrollMatch match = stitcher.addFrame(frame, *canvas);
        frame.release();
        if (match.heightLimited) {
            limitReached = true;
        } else if (match.duplicate) {
            ++duplicateCount;
        } else if (!match.matched) {
            ++gapCount;
        }
        // 逐帧记录位移/分数/画布高度：拼接出问题时这段日志是唯一能离线复现的证据。
        // 只在送拼时记（约 5~10 条/秒），不影响性能；用户贴日志即可定位。
        if (match.matched) {
            log.info("[long_capture] 帧 " + std::to_string(feedCount) + "：alg=" + algorithmName +
                     " 位移 " + std::to_string(match.delta) + "，分数 " +
                     std::to_string(match.score) + "，画布 " +
                     std::to_string(canvas->height()));
        } else if (!match.duplicate && !match.heightLimited) {
            log.warn("[long_capture] 帧 " + std::to_string(feedCount) + "：alg=" + algorithmName +
                     " 缺口（分数 " + std::to_string(match.score) + "），画布 " +
                     std::to_string(canvas->height()));
        }

        const auto now = std::chrono::steady_clock::now();
        if (limitReached || now - lastHudRefresh >= std::chrono::milliseconds(kHudRefreshMs)) {
            updateHud();
            lastHudRefresh = now;
        }

        if (limitReached) {
            log.info("[long_capture] 画布已达高度上限 " + std::to_string(canvas->height()) +
                     " px，停止抓帧，等待保存/取消");
            // 停止抓帧，但保留 HUD/按键，等用户决定保存还是丢弃。
            while (!waitCommand(200)) {
            }
            break;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - frameStart);
        const int remaining = kFrameIntervalMs - static_cast<int>(elapsed.count());
        if (remaining > 0 && waitCommand(remaining)) {
            break;
        }
    }

    // 先注销 Enter/Esc：保存/失败提示的 2 秒展示期间不应该继续全局吞掉 Enter。
    if (m_keys) {
        m_keys->endSession();
    }
    keysGuard.keys = nullptr;

    bool saveRequested = false;
    bool cancelRequested = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        saveRequested = m_saveRequested;
        cancelRequested = m_cancelRequested || !m_running;
    }

    bool saved = false;
    if (!failed && !cancelRequested && saveRequested) {
        if (!canvas || canvas->empty()) {
            error = "没有拼接出任何内容";
            failed = true;
        } else {
            std::string path;
            std::string saveError;
            if (saveCanvas(*canvas, path, saveError)) {
                saved = true;
                log.info("[long_capture] 已保存长图：" + path + "（" +
                         std::to_string(canvas->width()) + "x" +
                         std::to_string(canvas->height()) + "，抓帧 " +
                         std::to_string(frameCount) + "，送拼 " + std::to_string(feedCount) +
                         "，重复 " + std::to_string(duplicateCount) + "，缺口 " +
                         std::to_string(gapCount) + "，算法 " + algorithmName + "）");
                if (hudReady) {
                    hud.showSaved(path);
                    // HUD 自己 2 秒后关闭；这里等它展示完，同时 stop() 能提前打断。
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(2100);
                    std::unique_lock<std::mutex> lock(m_mutex);
                    while (m_running && !m_cancelRequested &&
                           std::chrono::steady_clock::now() < deadline) {
                        m_cv.wait_until(lock, deadline);
                    }
                } else {
                    // HUD 不可用时用托盘气泡告知保存结果（路径作为正文原样显示）。
                    showTrayHint("long.tray.saved_title", "长截图已保存", nullptr, path.c_str());
                }
            } else {
                error = saveError;
                failed = true;
            }
        }
    }

    if (failed) {
        log.warn("[long_capture] 会话失败：" + error);
        if (hudReady) {
            hud.showFailure(error);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(2600);
            std::unique_lock<std::mutex> lock(m_mutex);
            while (m_running && !m_cancelRequested && std::chrono::steady_clock::now() < deadline) {
                m_cv.wait_until(lock, deadline);
            }
        } else {
            showTrayHint("long.tray.error_title", "长截图失败", nullptr, error.c_str());
        }
    } else if (!saved) {
        log.info("[long_capture] 会话已取消，画布未落盘");
        if (hudReady) {
            hud.setStatus(localized("long.hud.cancelled", "已取消"), "");
        }
    }

    hud.stop();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_saveRequested = false;
        m_cancelRequested = false;
        m_selectionReady = false;
        m_selectionCancelled = false;
        m_selectedRegion = ScreenRect{};
    }
}

}  // namespace long_capture
