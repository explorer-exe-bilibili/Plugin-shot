// region_portal —— xdg-desktop-portal 的 Screenshot 门户调用（Linux/Wayland 分支）。
//
// 为什么必须走门户：Wayland 客户端拿不到屏幕内容（没有 X11 的 XGetImage，也没有
// Win32 的 BitBlt），截图由合成器/门户后端完成，首次还会弹系统授权框。这是需求
// "方案 B" 的第一步：只截一次屏，之后所有操作都在我们自己的窗口里完成。
//
// 实现要点：
//  · libportal 的 API 是异步的（回调 + GMainLoop），而本插件需要在 UI 线程上得到
//    "有/没有图片"的确定结果，所以这里临时建一个线程私有的 GMainContext + GMainLoop，
//    阻塞到回调、超时或中断为止。不碰宿主主循环，也不依赖 GLFW 的事件循环；
//  · 超时用一个 GLib timeout source 实现：到点先取消 D-Bus 请求，再退出主循环，
//    避免门户一直挂着把覆盖层永远挡在"准备阶段"；超时值见 region_portal.h 的
//    kPortalTimeoutSeconds（30s，真机上 120s 会被用户当成"点了没反应"）；
//  · 等待期间每 kPortalWaitLogSeconds 秒写一条 info（"仍在等待 portal 授权…"）：
//    真机日志里"region_selector 没有任何日志"最常见的原因就是这段静默等待；
//  · 门户返回的是 `file://` URI，转成本地路径交给上层，由上层用 stb 解码后删除
//    （临时文件属于调用方，门户不会替我们清理）。
//
// 日志：用户取消/拒绝只记 debug（需求：取消静默）；门户不可用/失败/超时记 WARN。
// 日志固定中文，不进语言表（e2e 按中文日志断言）。

#include "region_portal.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <gio/gio.h>
#include <glib.h>
#include <libportal/portal.h>
// xdp_portal_new() 在 0.7.x 里只在 portal-helpers.h 声明（portal.h 是汇总头，不含它）；
// 两个头都有 #pragma once，重复包含无副作用。Ubuntu 24.04 = libportal 0.7.1。
#include <libportal/portal-helpers.h>

#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

/** @brief 中断检查的轮询间隔（毫秒）：宿主退出/热卸载时不必等满超时。 */
constexpr guint kAbortPollMs = 100;

/** @brief 一次异步调用在"发起 → 回调 / 超时 / 中断"之间传递的状态（只在本 TU 内使用）。 */
struct PortalCall {
    GMainLoop* loop = nullptr;        /**< 本线程私有的主循环（回调里 quit）。 */
    GCancellable* cancellable = nullptr; /**< 超时/中断后取消 D-Bus 请求。 */
    const std::function<bool()>* shouldAbort = nullptr; /**< 调用方的中断检查（可空）。 */
    const ShotHostServices* host = nullptr; /**< 日志目标（可空）；进度日志要用。 */
    guint waitedSeconds = 0;          /**< 已经等了多少秒：进度日志里显示。 */
    gboolean timedOut = FALSE;        /**< 是否是超时结束的。 */
    gboolean aborted = FALSE;         /**< 是否是调用方要求中断（宿主退出等）。 */
    char* uri = nullptr;              /**< 成功时的 `file://` URI（g_free 释放）。 */
    GError* error = nullptr;          /**< 失败原因（g_error_free 释放）。 */
};

/** @brief 异步回调：xdp_portal_take_screenshot_finish 的结果写进 PortalCall 并退出主循环。 */
void onScreenshotDone(GObject* source, GAsyncResult* result, gpointer data) {
    auto* call = static_cast<PortalCall*>(data);
    if (call == nullptr) {
        return;
    }
    // libportal 0.6+ 的 finish 直接返回 `file://` URI（成功）或 nullptr（失败，错误在 error）。
    GError* error = nullptr;
    char* uri = xdp_portal_take_screenshot_finish(XDP_PORTAL(source), result, &error);
    if (uri != nullptr) {
        call->uri = uri;
    } else if (error != nullptr) {
        call->error = error;
    }
    // 无论成功失败都退出主循环：调用方只关心"有没有结果"。
    if (call->loop != nullptr) {
        g_main_loop_quit(call->loop);
    }
}

/** @brief 超时回调：标记超时、取消请求，再退出主循环。 */
gboolean onScreenshotTimeout(gpointer data) {
    auto* call = static_cast<PortalCall*>(data);
    if (call != nullptr) {
        call->timedOut = TRUE;
        if (call->cancellable != nullptr) {
            g_cancellable_cancel(call->cancellable);
        }
        if (call->loop != nullptr) {
            g_main_loop_quit(call->loop);
        }
    }
    return G_SOURCE_REMOVE;
}

