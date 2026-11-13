// Linux 后端：TrayIcon 的生命周期、服务线程与事件投递（start / stop / post）。
//
// 线程模型（对应 document/EVENT_BUS.md §3）：
//   · 宿主线程只调 init / deinit / post_to_plugin 与 set_config，不接触任何 GDBus
//     对象；事件总线回调只做"深拷贝入队 + g_main_context_invoke_full 唤醒"；
//   · 服务线程跑私有 GMainContext/GMainLoop：连接会话总线、注册 SNI 与 dbusmenu
//     对象、派发方法调用、重建菜单全部串行在这条线程上；
//   · stop 顺序：先退订事件总线（deinit 返回后宿主不再回调）→ 拒绝新投递 →
//     取消连接/注册 → 把注销动作投到服务线程 → join。注销动作在服务线程上执行，
//     因为 GDBus 对象与注销必须共用同一个上下文（见 tray_icon_linux_impl.h 文件头）。
//
// 为什么不用 g_bus_own_name：它内部的 g_bus_get 是进程级 singleton，会把消息派发
// 固定在第一次使用时的 thread-default 上下文上；screen_capture 的 libportal 等也
// 用会话总线，互相抢 singleton 会丢回调。这里建本插件私有的 GDBusConnection。

#include "tray_icon.h"

#include <cstring>
#include <exception>
#include <mutex>
#include <queue>
#include <string>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "shot_plugin_helpers.h"
#include "tray_config.h"
#include "tray_icon_linux_impl.h"

namespace tray {

namespace {

/**
 * @brief 本插件私有的 GDBus 连接参数。
 *
 * AUTHENTICATION_CLIENT：按客户端身份认证；MESSAGE_BUS_CONNECTION：告诉 GDBus 这是
 * 消息总线连接（允许 g_bus_own_name_on_connection / RequestName 等总线操作）。
 */
constexpr GDBusConnectionFlags kConnectionFlags = static_cast<GDBusConnectionFlags>(
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION);

/**
 * @brief lists.changed 的负载是否与托盘菜单有关。
 *
 * 与 Windows 后端同一套过滤规则：只接受 `tray_command_list`（菜单增删）与
 * `i18n_list`（语言变化）；空负载 / 结构不认识时按"可能是任意列表"处理——
 * 宁可多重建一次，也不要漏掉本插件关心的刷新。
 *
 * @param[in] payload 事件负载（UTF-8 JSON 或裸 list id）。
 * @return true = 需要重建菜单。
 */
bool refersToTrayMenu(const std::string& payload) {
    if (payload.empty()) {
        return true;
    }
    const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, false);
    if (doc.is_object()) {
        const auto it = doc.find("list");
        if (it != doc.end() && it->is_string()) {
            const std::string list = it->get<std::string>();
            return list == kTrayCommandListId || list == kI18nListId;
        }
        return true;
    }
    // 不是 JSON：兼容"负载直接就是 list id"的约定，用子串匹配容忍引号/空白。
    return payload.find(kTrayCommandListId) != std::string::npos ||
           payload.find(kI18nListId) != std::string::npos;
}

}  // namespace

TrayIcon::TrayIcon() : m_impl(std::make_unique<Impl>()) {}

TrayIcon::Impl::~Impl() {
    // 服务线程已经由 stop() join；这里只释放活到 Impl 析构的 GLib 对象。
    // context 故意不在 stop() 里释放：同一对象可以 stop→start 反复启用，而
    // GDBus 连接/异步回调都绑定在这个上下文上（见头文件说明）。
    if (connectCancellable != nullptr) {
        g_object_unref(connectCancellable);
        connectCancellable = nullptr;
    }
    if (watcherCancellable != nullptr) {
        g_object_unref(watcherCancellable);
        watcherCancellable = nullptr;
    }
    if (context != nullptr) {
        g_main_context_unref(context);
        context = nullptr;
    }
}

