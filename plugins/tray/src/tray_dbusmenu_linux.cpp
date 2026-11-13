// Linux 后端：com.canonical.dbusmenu 对象、菜单重建与点击分发。
//
// SNI 宿主（Ubuntu AppIndicators / KDE）不通过事件推送菜单，而是在图标被点击时
// 直接按 Menu 属性（/StatusNotifierItem/Menu）调用 dbusmenu：
//   · GetLayout：取整棵菜单树（本插件只有一层，root id 0 + 子项 id 1..N）；
//   · GetGroupProperties：本插件按协议允许返回空，客户端会用 GetLayout 的数据；
//   · Event(id, "clicked")：用户点了某一项，发布条目声明的事件；
//   · AboutToShow(id)：返回 false（布局总是现成的，不需要"即将显示"回调）。
//
// 菜单内容与 Windows 完全同源：TrayMenu 解析宿主列表服务里的 tray_command_list
// （解析/排序/兜底在 tray_menu.cpp，两个平台共用）。本文件只做 dbusmenu 渲染。
// 重建发生在服务线程上：宿主事件总线的 lists.changed 回调只入队 + 唤醒（见
// tray_icon_linux.cpp），因此这里不需要额外加锁，也不会和方法派发并发。

#include "tray_icon_linux_impl.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include <gio/gio.h>

#include "shot_plugin_helpers.h"

namespace tray {
namespace {

/**
 * @brief com.canonical.dbusmenu 的接口描述。
 *
 * 布局项类型是 `(ia{sv}av)`（id 是 int32，不是 uint32）——这是 dbusmenu 规范的
 * 公开签名，按 uint32 返回会让客户端的类型检查失败。
 * LayoutUpdated 是 (u revision, i parentId)：与 libdbusmenu 的 server.c 一致。
 */
constexpr char kMenuXml[] = R"xml(
<node>
  <interface name="com.canonical.dbusmenu">
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <signal name="LayoutUpdated">
      <arg name="revision" type="u"/>
      <arg name="parentId" type="i"/>
    </signal>
    <property name="Version" type="u" access="read"/>
    <property name="Status" type="s" access="read"/>
  </interface>
</node>
)xml";

/** @brief dbusmenu 版本：3（Canonical 的当前协议版本，AppIndicator 需要 ≥ 2）。 */
constexpr guint32 kMenuVersion = 3;

/** @brief 构造一个空的 `av`（布局项的子项数组）。 */
GVariant* emptyChildren() {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("av"));
    return g_variant_builder_end(&builder);
}

/**
 * @brief 把一个托盘条目转成 dbusmenu 布局项的属性字典（a{sv}）。
 *
 * 命令项：label（已由 TrayMenu 按 i18n 解析）、enabled/visible = true；
 * 分隔线：type = "separator"（客户端据此渲染分隔线而不是可点击项）。
 */
GVariant* itemProperties(const TrayCommandItem& item) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    if (item.separator) {
        g_variant_builder_add(&builder, "{sv}", "type", g_variant_new_string("separator"));
    } else {
        g_variant_builder_add(&builder, "{sv}", "label", g_variant_new_string(item.label.c_str()));
        g_variant_builder_add(&builder, "{sv}", "enabled", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&builder, "{sv}", "visible", g_variant_new_boolean(TRUE));
    }
    return g_variant_builder_end(&builder);
}

/** @brief 组装一个 dbusmenu 布局项 `(i a{sv} av)`；`props`/`children` 的所有权被接管。 */
GVariant* layoutItem(gint32 id, GVariant* props, GVariant* children) {
    return g_variant_new("(i@a{sv}@av)", id, props, children);
}

/**
 * @brief 按 GetLayout 的入参组装布局树。
 * @param[in]  menu           当前菜单模型（子项 id 从 1 开始，与 menuModel.items() 下标一一对应）。
 * @param[in]  parentId       GetLayout 的 parentId：0 = 根。
 * @param[in]  recursionDepth 0 只返回该节点本身，其它值（含 -1）返回全部子项。
 * @param[out] known          false = parentId 不存在（调用方回 D-Bus 错误）。
 * @return 布局项；未知 parentId 时返回 nullptr。
 */
