#pragma once

// ============================================================================
//  include/tray_icon_linux_impl.h —— 内部头：Linux（StatusNotifierItem）后端的
//  TrayIcon::Impl、事件主题与 DBus 常量
//
//  **不是插件对外接口**：只有本插件的 Linux 版 .cpp 包含它（Windows 后端用
//  tray_icon_impl.h）。公共头 tray_icon.h 不出现任何 GLib/GDBus 类型。
//
//  线程模型（与 document/EVENT_BUS.md §3 一致）：
//    · 宿主线程（init / deinit / post_to_plugin / set_config / 事件总线回调）
//      永远不碰 GDBus 对象；事件总线的 lists.changed 回调只做"深拷贝入队 +
//      g_main_context_invoke_full 唤醒"；
//    · 一条插件自己的服务线程跑私有 GMainContext/GMainLoop：连接会话总线、
//      注册 SNI 与 dbusmenu 对象、派发方法调用、重建菜单全在这条线程上串行；
//    · stop() 在宿主线程退订总线 → 取消连接 → 把注销动作投到服务线程 →
//      join。服务线程返回前注销对象、释放公共名字、断开连接、退出主循环。
//
//  为什么用私有 GDBusConnection（而不是 g_bus_own_name 的 g_bus_get singleton）：
//  进程里 plugins/screen_capture（libportal）等也会用会话总线；g_bus_get 的
//  singleton 把消息派发固定挂在**第一次调用时的 thread-default 上下文**上，
//  谁的上下文先死谁就把别人（或自己）的回调弄丢。这里用
//  g_dbus_address_get_for_bus_sync + g_dbus_connection_new_for_address_sync 建
//  私有连接，派发上下文就是本插件的 GMainContext，与其它插件互不影响。
//
//  定义按职责拆成几个 .cpp：
//    tray_icon_linux.cpp    —— 生命周期 / 线程握手 / 总线连接 / lists.changed 队列
//    tray_sni_linux.cpp     —— org.kde.StatusNotifierItem 对象、属性、Activate、
//                              向 org.kde.StatusNotifierWatcher 注册
//    tray_dbusmenu_linux.cpp—— com.canonical.dbusmenu 对象、GetLayout/Event、
//                              菜单重建与 LayoutUpdated、点击发布事件
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <gio/gio.h>

#include "plugin_api.h"
#include "tray_icon.h"
#include "tray_menu.h"

