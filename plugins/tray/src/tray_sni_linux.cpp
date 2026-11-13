// Linux 后端：org.kde.StatusNotifierItem 对象与 watcher 注册。
//
// SNI 是 Ubuntu GNOME（AppIndicator 扩展）/ KDE / Xfce 等桌面统一的托盘协议：
// 图标进程在会话总线上拥有 org.kde.StatusNotifierItem-<pid>-1，导出
// /StatusNotifierItem；托盘宿主（org.kde.StatusNotifierWatcher）收到注册后按
// 属性查询图标，并在用户点击时调用 Activate / ContextMenu，或直接读取 Menu 属性
// 指向的 com.canonical.dbusmenu 对象（菜单在 tray_dbusmenu_linux.cpp）。
//
// 本文件只做"图标本体"：属性、Activate 语义、向 watcher 注册；菜单的布局与
// 点击事件在另一个文件，两个对象共用同一条服务线程与 GDBus 连接。
//
// 图标用主题图标名 camera-photo-symbolic（IconName），不内嵌 IconPixmap：
// 随系统图标主题与深浅色切换，比固定位图更自然。`icon` 配置（.ico 文件）是
// Windows 后端的路径，Linux 忽略（README §5）。

#include "tray_icon_linux_impl.h"

#include <cstring>
#include <string>

#include <gio/gio.h>

#include "shot_plugin_helpers.h"

namespace tray {
namespace {

/**
 * @brief org.kde.StatusNotifierItem 的接口描述。
 *
 * 只声明本插件真正提供的属性/方法；IconPixmap、ToolTip、Attention*、Scroll 等
 * 可选成员不声明，宿主会按"没有该属性"处理并回退到 IconName/Title。
 * Activate 与 SecondaryActivate 都带 x/y（图标坐标）参数——协议固定，本插件
 * 用不到坐标，但签名必须一致，否则 GDBus 会以 InvalidArgs 拒绝调用。
 */
constexpr char kSniXml[] = R"xml(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
  </interface>
</node>
)xml";

}  // namespace

bool TrayIcon::Impl::registerSniObject() {
    // 函数内静态：vtable 只在首次注册时构造，且能访问 Impl 的静态回调（TrayIcon::Impl
    // 是私有嵌套类型，自由函数里无法命名它）。
    static const GDBusInterfaceVTable kVTable = {
        .method_call = &Impl::onSniMethodCall,
        .get_property = &Impl::onSniGetProperty,
        .set_property = nullptr,
    };
    const shot::Logger log(host);
    GError* error = nullptr;
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kSniXml, &error);
    if (info == nullptr) {
        log.error("[tray] 解析 StatusNotifierItem 接口描述失败：" + glibErrorText(error));
        if (error != nullptr) {
            g_error_free(error);
        }
        return false;
    }
    GDBusInterfaceInfo* iface = g_dbus_node_info_lookup_interface(info, kSniInterface);
    if (iface == nullptr) {
        log.error("[tray] StatusNotifierItem 接口描述里找不到 org.kde.StatusNotifierItem");
        g_dbus_node_info_unref(info);
        return false;
    }
    const guint registration =
        g_dbus_connection_register_object(connection, kSniObjectPath, iface, &kVTable, this,
                                          nullptr, &error);
    if (registration == 0) {
        log.error("[tray] 注册 /StatusNotifierItem 对象失败：" + glibErrorText(error));
        if (error != nullptr) {
            g_error_free(error);
        }
        g_dbus_node_info_unref(info);
        return false;
    }
    sniRegistration = registration;
    sniNodeInfo = info;  // 注销对象后再 unref（GDBus 注册期间会用它）
    return true;
}

void TrayIcon::Impl::registerWithWatcher() {
    if (connection == nullptr) {
        return;
    }
    // 异步注册 + 超时：没有 watcher 时不能阻塞服务线程，也不能反复重试刷屏。
    // 参数按 SNI 规范传"服务名"（也可以是对象路径）；传服务名时 watcher 会到
    // /StatusNotifierItem 上查询属性。
    g_dbus_connection_call(connection, kStatusNotifierWatcherName, kStatusNotifierWatcherPath,
                           kStatusNotifierWatcherInterface, "RegisterStatusNotifierItem",
                           g_variant_new("(s)", busName.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE,
                           kWatcherRegisterTimeoutMs, watcherCancellable,
                           &Impl::onWatcherRegisterDone, this);
}

void TrayIcon::Impl::onWatcherRegisterDone(GObject* source, GAsyncResult* result,
                                           gpointer userData) {
    auto* impl = static_cast<Impl*>(userData);
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply != nullptr) {
        g_variant_unref(reply);
    }
    if (error == nullptr) {
        if (impl != nullptr && !impl->stopping.load()) {
            shot::Logger(impl->host).info(
                "[tray] 已向 org.kde.StatusNotifierWatcher 注册托盘图标（GNOME 的 AppIndicator 扩展会显示它）");
        }
        return;
    }
    // stop() 取消在途调用会得到 CANCELLED：这是主动注销，不是错误，安静返回。
    const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    const std::string text = glibErrorText(error);
    g_error_free(error);
    if (cancelled || impl == nullptr || impl->stopping.load()) {
        return;
    }
    // 没有 SNI watcher 是正常的降级（纯 Wayland 无扩展、精简桌面、CI 环境）：
    // 只记一条 WARN 并保持插件可用；这条回调只会在注册时触发一次，不会刷屏。
    shot::Logger(impl->host).warn(
        "[tray] 没有可用的 SNI watcher（" + std::string(kStatusNotifierWatcherName) + "）：" +
        text +
        "；GNOME 需要在「扩展」里启用 AppIndicator/Ubuntu AppIndicators（Ubuntu 默认自带），"
        "托盘图标不会显示，插件其余功能不受影响");
}