TrayIcon::~TrayIcon() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool TrayIcon::start(const ShotHostServices* host, TrayConfig* config, shot::I18n* i18n) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->thread.joinable()) {
        return true;  // 已经在跑：重复 init 是空操作（宿主包装类本身也是幂等的）
    }
    m_impl->host = host;
    m_impl->config = config;
    m_impl->i18n = i18n;
    m_impl->acceptingEvents.store(true);
    m_impl->stopping.store(false);
    m_impl->shutdownDone = false;
    m_impl->menuRevision = 0;
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        std::queue<std::string>().swap(m_impl->pendingPayloads);
        m_impl->drainScheduled = false;
    }
    // Linux 的图标是主题图标名（IconName），不是文件：把 Windows 的 icon 配置
    // 在 Linux 上忽略的事实明确写进日志，免得用户以为设置页的图标选项坏了。
    if (config != nullptr) {
        const std::string iconPath = config->iconPath();
        if (!iconPath.empty()) {
            shot::Logger(host).warn(
                "[tray] Linux 使用主题图标 " + std::string(kIconName) +
                "（IconName），icon 配置的 .ico 文件在 Linux 上被忽略");
        }
    }

    // 先订阅再起线程：订阅之后到达的 lists.changed 会被队列接住，不会因为线程
    // 还没就绪而丢。
    shot::EventBus bus(host);
    if (bus.available()) {
        m_impl->busSubscribed =
            bus.subscribe(kTopicListsChanged, &Impl::onBusEvent, m_impl.get());
        if (!m_impl->busSubscribed) {
            shot::Logger(host).warn("[tray] 订阅 lists.changed 失败，菜单不会随列表变化刷新");
        }
    } else {
        // 旧版宿主没有事件总线：图标照常出现，只是菜单不会随运行期数据刷新。
        shot::Logger(host).warn("[tray] 宿主未提供事件总线（旧版宿主？），托盘只显示不刷新");
    }

    // 私有主上下文只在第一次 start 时创建，之后 stop→start 复用（见头文件）。
    if (m_impl->context == nullptr) {
        m_impl->context = g_main_context_new();
        if (m_impl->context == nullptr) {
            shot::Logger(host).warn("[tray] 创建 GLib 主上下文失败，Linux 托盘不可用");
            if (m_impl->busSubscribed) {
                shot::EventBus(host).unsubscribe(m_impl.get());
                m_impl->busSubscribed = false;
            }
            m_impl->acceptingEvents.store(false);
            return false;
        }
    }
    if (m_impl->connectCancellable == nullptr) {
        m_impl->connectCancellable = g_cancellable_new();
    }
    if (m_impl->watcherCancellable == nullptr) {
        m_impl->watcherCancellable = g_cancellable_new();
    }
    // 服务名带 pid：同一个会话里跑多个宿主实例也不会冲突（SNI 规范的约定）。
    m_impl->busName = std::string(kStatusNotifierItemNamePrefix) +
                      std::to_string(static_cast<long>(getpid())) + "-1";

    // 主循环在调用方线程创建、由服务线程 g_main_loop_run：这样"循环建不起来"是
    // start() 的同步失败，不会留下"线程已退出但 joinable"的中间态。
    m_impl->loop = g_main_loop_new(m_impl->context, FALSE);
    if (m_impl->loop == nullptr) {
        shot::Logger(host).warn("[tray] 创建 GLib 主循环失败，Linux 托盘不可用");
        if (m_impl->busSubscribed) {
            shot::EventBus(host).unsubscribe(m_impl.get());
            m_impl->busSubscribed = false;
        }
        m_impl->acceptingEvents.store(false);
        return false;
    }
    try {
        m_impl->thread = std::thread(&Impl::threadMain, m_impl.get());
    } catch (const std::exception& error) {
        // 线程资源耗尽等：只让托盘降级，不影响宿主与其它插件；异常不越过 ABI 边界。
        g_main_loop_unref(m_impl->loop);
        m_impl->loop = nullptr;
        if (m_impl->busSubscribed) {
            shot::EventBus(host).unsubscribe(m_impl.get());
            m_impl->busSubscribed = false;
        }
        m_impl->acceptingEvents.store(false);
        shot::Logger(host).warn(std::string("[tray] 创建 SNI 服务线程失败：") + error.what());
        return false;
    }
    shot::Logger(host).info("[tray] Linux SNI 后端启动：图标 " + std::string(kIconName) +
                            "，菜单来自 tray_command_list（服务名 " + m_impl->busName + "）");
    // 不在这里等 bus/name 就绪：注册是异步的（服务线程里 g_bus_own_name_on_connection），
    // 无桌面/无 watcher 时也不能阻塞宿主 init。
    return true;
}