namespace tray {

class TrayConfig;

// ---------------------------------------------------------------------------
//  事件主题 / 列表 id（与 Windows 后端、document/EVENT_BUS.md 完全一致）
// ---------------------------------------------------------------------------

/** @brief 订阅：宿主列表内容变化（负载 {"list":"<list id>"}）。 */
inline constexpr char kTopicListsChanged[] = "lists.changed";
/** @brief i18n_list 的列表 id：语言快照变化时菜单要按新语言重建。 */
inline constexpr char kI18nListId[] = "i18n_list";
/** @brief 宿主卸载 UI 插件前投递的事件（走 post_to_plugin，不走总线）。 */
inline constexpr char kEventHostShutdown[] = "host.shutdown";
/** @brief 发布：打开设置页（SNI Activate 的主激活动作）。 */
inline constexpr char kTopicSettingsOpen[] = "settings.open";

// ---------------------------------------------------------------------------
//  SNI / dbusmenu 的固定名字与对象路径（协议常量，不要改）
// ---------------------------------------------------------------------------

/** @brief 会话总线上要拥有的服务名前缀，完整名字是 <前缀><pid>-1。 */
inline constexpr char kStatusNotifierItemNamePrefix[] = "org.kde.StatusNotifierItem-";
/** @brief SNI 宿主（Ubuntu AppIndicators / KDE 托盘）的 watcher 服务名。 */
inline constexpr char kStatusNotifierWatcherName[] = "org.kde.StatusNotifierWatcher";
/** @brief watcher 的对象路径。 */
inline constexpr char kStatusNotifierWatcherPath[] = "/StatusNotifierWatcher";
/** @brief watcher 的接口名。 */
inline constexpr char kStatusNotifierWatcherInterface[] = "org.kde.StatusNotifierWatcher";
/** @brief 图标的对象路径：SNI 规范固定 /StatusNotifierItem。 */
inline constexpr char kSniObjectPath[] = "/StatusNotifierItem";
/** @brief 图标接口名。 */
inline constexpr char kSniInterface[] = "org.kde.StatusNotifierItem";
/** @brief 菜单的对象路径：Menu 属性指向它。 */
inline constexpr char kMenuObjectPath[] = "/StatusNotifierItem/Menu";
/** @brief 菜单接口名（Canonical dbusmenu）。 */
inline constexpr char kMenuInterface[] = "com.canonical.dbusmenu";
/** @brief SNI Id 属性：应用标识（不是总线名，规范要求稳定、不含空格）。 */
inline constexpr char kAppId[] = "plugin-shot";
/**
 * @brief SNI IconName：主题图标名。
 *
 * 托盘在 Linux 上**只给主题图标名**、不给 IconPixmap：Ubuntu GNOME 的
 * AppIndicator 扩展按 IconName 从当前图标主题取图，跟着系统主题/深浅色变化，
 * 比内置位图更自然；camera-photo-symbolic 是 GNOME 图标主题里稳定的相机图标。
 * `icon` 配置是 Windows 的 .ico 文件路径，Linux 后端忽略它（README §5 有说明）。
 */
inline constexpr char kIconName[] = "camera-photo-symbolic";
/** @brief 向 watcher 注册的超时（毫秒）：没有 watcher 时异步失败，不阻塞服务线程。 */
inline constexpr int kWatcherRegisterTimeoutMs = 2000;

/**
 * @brief 把 GError 转成可追加到中文日志里的文本。
 * @param[in] error 可能为 nullptr。
 * @return 错误文本；没有错误时返回"未知错误"。
 */
inline std::string glibErrorText(const GError* error) {
    return (error != nullptr && error->message != nullptr) ? std::string(error->message)
                                                           : std::string("未知错误");
}

/**
 * @brief TrayIcon 的 Linux 实现：GDBus 连接、SNI/dbusmenu 对象、服务线程、事件队列。
 *
 * 声明集中在这个结构里，定义按职责分散到三个 .cpp（STYLE.md §6.7 / §6.8）。
 * GDBus 的 C 回调签名由 GLib 固定，统一声明成 static 成员函数。
 */
struct TrayIcon::Impl {
    // ---- start() / stop() 跨线程共享的状态 ----
    std::thread thread;                    /**< 服务线程；joinable 表示 start 成功过。 */
    std::atomic<bool> acceptingEvents{false}; /**< stop() 一开始就置 false，总线回调不再入队。 */
    std::atomic<bool> stopping{false};     /**< true = 正在主动注销，name_lost 不算异常。 */
    bool busSubscribed = false;            /**< 是否成功订阅过事件总线（宿主线程序列访问）。 */

    const ShotHostServices* host = nullptr; /**< 宿主服务表；生命周期覆盖插件实例。 */
    TrayConfig* config = nullptr;           /**< 配置（Linux 只用 README 说明 icon 被忽略）。 */
    shot::I18n* i18n = nullptr;             /**< 多语言解析器；生命周期由 TrayState 持有。 */

    // ---- GLib / GDBus（只在服务线程上创建、使用、销毁；context 例外，见 stop） ----
    GMainContext* context = nullptr;        /**< 第一次 start 创建；活到 Impl 析构（可重复 start）。 */
    GMainLoop* loop = nullptr;              /**< 每次 start 新建；服务线程退出时 unref。 */
    GDBusConnection* connection = nullptr;  /**< 本插件私有的会话总线连接（不是 g_bus_get singleton）。 */
    GCancellable* connectCancellable = nullptr; /**< 让 stop() 能取消卡在连接阶段的线程。 */
    GCancellable* watcherCancellable = nullptr; /**< 取消向 watcher 注册的异步调用。 */
    guint busOwnerId = 0;                   /**< g_bus_own_name_on_connection 的句柄。 */
    guint sniRegistration = 0;              /**< /StatusNotifierItem 的对象注册句柄。 */
    guint menuRegistration = 0;             /**< /StatusNotifierItem/Menu 的对象注册句柄。 */
    GDBusNodeInfo* sniNodeInfo = nullptr;   /**< SNI 接口描述；注册期间 GDBus 会用，注销后 unref。 */
    GDBusNodeInfo* menuNodeInfo = nullptr;  /**< dbusmenu 接口描述；同上。 */
    bool shutdownDone = false;              /**< 服务线程上的注销流程是否已执行过（幂等标记）。 */
    std::string busName;                    /**< org.kde.StatusNotifierItem-<pid>-1。 */

    // ---- 菜单模型（只在服务线程上读写） ----
    TrayMenu menuModel;                     /**< tray_command_list 的解析结果（含本地兜底）。 */
    std::uint32_t menuRevision = 0;         /**< dbusmenu 布局版本号，每次重建 +1。 */