GVariant* buildLayout(const TrayMenu& menu, gint32 parentId, gint32 recursionDepth, bool& known) {
    known = true;
    if (parentId == 0) {
        GVariantBuilder rootProps;
        g_variant_builder_init(&rootProps, G_VARIANT_TYPE("a{sv}"));
        // children-display=submenu：告诉客户端这个节点有子菜单，可以展开/弹出。
        g_variant_builder_add(&rootProps, "{sv}", "children-display",
                              g_variant_new_string("submenu"));
        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        if (recursionDepth != 0) {
            gint32 id = 1;
            for (const TrayCommandItem& item : menu.items()) {
                g_variant_builder_add(&children, "v",
                                      layoutItem(id, itemProperties(item), emptyChildren()));
                ++id;
            }
        }
        return layoutItem(0, g_variant_builder_end(&rootProps), g_variant_builder_end(&children));
    }
    // 本插件的菜单只有一层；客户端理论上可以按子项 id 取节点。
    const std::size_t index =
        (parentId > 0) ? static_cast<std::size_t>(parentId - 1) : static_cast<std::size_t>(-1);
    if (index >= menu.items().size()) {
        known = false;
        return nullptr;
    }
    return layoutItem(parentId, itemProperties(menu.items()[index]), emptyChildren());
}

}  // namespace

bool TrayIcon::Impl::registerMenuObject() {
    static const GDBusInterfaceVTable kVTable = {
        .method_call = &Impl::onMenuMethodCall,
        .get_property = &Impl::onMenuGetProperty,
        .set_property = nullptr,
    };
    const shot::Logger log(host);
    GError* error = nullptr;
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kMenuXml, &error);
    if (info == nullptr) {
        log.error("[tray] 解析 dbusmenu 接口描述失败：" + glibErrorText(error));
        if (error != nullptr) {
            g_error_free(error);
        }
        return false;
    }
    GDBusInterfaceInfo* iface = g_dbus_node_info_lookup_interface(info, kMenuInterface);
    if (iface == nullptr) {
        log.error("[tray] dbusmenu 接口描述里找不到 com.canonical.dbusmenu");
        g_dbus_node_info_unref(info);
        return false;
    }
    const guint registration =
        g_dbus_connection_register_object(connection, kMenuObjectPath, iface, &kVTable, this,
                                          nullptr, &error);
    if (registration == 0) {
        log.error("[tray] 注册 /StatusNotifierItem/Menu 对象失败：" + glibErrorText(error));
        if (error != nullptr) {
            g_error_free(error);
        }
        g_dbus_node_info_unref(info);
        return false;
    }
    menuRegistration = registration;
    menuNodeInfo = info;  // 注销对象后再 unref
    return true;
}

void TrayIcon::Impl::rebuildMenu(bool notifyHost) {
    TrayMenu fresh;
    try {
        // 从宿主列表服务取最新快照；列表不可用时 TrayMenu 会装本地兜底「退出」。
        // 读取是两段式只读快照（内部加锁），可以在本服务线程上调用。
        fresh.load(host, i18n);
    } catch (...) {
        // 分配失败等不能让异常穿出服务线程（std::terminate 会杀掉宿主）。
        shot::Logger(host).warn("[tray] 读取 tray_command_list 异常，保留旧菜单");
        return;
    }
    menuModel = std::move(fresh);
    ++menuRevision;

    // 把渲染结果写进日志：验收/排障时不用开桌面也能确认菜单里到底有什么。
    std::size_t commands = 0;
    std::size_t separators = 0;
    std::string summary;
    for (const TrayCommandItem& item : menuModel.items()) {
        if (item.separator) {
            ++separators;
            summary += " |";
        } else {
            ++commands;
            summary += " [" + item.label + "→" + item.event + "]";
        }
    }
    shot::Logger(host).info(
        "[tray] SNI 菜单已重建：" + std::to_string(commands) + " 个命令、" +
        std::to_string(separators) + " 条分隔线（来源 " +
        (menuModel.isFallback() ? "本地兜底" : "tray_command_list") + "）" + summary);

    if (!notifyHost || connection == nullptr) {
        return;
    }
    // 通知已经打开菜单的客户端重新取布局；signal 是广播，不指定 destination。
    GError* error = nullptr;
    g_dbus_connection_emit_signal(connection, nullptr, kMenuObjectPath, kMenuInterface,
                                  "LayoutUpdated",
                                  g_variant_new("(ui)", static_cast<guint32>(menuRevision),
                                                static_cast<gint32>(0)),
                                  &error);
    if (error != nullptr) {
        shot::Logger(host).warn("[tray] 发送 dbusmenu LayoutUpdated 失败：" + glibErrorText(error));
        g_error_free(error);
    }
}

const TrayCommandItem* TrayIcon::Impl::menuItem(gint32 id) const {
    if (id <= 0) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(id - 1);
    if (index >= menuModel.items().size()) {
        return nullptr;
    }
    const TrayCommandItem& item = menuModel.items()[index];
    // 分隔线有 id（布局里占一项）但不可点击：Event(clicked) 直接忽略。
    return item.separator ? nullptr : &item;
}