void TrayIcon::stop() {
    if (!m_impl) {
        return;
    }
    // 1. 先退订事件总线：deinit 返回后宿主不再回调本插件（EVENT_BUS.md §5）。
    if (m_impl->busSubscribed) {
        shot::EventBus bus(m_impl->host);
        bus.unsubscribe(m_impl.get());
        m_impl->busSubscribed = false;
    }
    if (!m_impl->thread.joinable()) {
        m_impl->acceptingEvents.store(false);
        return;
    }
    // 2. 拒绝新投递；取消可能卡在"连接会话总线"阶段的线程。
    m_impl->acceptingEvents.store(false);
    m_impl->stopping.store(true);
    if (m_impl->connectCancellable != nullptr) {
        g_cancellable_cancel(m_impl->connectCancellable);
    }
    if (m_impl->watcherCancellable != nullptr) {
        g_cancellable_cancel(m_impl->watcherCancellable);
    }
    // 3. 注销动作投到服务线程（GDBus 对象只能在它自己的上下文上注销）。
    if (m_impl->context != nullptr) {
        g_main_context_invoke_full(m_impl->context, G_PRIORITY_HIGH, &Impl::onShutdownSource,
                                   m_impl.get(), nullptr);
    }
    // 4. join：返回时 SNI 已注销、公共名字已释放、DBus 连接已销毁。
    m_impl->thread.join();
    // 5. 服务线程已退出，取消用的 GCancellable 可以释放；下次 start 重新创建。
    if (m_impl->connectCancellable != nullptr) {
        g_object_unref(m_impl->connectCancellable);
        m_impl->connectCancellable = nullptr;
    }
    if (m_impl->watcherCancellable != nullptr) {
        g_object_unref(m_impl->watcherCancellable);
        m_impl->watcherCancellable = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        std::queue<std::string>().swap(m_impl->pendingPayloads);
        m_impl->drainScheduled = false;
    }
    shot::Logger(m_impl->host).info("[tray] SNI 已注销，D-Bus 服务线程已退出");
}

void TrayIcon::requestIconRefresh() {
    // Linux 的图标来自桌面图标主题（IconName），没有"换图标文件"这条路径；
    // 配置变化只影响 Windows。这里保持空操作，避免调用方以为失败。
}

std::int32_t TrayIcon::post(const char* name, const char* data, std::uint32_t len) {
    if (!m_impl || name == nullptr) {
        return -1;
    }
    // 宿主卸载 UI 插件前先投 host.shutdown 再 deinit（SETTINGS.md §3.1）：收到就
    // 立刻把线程停掉，不等 deinit——deinit 里的 stop() 因此变成幂等空操作。
    if (std::strcmp(name, kEventHostShutdown) == 0) {
        stop();
        return 0;
    }
    // 事件总线之外，宿主也可能直接 post 列表变化；走与事件总线同一条队列。
    if (std::strcmp(name, kTopicListsChanged) == 0) {
        if (!m_impl->acceptingEvents.load()) {
            return 1;  // 托盘不在（还没起好 / 正在关闭）：丢弃
        }
        try {
            m_impl->enqueueListsChanged(data, len);
            return 0;
        } catch (...) {
            return 2;  // 分配失败等：异常绝不跨越 DLL 边界
        }
    }
    // 其余主题 Linux 托盘不消费：返回 0（已接受但不处理）而不是报错，
    // 避免宿主把"托盘不关心这个事件"误判成投递失败。
    return m_impl->acceptingEvents.load() ? 0 : 1;
}