/**
 * @brief 中断轮询：调用方（宿主退出 / 热卸载）要求放弃时取消请求并退出主循环。
 *
 * 否则 stop() 的 join 会被门户授权框卡住，最长要等满 kPortalTimeoutSeconds。
 */
gboolean onAbortPoll(gpointer data) {
    auto* call = static_cast<PortalCall*>(data);
    if (call == nullptr || call->shouldAbort == nullptr) {
        return G_SOURCE_CONTINUE;
    }
    if (!(*call->shouldAbort)()) {
        return G_SOURCE_CONTINUE;
    }
    call->aborted = TRUE;
    if (call->cancellable != nullptr) {
        g_cancellable_cancel(call->cancellable);
    }
    if (call->loop != nullptr) {
        g_main_loop_quit(call->loop);
    }
    return G_SOURCE_REMOVE;
}

/**
 * @brief 进度日志：等待授权期间每 kPortalWaitLogSeconds 秒写一条 info。
 *
 * 为什么非要这个：门户授权框是系统窗口，用户可能根本没看见（点到了别的窗口后面），
 * 而"点了区域截屏之后没有任何 region_selector 日志"最常见的原因就是这段静默等待。
 * 有了这几行，真机日志能一眼看出"卡在等授权"还是"门户已返回"。
 */
gboolean onWaitProgress(gpointer data) {
    auto* call = static_cast<PortalCall*>(data);
    if (call == nullptr) {
        return G_SOURCE_REMOVE;
    }
    call->waitedSeconds += static_cast<guint>(kPortalWaitLogSeconds);
    shot::Logger(call->host).info(
        "region_selector：仍在等待 portal 授权…（已等待 " + std::to_string(call->waitedSeconds) +
        "s，最多 " + std::to_string(kPortalTimeoutSeconds) + "s）");
    return G_SOURCE_CONTINUE;
}

/** @brief GError 的消息文本（可能为空），用于拼日志。 */
const char* errorText(const GError* error) {
    return (error != nullptr && error->message != nullptr) ? error->message : "未知原因";
}

/**
 * @brief 错误是不是"用户取消 / 拒绝授权"。
 *
 * 不同版本的 portal 与 libportal 对同一个用户动作给出的错误域不一样（G_IO 的
 * CANCELLED、DBus 的远程错误、或 portal 自己拼的消息），所以除了标准匹配之外再
 * 看一眼消息文本。判错的代价只是日志级别，不影响功能。
 */
bool isCancellation(const GError* error) {
    if (error == nullptr) {
        return false;
    }
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        return true;
    }
    if (error->message == nullptr) {
        return false;
    }
    char* lower = g_ascii_strdown(error->message, -1);
    const bool cancelled =
        std::strstr(lower, "cancel") != nullptr || std::strstr(lower, "denied") != nullptr ||
        std::strstr(lower, "dismiss") != nullptr || std::strstr(lower, "rejected") != nullptr;
    g_free(lower);
    return cancelled;
}

/**
 * @brief 错误是不是"门户服务不可用"。
 *
 * 典型场景：没装 xdg-desktop-portal、桌面后端没有实现 Screenshot、D-Bus 连不上。
 * 与其它失败相比只是日志措辞不同（都是 WARN + 不发事件），但真机排查时这行很关键。
 */
bool isServiceUnavailable(const GError* error) {
    if (error == nullptr) {
        return false;
    }
    return g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
           g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
           g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY) ||
           g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) ||
           g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

}  // namespace

PortalScreenshot::PortalScreenshot(const ShotHostServices* host) : m_host(host) {}