void TrayIcon::Impl::onMenuMethodCall(GDBusConnection* /*connection*/, const gchar* /*sender*/,
                                      const gchar* /*objectPath*/, const gchar* /*interfaceName*/,
                                      const gchar* methodName, GVariant* parameters,
                                      GDBusMethodInvocation* invocation, gpointer userData) {
    auto* impl = static_cast<Impl*>(userData);
    if (impl == nullptr || methodName == nullptr) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "tray: invalid dbusmenu call");
        return;
    }
    try {
        if (std::strcmp(methodName, "GetLayout") == 0) {
            gint32 parentId = 0;
            gint32 recursionDepth = -1;
            g_variant_get_child(parameters, 0, "i", &parentId);
            g_variant_get_child(parameters, 1, "i", &recursionDepth);
            bool known = false;
            GVariant* layout = buildLayout(impl->menuModel, parentId, recursionDepth, known);
            if (!known || layout == nullptr) {
                g_dbus_method_invocation_return_error_literal(
                    invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                    "tray: GetLayout parentId not found");
                return;
            }
            g_dbus_method_invocation_return_value(
                invocation,
                g_variant_new("(u@(ia{sv}av))", static_cast<guint32>(impl->menuRevision), layout));
            return;
        }
        if (std::strcmp(methodName, "GetGroupProperties") == 0) {
            // 协议允许返回空数组：客户端在 GetLayout 里已经拿到全部属性。
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
            GVariant* empty = g_variant_builder_end(&builder);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new_tuple(&empty, 1));
            return;
        }
        if (std::strcmp(methodName, "Event") == 0) {
            gint32 id = 0;
            const gchar* eventId = nullptr;
            g_variant_get_child(parameters, 0, "i", &id);
            g_variant_get_child(parameters, 1, "&s", &eventId);
            if (g_strcmp0(eventId, "clicked") == 0) {
                if (const TrayCommandItem* item = impl->menuItem(id)) {
                    impl->publishCommand(*item);
                }
            } else {
                // opened / closed / hovered 等事件不需要处理：菜单是纯静态数据。
                shot::Logger(impl->host).debug("[tray] 忽略 dbusmenu 事件：" +
                                               std::string((eventId != nullptr) ? eventId : ""));
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (std::strcmp(methodName, "AboutToShow") == 0) {
            // 布局总是预构建好的，无需"即将显示"更新；false = 客户端不用再等更新。
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
            return;
        }
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR,
                                                      G_DBUS_ERROR_UNKNOWN_METHOD,
                                                      "tray: unknown dbusmenu method");
    } catch (...) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "tray: dbusmenu call failed");
    }
}

GVariant* TrayIcon::Impl::onMenuGetProperty(GDBusConnection* /*connection*/,
                                            const gchar* /*sender*/, const gchar* /*objectPath*/,
                                            const gchar* /*interfaceName*/,
                                            const gchar* propertyName, GError** error,
                                            gpointer /*userData*/) {
    if (propertyName == nullptr) {
        if (error != nullptr) {
            g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
        }
        return nullptr;
    }
    if (std::strcmp(propertyName, "Version") == 0) {
        return g_variant_new_uint32(kMenuVersion);
    }
    if (std::strcmp(propertyName, "Status") == 0) {
        // "normal" = 菜单可用且没有需要注意的状态（offline / notice 是另外两个值）。
        return g_variant_new_string("normal");
    }
    if (error != nullptr) {
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s",
                    propertyName);
    }
    return nullptr;
}

void TrayIcon::Impl::publish(const char* topic) {
    // 托盘不直接调用别的插件：所有动作只通过事件总线广播（EVENT_BUS.md §1），
    // 发送方不知道谁在听，接收方不知道谁发的。
    // publish 是任意线程可调的（document/EVENT_BUS.md §3），这里在服务线程上调用。
    if (topic == nullptr) {
        return;
    }
    shot::EventBus bus(host);
    if (bus.publish(topic)) {
        // 日志写"动作"而不是"发布成功"：验收/排障时靠它确认哪个入口触发过。
        shot::Logger(host).info(std::string("[tray] 发布 ") + topic);
    } else {
        shot::Logger(host).warn(std::string("[tray] 发布失败（无事件总线或队列满）: ") + topic);
    }
}

void TrayIcon::Impl::publishCommand(const TrayCommandItem& item) {
    // 点击 = 发布条目声明的事件：托盘既不知道、也不关心是谁在听。
    if (item.event.empty()) {
        return;
    }
    shot::EventBus bus(host);
    if (bus.publish(item.event.c_str(), item.data)) {
        shot::Logger(host).info("[tray] 发布 " + item.event + "（菜单项 " + item.id + "）");
    } else {
        shot::Logger(host).warn("[tray] 发布失败（无事件总线或队列满）: " + item.event);
    }
}

}  // namespace tray
