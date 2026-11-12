// ScreenCast 门户的异步回调链：CreateSession/SelectSources → Start → 流信息 → fd。
//
// 所有函数都只在门户线程上执行（由 wayland_portal_session.cpp 的 portalThreadMain
// 发起）。libportal 0.7.x（Ubuntu 24.04）的流程：
//   xdp_portal_initable_new()
//     → xdp_portal_create_screencast_session(types=MONITOR, cursor=EMBEDDED,
//       persist=NONE)：内部做 CreateSession 与 SelectSources；
//     → xdp_session_start()：GNOME 在这里弹授权/选择框；
//     → xdp_session_get_streams() 取 node_id / size；
//     → xdp_session_open_pipewire_remote() 拿 PipeWire 远程 fd。
// 新版 libportal 把最后一步改名为 xdp_portal_open_pipewire_remote_for_screencast()
// 并提供了 XdpStream 对象；Ubuntu 24.04 只有会话级接口，这里按 0.7.x 写。
//
// 回调签名由 GLib/libportal 固定，只能是自由函数（STYLE.md §6.7 的合法例外）。

#include <mutex>
#include <string>
#include <utility>

#include <unistd.h>

#include <libportal/portal-helpers.h>
#include <libportal/portal.h>

#include "shot_plugin_helpers.h"
#include "wayland_portal_session_impl.h"

namespace screen_capture {
namespace {

/**
 * @brief 把门户的 GError 翻译成可行动的中文原因。
 *
 * 分类是需求要的"未授权 / 取消 / 门户不可用 / 其它"：错误码能判断的用错误码，
 * 判断不了的带上原始 message，方便真机定位（日志里不吞原因）。
 *
 * @param[in] step  失败步骤（如 "创建 ScreenCast 会话"）。
 * @param[in] error 门户返回的错误；可为 nullptr。
 * @return 中文原因。
 */
std::string describePortalError(const char* step, GError* error) {
    std::string text(step);
    text += "失败：";
    if (error == nullptr) {
        text += "门户没有给出原因";
        return text;
    }
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        text += "用户取消了共享（门户返回取消）";
        return text;
    }
    if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED)) {
        text += "未授权（门户拒绝共享屏幕）";
        return text;
    }
    if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
        g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER)) {
        text += "xdg-desktop-portal 服务不可用（没有运行，或桌面没有门户后端）";
        return text;
    }
    if (g_dbus_error_is_remote_error(error)) {
        gchar* remote = g_dbus_error_get_remote_error(error);
        if (g_strcmp0(remote, "org.freedesktop.portal.Error.Cancelled") == 0) {
            text += "用户取消了共享（门户返回取消）";
        } else if (g_strcmp0(remote, "org.freedesktop.portal.Error.NotAllowed") == 0 ||
                   g_strcmp0(remote, "org.freedesktop.portal.Error.PermissionDenied") == 0) {
            text += "未授权（门户拒绝共享屏幕）";
        } else {
            text += (remote != nullptr) ? remote : "门户返回 D-Bus 错误";
            text += "：";
            text += error->message;
        }
        g_free(remote);
        return text;
    }
    text += error->message;
    return text;
}

// ---- 下面的回调签名由 GLib/libportal 固定，只能是自由函数（STYLE.md §6.7 例外） ----

/** @brief 会话被合成器/用户关闭：只记录，采集层下次复用会话时发现并重建。 */
void onSessionClosed(XdpSession* /*session*/, gpointer data) {
    auto& impl = *static_cast<WaylandPortalSessionImpl*>(data);
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.closed = true;
        impl.closedReason = "门户会话已被关闭（用户撤销共享或输出被移除）";
    }
    shot::Logger(impl.host).warn("screen_capture: " + impl.closedReason +
                                 "；下一次截屏会重新建立会话");
}

/**
 * @brief Start 完成：解析流信息、打开 PipeWire 远程 fd，然后把会话置为 Ready。
 */