PortalScreenshotStatus PortalScreenshot::take(std::string& pngPath,
                                              const std::function<bool()>& shouldAbort) const {
    pngPath.clear();
    const shot::Logger log(m_host);

    // 本函数跑在插件的 UI 线程上：给 GLib 一个线程私有的 GMainContext，既不碰宿主
    // 的主循环，也不会被默认上下文里的其它异步任务干扰。
    GMainContext* context = g_main_context_new();
    g_main_context_push_thread_default(context);

    PortalScreenshotStatus status = PortalScreenshotStatus::Failed;
    XdpPortal* portal = xdp_portal_new();
    if (portal == nullptr) {
        log.warn("region_selector：xdg-desktop-portal 不可用（libportal 无法创建门户代理）");
        status = PortalScreenshotStatus::Unavailable;
    } else {
        PortalCall call;
        call.loop = g_main_loop_new(context, FALSE);
        call.cancellable = g_cancellable_new();
        call.shouldAbort = &shouldAbort;
        call.host = m_host;
        GSource* timeout = g_timeout_source_new_seconds(kPortalTimeoutSeconds);
        g_source_set_callback(timeout, &onScreenshotTimeout, &call, nullptr);
        g_source_attach(timeout, context);
        // 进度日志：没有它，等待授权的几十秒在日志里是一片空白（真机排查时误判成"没反应"）。
        GSource* progress = g_timeout_source_new_seconds(kPortalWaitLogSeconds);
        g_source_set_callback(progress, &onWaitProgress, &call, nullptr);
        g_source_attach(progress, context);
        // 中断轮询：宿主退出/热卸载时 stop() 会 join 本线程，不能让授权框卡住它。
        GSource* abortPoll = g_timeout_source_new(kAbortPollMs);
        g_source_set_callback(abortPoll, &onAbortPoll, &call, nullptr);
        g_source_attach(abortPoll, context);

        // 非交互模式（flags = NONE）= 整屏截图；parent = NULL：本插件没有可以当门户
        // 对话框父窗口的稳定窗口句柄（覆盖层此时还没显示）。
        log.debug("region_selector：已经发出 portal 截图请求，等待门户回调");
        xdp_portal_take_screenshot(portal, nullptr, XDP_SCREENSHOT_FLAG_NONE,
                                   call.cancellable, &onScreenshotDone, &call);
        g_main_loop_run(call.loop);

        // 超时/中断退出时，取消产生的完成回调可能还挂在上下文里：走完这些小任务，
        // 避免把挂起的 D-Bus 回复带出本函数（回调只写栈上的 call，跑完更干净）。
        for (int guard = 0; guard < 8 && g_main_context_pending(context) != FALSE; ++guard) {
            g_main_context_iteration(context, FALSE);
        }

        if (call.aborted) {
            // 宿主正在退出：安静放弃（这不是用户的取消动作，但也不能再发事件）。
            status = PortalScreenshotStatus::Cancelled;
            log.debug("region_selector：宿主正在退出，放弃等待门户截图授权");
        } else if (call.timedOut) {
            status = PortalScreenshotStatus::TimedOut;
            log.warn("region_selector：portal 截图超时（等 " +
                     std::to_string(kPortalTimeoutSeconds) +
                     "s 仍没有拿到授权/截图，已取消门户请求），按取消处理本次框选");
        } else if (call.uri != nullptr) {
            GError* uriError = nullptr;
            char* localPath = g_filename_from_uri(call.uri, nullptr, &uriError);
            if (localPath == nullptr) {
                status = PortalScreenshotStatus::Failed;
                log.warn(std::string("region_selector：门户返回的 URI 无法转成本地路径：") +
                         errorText(uriError));
            } else {
                pngPath = localPath;
                g_free(localPath);
                status = PortalScreenshotStatus::Ok;
                log.info("region_selector：xdg-desktop-portal 截图已返回 " + pngPath);
            }
            if (uriError != nullptr) {
                g_error_free(uriError);
            }
        } else if (call.error != nullptr) {
            if (isCancellation(call.error)) {
                // 需求：用户取消静默——不弹窗、不发事件，只在 debug 里留痕。
                status = PortalScreenshotStatus::Cancelled;
                log.debug("region_selector：用户取消/拒绝了门户截图授权");
            } else if (isServiceUnavailable(call.error)) {
                status = PortalScreenshotStatus::Unavailable;
                log.warn(std::string("region_selector：xdg-desktop-portal 截图门户不可用：") +
                         errorText(call.error));
            } else {
                status = PortalScreenshotStatus::Failed;
                log.warn(std::string("region_selector：xdg-desktop-portal 截图失败：") +
                         errorText(call.error));
            }
        } else {
            status = PortalScreenshotStatus::Failed;
            log.warn("region_selector：xdg-desktop-portal 截图没有返回文件，也没有返回错误");
        }

        g_source_destroy(abortPoll);
        g_source_unref(abortPoll);
        g_source_destroy(progress);
        g_source_unref(progress);
        g_source_destroy(timeout);
        g_source_unref(timeout);
        if (call.uri != nullptr) {
            g_free(call.uri);
        }
        if (call.error != nullptr) {
            g_error_free(call.error);
        }
        g_object_unref(call.cancellable);
        g_main_loop_unref(call.loop);
        g_object_unref(portal);
    }

    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    return status;
}

bool isWaylandSession() {
    // WAYLAND_DISPLAY 是 Wayland 会话最直接的证据；XDG_SESSION_TYPE 由 display
    // manager 设置。有些精简环境只有其中一个，任一命中就按 Wayland 处理。
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (waylandDisplay != nullptr && waylandDisplay[0] != '\0') {
        return true;
    }
    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType == nullptr) {
        return false;
    }
    // 少数发行版写成 "Wayland"：不区分大小写比较。
    return g_ascii_strcasecmp(sessionType, "wayland") == 0;
}

}  // namespace region_selector
