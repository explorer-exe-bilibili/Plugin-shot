// D-Bus 会话服务的生命周期：专用线程 + 私有 GMainContext + g_bus_own_name。
//
// 宿主没有 GLib 主循环，所以这里自己开一条线程跑 GMainLoop：只有上下文被迭代时，
// GDBus 的 bus_acquired / name_acquired / name_lost 与方法调用才会派发。注册仍是
// 异步的（g_bus_own_name 立即返回），start() 不阻塞调用方。
//
// 关闭顺序（保证 DLL 卸载后不再有回调）：
//   stop() 在调用方线程 → g_main_context_invoke_full 把 shutdown 动作投到服务线程
//   → 服务线程 unregister object / g_bus_unown_name / g_main_loop_quit
//   → threadMain 收尾 → join 返回。私有 GMainContext **不在这里释放**：g_bus_own_name
//   内部用的会话连接是 g_bus_get 的进程级 singleton，它把消息派发固定挂在创建时的
//   thread-default 上下文上；把上下文留到对象析构，重新启用（配置 enabled 关闭再打开）
//   时还能继续用同一条连接，否则新上下文永远收不到方法调用。
// 注销动作必须在服务线程上执行：GDBus 的方法派发与注销共用同一个上下文，换线程
// 直接调 unregister/unown 会与在途回调竞争。

#include "control_service.h"

#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <gio/gio.h>

#include "control_service_impl.h"
#include "shot_plugin_helpers.h"