void onSessionStarted(GObject* source, GAsyncResult* result, gpointer data) {
    auto& impl = *static_cast<WaylandPortalSessionImpl*>(data);
    GError* error = nullptr;
    if (!xdp_session_start_finish(XDP_SESSION(source), result, &error)) {
        const std::string message = describePortalError("启动共享会话", error);
        g_clear_error(&error);
        shot::Logger(impl.host).error("screen_capture: " + message);
        markPortalFailed(impl, message);
        return;
    }

    // streams 的类型是 a(ua{sv})：第一项是 PipeWire node id，字典里有 "size" (ii)。
    // 只取第一个带尺寸的流（本插件请求的是单个输出，multiple=false）。
    GVariant* streams = xdp_session_get_streams(XDP_SESSION(source));
    guint32 nodeId = 0;
    gint32 width = 0;
    gint32 height = 0;
    bool found = false;
    if (streams != nullptr) {
        GVariantIter iter;
        g_variant_iter_init(&iter, streams);
        GVariant* properties = nullptr;
        guint32 candidate = 0;
        while (!found && g_variant_iter_next(&iter, "(u@a{sv})", &candidate, &properties)) {
            gint32 streamWidth = 0;
            gint32 streamHeight = 0;
            if (g_variant_lookup(properties, "size", "(ii)", &streamWidth, &streamHeight) &&
                streamWidth > 0 && streamHeight > 0) {
                nodeId = candidate;
                width = streamWidth;
                height = streamHeight;
                found = true;
            }
            g_variant_unref(properties);
        }
    }
    if (!found) {
        const std::string message = "门户没有返回可用的视频流（可能没有选中任何输出）";
        shot::Logger(impl.host).error("screen_capture: " + message);
        markPortalFailed(impl, message);
        return;
    }

    // Ubuntu 24.04 的 libportal 0.7.1 只有会话级接口；新版才有
    // xdp_portal_open_pipewire_remote_for_screencast()。
    const int fd = xdp_session_open_pipewire_remote(XDP_SESSION(source));
    if (fd < 0) {
        const std::string message = "无法打开门户的 PipeWire 远程连接（OpenPipeWireRemote 失败）";
        shot::Logger(impl.host).error("screen_capture: " + message);
        markPortalFailed(impl, message);
        return;
    }

    bool stopped = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.stopRequested) {
            stopped = true;
            impl.state = WaylandPortalSessionImpl::State::Failed;
            impl.message = "Wayland 门户会话在建立过程中被停止";
        } else {
            impl.pipewireFd = fd;
            impl.nodeId = nodeId;
            impl.width = width;
            impl.height = height;
            impl.state = WaylandPortalSessionImpl::State::Ready;
            impl.message.clear();
        }
    }
    if (stopped) {
        close(fd);
        impl.stateCv.notify_all();
        return;
    }
    impl.stateCv.notify_all();
    shot::Logger(impl.host).info(
        "screen_capture: Wayland 门户会话已启动（门户口径尺寸 " + std::to_string(width) + "x" +
        std::to_string(height) + "；光标按 embedded 模式交给合成器）");
}

/**
 * @brief CreateSession/SelectSources 完成：拿到会话后立刻发起 Start（弹授权框）。
 */
void onSessionCreated(GObject* source, GAsyncResult* result, gpointer data) {
    auto& impl = *static_cast<WaylandPortalSessionImpl*>(data);
    GError* error = nullptr;
    XdpSession* session =
        xdp_portal_create_screencast_session_finish(XDP_PORTAL(source), result, &error);
    if (session == nullptr) {
        const std::string message = describePortalError("创建 ScreenCast 会话", error);
        g_clear_error(&error);
        shot::Logger(impl.host).error("screen_capture: " + message);
        markPortalFailed(impl, message);
        return;
    }

    bool stopped = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.stopRequested) {
            stopped = true;
            impl.state = WaylandPortalSessionImpl::State::Failed;
            impl.message = "Wayland 门户会话在建立过程中被停止";
        } else {
            impl.session = session;
        }
    }
    if (stopped) {
        // 停止请求在会话创建期间到达：不启动、不弹框，让定时源退出主循环。
        g_object_unref(session);
        impl.stateCv.notify_all();
        return;
    }

    // 宿主没有可导出的父窗口（跨平台 ABI 不带窗口句柄），传 NULL：GNOME 仍会弹
    // 共享选择框，但可能不置顶到某个窗口前面。
    g_signal_connect(session, "closed", G_CALLBACK(onSessionClosed), &impl);
    xdp_session_start(session, nullptr, impl.cancellable, onSessionStarted, &impl);
}

}  // namespace

void markPortalFailed(WaylandPortalSessionImpl& impl, std::string message) {
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.state != WaylandPortalSessionImpl::State::Ready) {
            impl.state = WaylandPortalSessionImpl::State::Failed;
        }
        impl.message = std::move(message);
    }
    impl.stateCv.notify_all();
}

void startScreencastSession(WaylandPortalSessionImpl& impl) {
    GError* error = nullptr;
    XdpPortal* portal = xdp_portal_initable_new(&error);
    if (portal == nullptr) {
        std::string message = "无法连接 xdg-desktop-portal：";
        message += (error != nullptr) ? error->message : "未知错误";
        message += "（请确认桌面已安装并运行 xdg-desktop-portal 与后端，Ubuntu 包："
                   "xdg-desktop-portal xdg-desktop-portal-gnome）";
        g_clear_error(&error);
        shot::Logger(impl.host).error("screen_capture: " + message);
        markPortalFailed(impl, message);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.portal = portal;
    }
    // source = monitor / window 决定门户允许用户选择的源类型：门户的 types 是
    // "允许出现在选择框里的类型"位掩码，本插件按配置只允许一种，避免用户选到与
    // 配置不一致的源。
    const XdpOutputType outputs =
        impl.shareWindow ? XDP_OUTPUT_WINDOW : XDP_OUTPUT_MONITOR;
    xdp_portal_create_screencast_session(
        portal, outputs, XDP_SCREENCAST_FLAG_NONE, XDP_CURSOR_MODE_EMBEDDED,
        XDP_PERSIST_MODE_NONE, nullptr, impl.cancellable, onSessionCreated, &impl);
}

}  // namespace screen_capture
