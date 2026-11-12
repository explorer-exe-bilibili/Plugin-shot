// 一次长截图会话：找窗口 → HUD/气泡 → 连续抓帧 → 在线拼接 → 保存或取消。
//
// 采集路径（需求约定的 P1 方案）：
//   每帧发布一条带 region 的 capture.request，等宿主流水线回 capture.done，再用
//   FrameLoader 从落盘文件解码。这样做的好处是完全复用宿主的采集/保存链路；
//   代价是每帧都会在 image_saver 的目录里产生一个临时文件（读完立即删除），并且
//   每帧的 capture.done 也会被其它订阅者（notify/clipboard）看到——见 README 的
//   "已知问题"，后续如果要彻底解决，需要插件自带的采集层（直接调用 screen_capture）。
//
// 串行节流：相邻两帧至少间隔 kFrameIntervalMs（60ms）。宿主流水线本身是串行的，
// 这个间隔足够 5Hz HUD 刷新与正常滚动速度，同时避免把 CPU 花在抓屏/编码/解码上。
//
// TODO: 改成插件自带采集层（直接调用 screen_capture 能力）以消除"每帧临时文件"与
//       "每帧 capture.done 被 notify/clipboard 看到"这两个副作用；那需要新增一个
//       私有事件或直接读像素，不在本期 P1 范围。
//
// 线程：本文件所有函数都只在 LongCapture 的会话线程上执行；HUD/按键/事件回调都只
// 置标志 + 唤醒，不在这里直接跨线程调用窗口 API。

#include "long_capture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "frame_loader.h"
#include "global_keys.h"
#include "hud.h"
#include "scroll_stitcher.h"
#include "shot_plugin_helpers.h"
#include "stitch_canvas.h"

namespace long_capture {
namespace {

// 只服务本文件的辅助函数与常量（STYLE.md §6.7）。

/** @brief 抓帧结果的最长等待：5 秒还没结果说明流水线卡住或缺少采集/保存插件。 */
constexpr int kCaptureTimeoutMs = 5000;

/** @brief 连续超时次数上限：达到后结束会话并提示。 */
constexpr int kMaxCaptureTimeouts = 3;

/** @brief 相邻两帧的最小间隔（毫秒）：串行节流，满足 5Hz HUD 与正常滚动速度。 */
constexpr int kFrameIntervalMs = 60;

/** @brief HUD 缩略图刷新间隔（毫秒）：需求是 5Hz。 */
constexpr int kHudRefreshMs = 200;

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

/** @brief 把 UTF-8 文本转成 filesystem::path（Windows 上内部转宽字符）。 */
std::filesystem::path fromUtf8Path(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const std::u8string text(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(text);
}

/**
 * @brief 删除本帧的临时截图文件（读完后立即调用）。
 *
 * 为什么删：P1 走宿主流水线，每帧都会在 image_saver 配置的目录里落一个文件；
 * 不删的话一次长会话会在用户目录里留下几十上百张中间帧。删除失败只记 DEBUG：
 * 文件可能被别的消费者（notify/clipboard）短暂占用，下一次会话不影响功能。
 *
 * @param[in] utf8Path capture.done 给出的路径。
 * @param[in] host     宿主服务表，只用于日志；可为 nullptr。
 */
void removeTemporaryFile(const std::string& utf8Path, const ShotHostServices* host) {
    if (utf8Path.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(fromUtf8Path(utf8Path), error);
    if (error) {
        shot::Logger(host).debug("[long_capture] 删除临时帧文件失败：" + utf8Path);
    }
}

/**
 * @brief 会话期间的按键 RAII：任何提前返回/异常都要注销 Enter/Esc。
 *
 * 不用 std::scope_exit 是为了不引入 C++23 之外的库；结构很小，够用即可。
 */
struct SessionKeysGuard {
    GlobalKeys* keys = nullptr; /**< 可能为 nullptr（全局按键初始化失败）。 */

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

    TargetWindow target;
    if (!resolveTarget(target, error)) {
        log.warn("[long_capture] 开始失败：" + error);
        showTrayHint("long.tray.failed_title", "长截图未能开始", nullptr, error.c_str());
        return;
    }
    log.info("[long_capture] 开始会话：客户区 " + std::to_string(target.client.width) + "x" +
             std::to_string(target.client.height) + " @" + std::to_string(target.client.x) + "," +
             std::to_string(target.client.y));

    Hud hud;
    const bool hudReady =
        hud.start(m_host, m_i18n.get(), target.client, target.handle, &LongCapture::onHudSave,
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
    ScrollStitcher stitcher;
    int frameCount = 0;
    int gapCount = 0;
    int duplicateCount = 0;
    int timeoutCount = 0;
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

        clearCaptureResult();
        if (!publishCaptureRequest(target.client)) {
            error = "发布采集请求失败（宿主事件队列满？）";
            failed = true;
            break;
        }

        CaptureResult result;
        if (!waitCapture(result, kCaptureTimeoutMs)) {
            bool command = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                command = m_saveRequested || m_cancelRequested || !m_running;
            }
            if (command) {
                break;
            }
            ++timeoutCount;
            log.warn("[long_capture] 等待抓帧结果超时（" + std::to_string(timeoutCount) + "/" +
                     std::to_string(kMaxCaptureTimeouts) + "）");
            if (timeoutCount >= kMaxCaptureTimeouts) {
                error = "等待抓帧结果超时：请确认 screen_capture / image_saver 插件已启用";
                failed = true;
                break;
            }
            continue;
        }
        timeoutCount = 0;

        if (result.failed) {
            error = result.reason.empty() ? "采集失败（宿主未给出原因）" : result.reason;
            failed = true;
            break;
        }

        const auto frameStart = std::chrono::steady_clock::now();
        CaptureFrame frame;
        std::string loadError;
        const bool loaded = FrameLoader::load(result.path, frame, loadError);
        // 无论解码成功与否，读完就删：中间帧不属于用户的截图结果。
        removeTemporaryFile(result.path, m_host);
        if (!loaded) {
            error = loadError;
            failed = true;
            break;
        }
        ++frameCount;

        if (!canvas) {
            canvas = std::make_unique<StitchCanvas>(frame.width, config.maxHeight);
        }
        if (frame.width != canvas->width()) {
            error = "目标窗口宽度变化（" + std::to_string(canvas->width()) + " → " +
                    std::to_string(frame.width) + "），长截图已停止";
            failed = true;
            frame.release();
            break;
        }

        const ScrollMatch match = stitcher.addFrame(frame, *canvas);
        frame.release();
        if (match.heightLimited) {
            limitReached = true;
        } else if (match.duplicate) {
            ++duplicateCount;
        } else if (!match.matched) {
            ++gapCount;
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
                         std::to_string(canvas->height()) + "，帧 " +
                         std::to_string(frameCount) + "，缺口 " + std::to_string(gapCount) + "）");
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
        m_captureReady = false;
        m_capture = CaptureResult{};
    }
}

}  // namespace long_capture