void TrayIcon::Impl::threadMain() {
    GMainContext* mainContext = context;
    GMainLoop* mainLoop = loop;
    if (mainContext == nullptr || mainLoop == nullptr) {
        return;  // start() 已建好两者才会起线程，这里只是兜底
    }
    // 只有把私有上下文设为 thread-default，GDBus 的异步回调、方法派发与
    // g_bus_own_name_on_connection 才会挂到它上面；顺序不能反过来。
    g_main_context_push_thread_default(mainContext);

    const bool connected = connectSessionBus();
    bool objectsReady = false;
    if (connected) {
        // 先注册对象、再向 watcher 注册服务名：watcher 收到 RegisterStatusNotifierItem
        // 后会立刻查询属性并调用 GetLayout，对象必须已经就绪。
        const bool sniReady = registerSniObject();
        const bool menuReady = registerMenuObject();
        objectsReady = sniReady && menuReady;
    }
    // 初始菜单模型：即使没有 D-Bus，读取 tray_command_list 也只会写一条日志，
    // 而且 stop() 之前排障日志里能看到菜单数据。
    rebuildMenu(false);
    if (objectsReady) {
        busOwnerId = g_bus_own_name_on_connection(
            connection, busName.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
            &Impl::onNameAcquiredCallback, &Impl::onNameLostCallback, this, nullptr);
        if (busOwnerId == 0) {
            shot::Logger(host).warn("[tray] 无法申请 SNI 服务名 " + busName + "，图标不会出现");
        }
    }

    g_main_loop_run(mainLoop);

    // 正常路径由 stop() 投递的 shutdown 完成；这里兜底，重复调用是空操作。
    shutdownOnServiceThread();
    g_main_loop_unref(mainLoop);
    loop = nullptr;
    g_main_context_pop_thread_default(mainContext);
}

bool TrayIcon::Impl::connectSessionBus() {
    const shot::Logger log(host);
    GError* error = nullptr;
    // 注意：这里用"地址 + 私有连接"，不是 g_bus_get 的进程级 singleton（见文件头）。
    gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, connectCancellable, &error);
    if (address == nullptr) {
        log.warn("[tray] 拿不到 D-Bus 会话总线地址（没有桌面会话？），Linux 托盘不可用：" +
                 glibErrorText(error) + "；插件其余功能不受影响");
        if (error != nullptr) {
            g_error_free(error);
        }
        return false;
    }
    GDBusConnection* conn = g_dbus_connection_new_for_address_sync(
        address, kConnectionFlags, nullptr, connectCancellable, &error);
    g_free(address);
    if (conn == nullptr) {
        log.warn("[tray] 连接 D-Bus 会话总线失败，Linux 托盘不可用：" + glibErrorText(error) +
                 "；插件其余功能不受影响");
        if (error != nullptr) {
            g_error_free(error);
        }
        return false;
    }
    // 会话总线断开时 GIO 默认 exit-on-close（raise SIGTERM 结束进程）。托盘只是
    // 可选 UI，不能因为总线断了就顺手把宿主杀掉；这里改为安静降级。
    g_dbus_connection_set_exit_on_close(conn, FALSE);
    connection = conn;  // 持有这一份引用，shutdown 时 unref
    return true;
}

void TrayIcon::Impl::shutdownOnServiceThread() {
    if (shutdownDone) {
        return;
    }
    shutdownDone = true;
    // 取消可能在途的 watcher 注册；回调会因 CANCELLED 安静返回。
    if (watcherCancellable != nullptr) {
        g_cancellable_cancel(watcherCancellable);
    }
    if (connection != nullptr) {
        if (sniRegistration != 0) {
            g_dbus_connection_unregister_object(connection, sniRegistration);
            sniRegistration = 0;
        }
        if (menuRegistration != 0) {
            g_dbus_connection_unregister_object(connection, menuRegistration);
            menuRegistration = 0;
        }
        // 注销对象后才能释放接口描述：GDBus 注册期间会用其中的 interface info。
        if (sniNodeInfo != nullptr) {
            g_dbus_node_info_unref(sniNodeInfo);
            sniNodeInfo = nullptr;
        }
        if (menuNodeInfo != nullptr) {
            g_dbus_node_info_unref(menuNodeInfo);
            menuNodeInfo = nullptr;
        }
        // 先释放公共名字（watcher 会看到 item 消失），再断开连接。
        if (busOwnerId != 0) {
            g_bus_unown_name(busOwnerId);
            busOwnerId = 0;
        }
        g_object_unref(connection);
        connection = nullptr;
    }
    if (loop != nullptr) {
        g_main_loop_quit(loop);
    }
}