void TrayIcon::Impl::onSniMethodCall(GDBusConnection* /*connection*/, const gchar* /*sender*/,
                                     const gchar* /*objectPath*/, const gchar* /*interfaceName*/,
                                     const gchar* methodName, GVariant* /*parameters*/,
                                     GDBusMethodInvocation* invocation, gpointer userData) {
    auto* impl = static_cast<Impl*>(userData);
    if (impl == nullptr || methodName == nullptr) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "tray: invalid StatusNotifierItem call");
        return;
    }
    try {
        if (std::strcmp(methodName, "Activate") == 0 ||
            std::strcmp(methodName, "SecondaryActivate") == 0) {
            // 主激活：列表里有 default 条目就执行它；没有（core 预置项都没有）
            // 就打开设置页。与 Windows 左键"默认命令"语义对齐，但 Linux 没有
            // "托盘双击打开主窗口"，设置页是更安全的主入口（截屏仍可从菜单触发）。
            impl->publishActivate();
        }
        // ContextMenu：菜单由 SNI 宿主直接调 dbusmenu 的 GetLayout，不需要动作；
        // 这里只回空回复（规范里它是方法调用，没有返回值）。
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } catch (...) {
        // 异常不能穿出 GDBus 的 C 回调帧；回一个错误让调用方看到失败而不是挂起。
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "tray: Activate failed");
    }
}

GVariant* TrayIcon::Impl::onSniGetProperty(GDBusConnection* /*connection*/, const gchar* /*sender*/,
                                           const gchar* /*objectPath*/,
                                           const gchar* /*interfaceName*/,
                                           const gchar* propertyName, GError** error,
                                           gpointer userData) {
    auto* impl = static_cast<Impl*>(userData);
    if (impl == nullptr || propertyName == nullptr) {
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
        return nullptr;
    }
    try {
        const std::string name = propertyName;
        // Category：应用类托盘项；ApplicationStatus 是"常驻状态图标"的语义。
        if (name == "Category") {
            return g_variant_new_string("ApplicationStatus");
        }
        if (name == "Id") {
            return g_variant_new_string(kAppId);
        }
        if (name == "Title") {
            // 悬停提示：与 Windows 的 NIF_TIP 用同一套文案 key；i18n 不可用时退回中文。
            const char* fallback = "Plugin-shot 托盘";
            const char* text =
                (impl->i18n != nullptr) ? impl->i18n->tr("tray.tooltip", fallback) : fallback;
            return g_variant_new_string((text != nullptr) ? text : fallback);
        }
        if (name == "Status") {
            return g_variant_new_string("Active");
        }
        if (name == "IconName") {
            return g_variant_new_string(kIconName);
        }
        if (name == "Menu") {
            return g_variant_new_object_path(kMenuObjectPath);
        }
        if (name == "ItemIsMenu") {
            // true = 图标只负责弹出菜单；GNOME/KDE 的 SNI 宿主会直接读取 Menu 属性。
            return g_variant_new_boolean(TRUE);
        }
    } catch (...) {
        // 解析/分配失败按"没有这个属性"返回，不把异常带出 GDBus 回调。
    }
    if (error != nullptr) {
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s",
                    propertyName);
    }
    return nullptr;
}

void TrayIcon::Impl::publishActivate() {
    // 与 Windows 左键的"默认命令"规则一致：列表里标了 default 的条目优先。
    for (const TrayCommandItem& item : menuModel.items()) {
        if (!item.separator && item.isDefault && !item.event.empty()) {
            shot::Logger(host).info("[tray] SNI 主激活：执行 default 命令 " + item.id);
            publishCommand(item);
            return;
        }
    }
    // core 预置项没有 default：Linux 的主激活打开设置页（非破坏性动作）。
    publish(kTopicSettingsOpen);
}

}  // namespace tray