    // ---- lists.changed 的跨线程队列（宿主线程写 / 服务线程读） ----
    std::mutex queueMutex;                  /**< 保护 pendingPayloads / drainScheduled。 */
    std::queue<std::string> pendingPayloads;/**< 待处理的 lists.changed 负载（空串 = 任意列表）。 */
    bool drainScheduled = false;            /**< 已投递过一次唤醒源，避免同一轮重复投递。 */

    // ---- 生命周期 / 线程握手（tray_icon_linux.cpp） ----
    ~Impl();
    void threadMain();
    bool connectSessionBus();
    void shutdownOnServiceThread();
    void onNameAcquired(const char* name);
    void onNameLost(GDBusConnection* lostConnection, const char* name);

    /** @brief 事件总线回调：只把负载拷进队列并唤醒服务线程（绝不碰 GDBus 对象）。 */
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);
    /** @brief 把一条 lists.changed 负载入队；重复调用是安全的。 */
    void enqueueListsChanged(const char* data, std::uint32_t len);
    /** @brief 服务线程上的唤醒源：排空队列并按需重建菜单。 */
    static gboolean onDrainSource(gpointer userData);
    /** @brief 在服务线程上处理队列里的 lists.changed（过滤 + 重建）。 */
    void drainListsChanged();
    /** @brief g_bus_own_name_on_connection 的 name_acquired 回调。 */
    static void onNameAcquiredCallback(GDBusConnection* connection, const gchar* name,
                                       gpointer userData);
    /** @brief g_bus_own_name_on_connection 的 name_lost 回调。 */
    static void onNameLostCallback(GDBusConnection* connection, const gchar* name,
                                   gpointer userData);
    /** @brief stop() 投到服务线程的关闭源。 */
    static gboolean onShutdownSource(gpointer userData);

    // ---- SNI（tray_sni_linux.cpp） ----
    bool registerSniObject();
    void registerWithWatcher();
    /** @brief 向 watcher 异步注册；无论成功失败都只记一条日志（不重试、不刷屏）。 */
    static void onWatcherRegisterDone(GObject* source, GAsyncResult* result, gpointer userData);
    /** @brief org.kde.StatusNotifierItem 的方法调用（Activate / SecondaryActivate / ContextMenu）。 */
    static void onSniMethodCall(GDBusConnection* connection, const gchar* sender,
                                const gchar* objectPath, const gchar* interfaceName,
                                const gchar* methodName, GVariant* parameters,
                                GDBusMethodInvocation* invocation, gpointer userData);
    /** @brief org.kde.StatusNotifierItem 的只读属性。 */
    static GVariant* onSniGetProperty(GDBusConnection* connection, const gchar* sender,
                                      const gchar* objectPath, const gchar* interfaceName,
                                      const gchar* propertyName, GError** error,
                                      gpointer userData);
    /** @brief Activate 的语义：有 default 条目就执行它，否则打开设置页。 */
    void publishActivate();

    // ---- dbusmenu（tray_dbusmenu_linux.cpp） ----
    bool registerMenuObject();
    /**
     * @brief 从宿主列表服务重读 tray_command_list 并替换菜单模型。
     * @param[in] notifyHost true = 重建后向 dbusmenu 客户端发 LayoutUpdated 信号。
     * @note 只在服务线程上调用；读取列表快照本身是任意线程可调的只读操作。
     */
    void rebuildMenu(bool notifyHost);
    /** @brief 按 dbusmenu 的 item id（从 1 开始）取菜单条目；越界/分隔线返回 nullptr。 */
    const TrayCommandItem* menuItem(gint32 id) const;
    /** @brief com.canonical.dbusmenu 的方法调用（GetLayout / GetGroupProperties / Event / AboutToShow）。 */
    static void onMenuMethodCall(GDBusConnection* connection, const gchar* sender,
                                 const gchar* objectPath, const gchar* interfaceName,
                                 const gchar* methodName, GVariant* parameters,
                                 GDBusMethodInvocation* invocation, gpointer userData);
    /** @brief com.canonical.dbusmenu 的只读属性（Version / Status）。 */
    static GVariant* onMenuGetProperty(GDBusConnection* connection, const gchar* sender,
                                       const gchar* objectPath, const gchar* interfaceName,
                                       const gchar* propertyName, GError** error,
                                       gpointer userData);
    /** @brief 发布一个只带主题的事件（例如 settings.open）。 */
    void publish(const char* topic);
    /** @brief 点击菜单项：发布该条目声明的 event，负载 = data（原样）。 */
    void publishCommand(const TrayCommandItem& item);
};

}  // namespace tray