void TrayIcon::Impl::onNameAcquired(const char* name) {
    shot::Logger(host).info("[tray] SNI 服务名已就绪：" +
                            std::string((name != nullptr) ? name : busName.c_str()) +
                            "（对象 " + kSniObjectPath + "，菜单 " + kMenuObjectPath + "）");
    registerWithWatcher();
}

void TrayIcon::Impl::onNameLost(GDBusConnection* lostConnection, const char* name) {
    if (stopping.load()) {
        return;  // 主动注销过程中的 name_lost 不是异常
    }
    std::string message = "[tray] SNI 服务名丢失：";
    message += (name != nullptr) ? name : busName;
    // GBusNameLostCallback 的约定：connection == NULL 表示连接建立失败或会话总线已断开；
    // 非空表示连接还在、只是名称拿不到（另一个进程占着它）。
    message += (lostConnection == nullptr)
                   ? "（无法连接会话总线或连接已断开）"
                   : "（名称被另一个进程占用）";
    message += "；托盘图标不会显示，插件其余功能不受影响";
    shot::Logger(host).warn(message);
}

void TrayIcon::Impl::onNameAcquiredCallback(GDBusConnection* /*connection*/, const gchar* name,
                                            gpointer userData) {
    if (auto* impl = static_cast<Impl*>(userData)) {
        impl->onNameAcquired(name);
    }
}

void TrayIcon::Impl::onNameLostCallback(GDBusConnection* connection, const gchar* name,
                                        gpointer userData) {
    if (auto* impl = static_cast<Impl*>(userData)) {
        impl->onNameLost(connection, name);
    }
}

gboolean TrayIcon::Impl::onShutdownSource(gpointer userData) {
    if (auto* impl = static_cast<Impl*>(userData)) {
        impl->shutdownOnServiceThread();
    }
    return G_SOURCE_REMOVE;
}

void TrayIcon::Impl::onBusEvent(void* user, const char* topic, const void* data,
                                std::uint32_t len) {
    auto* self = static_cast<Impl*>(user);
    if (self == nullptr || topic == nullptr) {
        return;
    }
    if (std::strcmp(topic, kTopicListsChanged) != 0) {
        return;  // 只订阅了 lists.changed，这里防御未来加订阅时漏过滤
    }
    // 回调运行在宿主分发线程上：只做深拷贝 + 唤醒服务线程，不碰任何 GDBus 对象。
    // 回调是跨 DLL 的 C 边界，异常绝不能穿过去（STYLE.md §6.2）：分配失败就丢掉这条事件。
    try {
        self->enqueueListsChanged(static_cast<const char*>(data), len);
    } catch (...) {
        shot::Logger(self->host).warn("[tray] 接收 lists.changed 失败（内存不足？），已丢弃该事件");
    }
}

void TrayIcon::Impl::enqueueListsChanged(const char* data, std::uint32_t len) {
    if (!acceptingEvents.load()) {
        return;
    }
    bool needWake = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        pendingPayloads.emplace((data != nullptr && len > 0) ? std::string(data, len) : std::string());
        if (!drainScheduled) {
            drainScheduled = true;
            needWake = true;
        }
    }
    if (needWake && context != nullptr) {
        // 从宿主线程投唤醒源：上下文归服务线程所有，所以不会在调用线程上直接执行。
        g_main_context_invoke_full(context, G_PRIORITY_DEFAULT, &Impl::onDrainSource, this, nullptr);
    }
}

gboolean TrayIcon::Impl::onDrainSource(gpointer userData) {
    if (auto* impl = static_cast<Impl*>(userData)) {
        impl->drainListsChanged();
    }
    return G_SOURCE_REMOVE;
}

void TrayIcon::Impl::drainListsChanged() {
    std::queue<std::string> payloads;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        payloads.swap(pendingPayloads);
        drainScheduled = false;
    }
    if (!acceptingEvents.load()) {
        return;  // 正在关闭：菜单不再重建
    }
    bool rebuild = false;
    while (!payloads.empty()) {
        if (refersToTrayMenu(payloads.front())) {
            rebuild = true;
        }
        payloads.pop();
    }
    if (!rebuild) {
        shot::Logger(host).debug("[tray] lists.changed 与托盘菜单无关，忽略");
        return;
    }
    rebuildMenu(true);
}

}  // namespace tray