namespace session_control {

namespace {

/**
 * @brief 把 GError 转成可追加到中文日志里的文本。
 * @param[in] error 可能为 nullptr。
 * @return 错误文本；没有错误时返回"未知错误"。
 * @note 只在本文件用，属于匿名命名空间的小工具（STYLE §6.7）。
 */
std::string errorText(const GError* error) {
    return (error != nullptr && error->message != nullptr) ? std::string(error->message)
                                                           : std::string("未知错误");
}

/** @brief 服务线程上执行的关闭源：注销并退出循环。 */
gboolean handleShutdown(gpointer user_data) {
    auto* impl = static_cast<detail::ControlServiceImpl*>(user_data);
    if (impl != nullptr) {
        impl->shutdownOnServiceThread();
    }
    return G_SOURCE_REMOVE;
}

/** @brief g_bus_own_name 的 bus_acquired 回调：转给实现对象。 */
void busAcquiredCallback(GDBusConnection* connection, const gchar* /*name*/, gpointer user_data) {
    auto* impl = static_cast<detail::ControlServiceImpl*>(user_data);
    if (impl != nullptr) {
        impl->onBusAcquired(connection);
    }
}

/** @brief g_bus_own_name 的 name_acquired 回调：转给实现对象。 */
void nameAcquiredCallback(GDBusConnection* /*connection*/, const gchar* name, gpointer user_data) {
    auto* impl = static_cast<detail::ControlServiceImpl*>(user_data);
    if (impl != nullptr) {
        impl->onNameAcquired(name);
    }
}

/** @brief g_bus_own_name 的 name_lost 回调：转给实现对象。 */
void nameLostCallback(GDBusConnection* connection, const gchar* name, gpointer user_data) {
    auto* impl = static_cast<detail::ControlServiceImpl*>(user_data);
    if (impl != nullptr) {
        impl->onNameLost(connection, name);
    }
}

}  // namespace

ControlService::ControlService() : m_impl(std::make_unique<detail::ControlServiceImpl>()) {}

ControlService::~ControlService() { stop(); }

detail::ControlServiceImpl::~ControlServiceImpl() {
    // stop() 已经把服务线程 join 掉；这里只负责最后一次释放私有上下文。
    // 上下文故意活到对象析构：它同时是 g_bus_get 会话连接 singleton 的派发上下文，
    // 重新启用时要继续用（见文件头）。
    if (context != nullptr) {
        g_main_context_unref(context);
        context = nullptr;
    }
}

bool ControlService::start(const ShotHostServices* host) {
    std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
    if (m_impl->thread.joinable()) {
        return true;  // 已在运行：重复 start 是空操作
    }
    if (host == nullptr) {
        return false;
    }
    m_impl->host = host;
    m_impl->stopping = false;
    // 上下文只在第一次 start 时创建，stop() 不释放（见文件头的 singleton 说明）。
    if (m_impl->context == nullptr) {
        m_impl->context = g_main_context_new();
        if (m_impl->context == nullptr) {
            shot::Logger(m_impl->host).warn(
                "[session_control] 创建 GLib 主上下文失败，D-Bus 入口不可用");
            return false;
        }
    }
    // 主循环在调用方线程创建、由服务线程 g_main_loop_run：这样"循环建不起来"是
    // start() 的同步失败，不会留下"线程已退出但 joinable"的中间态；stop() 投递的
    // 关闭源也一定能被这条线程派发到（线程剩下的动作只有 push 上下文 + own_name + run）。
    m_impl->loop = g_main_loop_new(m_impl->context, FALSE);
    if (m_impl->loop == nullptr) {
        shot::Logger(m_impl->host).warn(
            "[session_control] 创建 GLib 主循环失败，D-Bus 入口不可用");
        return false;
    }
    // 先置 true 再建线程：线程创建失败时由下面的 catch 清回 false。
    m_impl->runningFlag.store(true);
    try {
        m_impl->thread = std::thread(&detail::ControlServiceImpl::threadMain, m_impl.get());
    } catch (const std::exception& error) {
        // 线程创建失败（资源耗尽等）只让本插件降级，不影响宿主与其它插件。
        m_impl->runningFlag.store(false);
        g_main_loop_unref(m_impl->loop);
        m_impl->loop = nullptr;
        shot::Logger(m_impl->host).warn(
            std::string("[session_control] 创建 D-Bus 服务线程失败：") + error.what());
        return false;
    }
    return true;
}

void ControlService::stop() {
    std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
    if (!m_impl->thread.joinable()) {
        m_impl->runningFlag.store(false);
        return;
    }
    // 关闭动作投递到服务线程：它持有 GDBus 的注册句柄，只能在自己的上下文上注销。
    if (m_impl->context != nullptr) {
        g_main_context_invoke_full(m_impl->context, G_PRIORITY_HIGH, &handleShutdown,
                                   m_impl.get(), nullptr);
    }
    m_impl->thread.join();
    m_impl->runningFlag.store(false);
    m_impl->host = nullptr;
}

bool ControlService::running() const noexcept { return m_impl->runningFlag.load(); }

// ControlServiceImpl 定义在 session_control::detail 里（PIMPL 内部类型，公共头只
// 暴露不透明门面），所以它的成员函数必须在这个命名空间里定义，否则编译器会把
// ControlServiceImpl 当成 session_control 下的另一个类型（"has not been declared"）。
namespace detail {

void ControlServiceImpl::threadMain() {
    GMainContext* mainContext = context;
    GMainLoop* mainLoop = loop;
    if (mainContext == nullptr || mainLoop == nullptr) {
        return;  // start() 已建好两者才会起线程，这里只是兜底
    }
    // 只有把私有上下文设为 thread-default，g_bus_own_name 的异步回调与 GDBus 的
    // 方法派发才会挂到它上面；顺序不能反过来（先 own 再 push 会落到默认上下文）。
    g_main_context_push_thread_default(mainContext);

    // g_bus_own_name 内部用的是 g_bus_get 的会话连接 singleton：它的消息派发上下文
    // 就是调用时的 thread-default，也就是本插件的私有上下文。宿主进程目前没有别的
    // GLib/GDBus 使用者，所以不存在抢占；将来若有别的插件也用 GDBus，要么接受回调
    // 落在本线程上，要么各自用独立连接（g_dbus_connection_new），不要依赖全局默认上下文。
    // TODO: 要和别的 GLib/D-Bus 插件共存时，改用私有连接（g_dbus_connection_new_for_address
    //       + g_bus_own_name_on_connection），把进程级会话连接 singleton 留给别人。
    ownerId = g_bus_own_name(G_BUS_TYPE_SESSION, kBusName, G_BUS_NAME_OWNER_FLAGS_NONE,
                             &busAcquiredCallback, &nameAcquiredCallback, &nameLostCallback, this,
                             nullptr);
    g_main_loop_run(mainLoop);

    // 正常路径由 stop() 投递的 shutdown 完成；这里兜底，重复调用是空操作。
    shutdownOnServiceThread();
    g_main_loop_unref(mainLoop);
    loop = nullptr;
    g_main_context_pop_thread_default(mainContext);
}

void ControlServiceImpl::shutdownOnServiceThread() {
    stopping = true;
    if (registrationId != 0) {
        if (connection != nullptr && !g_dbus_connection_is_closed(connection)) {
            g_dbus_connection_unregister_object(connection, registrationId);
        }
        registrationId = 0;
    }
    // 注销对象后才能释放接口描述：GDBus 注册期间会用其中的 interface info。
    if (nodeInfo != nullptr) {
        g_dbus_node_info_unref(nodeInfo);
        nodeInfo = nullptr;
    }
    if (connection != nullptr) {
        g_object_unref(connection);
        connection = nullptr;
    }
    if (ownerId != 0) {
        g_bus_unown_name(ownerId);
        ownerId = 0;
    }
    if (loop != nullptr) {
        g_main_loop_quit(loop);
    }
}

void ControlServiceImpl::onBusAcquired(GDBusConnection* busConnection) {
    if (busConnection == nullptr) {
        return;
    }
    const shot::Logger log(host);
    // GIO 的会话连接默认 exit-on-close：总线一断就 raise(SIGTERM) 结束整个进程。本插件
    // 只是一个可选的触发入口，名称丢了应当降级，不能顺手把宿主杀掉；宿主自己的退出
    // 由它的主循环决定（会话真正结束时 logind/会话管理器会收走进程，不靠这条）。
    g_dbus_connection_set_exit_on_close(busConnection, FALSE);
    GError* error = nullptr;
    GDBusNodeInfo* parsed = g_dbus_node_info_new_for_xml(introspectionXml(), &error);
    if (parsed == nullptr) {
        log.warn(std::string("[session_control] 解析 D-Bus 接口描述失败：") + errorText(error));
        if (error != nullptr) {
            g_error_free(error);
        }
        return;
    }
    // g_dbus_connection_register_object 的形参是 GDBusInterfaceInfo*（非 const），
    // 不能写成 const 指针；GDBus 注册期间会自己 ref 一份，注销对象后才释放。
    GDBusInterfaceInfo* iface = g_dbus_node_info_lookup_interface(parsed, kInterfaceName);
    if (iface == nullptr) {
        log.warn("[session_control] D-Bus 接口描述里找不到 org.pluginshot.Control");
        g_dbus_node_info_unref(parsed);
        return;
    }
    const guint registration = g_dbus_connection_register_object(
        busConnection, kObjectPath, iface, &methodVTable(), this, nullptr, &error);
    if (registration == 0) {
        log.warn(std::string("[session_control] 注册 D-Bus 对象失败：") + errorText(error));
        if (error != nullptr) {
            g_error_free(error);
        }
        g_dbus_node_info_unref(parsed);
        return;
    }
    connection = static_cast<GDBusConnection*>(g_object_ref(busConnection));
    registrationId = registration;
    nodeInfo = parsed;  // 注销对象后再 unref；持有引用比依赖 GDBus 内部复制更稳
    log.debug("[session_control] D-Bus 对象已注册：/org/pluginshot/Control");
}

void ControlServiceImpl::onNameAcquired(const char* name) {
    std::string message = "[session_control] 已获取 D-Bus 名称 ";
    message += (name != nullptr) ? name : kBusName;
    message += "（Wayland 下可用桌面自定义快捷键调用 gdbus）";
    shot::Logger(host).info(message);
}

void ControlServiceImpl::onNameLost(GDBusConnection* lostConnection, const char* name) {
    if (stopping) {
        return;  // 主动注销过程中的 name_lost 不是异常
    }
    std::string message = "[session_control] 无法使用 D-Bus 名称 ";
    message += (name != nullptr) ? name : kBusName;
    // GBusNameLostCallback 的约定：connection == NULL 表示连接建立失败或会话总线已断开；
    // 非空表示连接还在、只是名称拿不到（另一个宿主已经占着它）。
    if (lostConnection == nullptr) {
        message += "：无法连接当前会话总线（会话里没有 D-Bus，或连接已断开）；";
    } else {
        message += "：名称已被另一个宿主占用；";
    }
    message += "本插件的会话入口不可用（其它功能与配置不受影响）";
    shot::Logger(host).warn(message);
}

}  // namespace detail

}  // namespace session_control
