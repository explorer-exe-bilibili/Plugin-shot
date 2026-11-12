#pragma once

// ============================================================================
//  plugins/session_control/include/control_service_impl.h —— 内部头，不属于对外接口
//
//  ControlService 的实现状态与 GDBus 常量。只被 src/control_service.cpp 与
//  src/control_service_dispatch.cpp 包含；对外只暴露 control_service.h。
//
//  GLib 对象（GMainContext / GMainLoop / GDBusNodeInfo / GDBusConnection）的类型
//  也只出现在这个内部头与实现文件里，公共头不依赖 <gio/gio.h>（STYLE §6.3 的
//  "平台类型只出现在 PIMPL 内部"）。
// ============================================================================

#include <atomic>
#include <mutex>
#include <thread>

#include <gio/gio.h>

#include "plugin_api.h"

namespace session_control {
namespace detail {

// D-Bus 坐标：bus name 与 object path 用同一个字符串，interface 名也相同。
// GDBus 的方法回调里会核对 interface；这里集中成常量，避免两处拼写漂移。
inline constexpr char kBusName[] = "org.pluginshot.Control";
inline constexpr char kObjectPath[] = "/org/pluginshot/Control";
inline constexpr char kInterfaceName[] = "org.pluginshot.Control";

/**
 * @brief ControlService 的实现状态：一条专用线程 + 私有 GMainContext + D-Bus 注册句柄。
 *
 * 线程归属：除了 start/stop（调用方线程，持 lifecycleMutex），下面这些函数都只在
 * 服务线程上执行：threadMain、shutdownOnServiceThread、onBusAcquired、
 * onNameAcquired、onNameLost、dispatchMethod。GLib 的注册/注销/方法派发因此天然
 * 串行，不需要再加锁。
 *
 * 为什么必须有专用线程：宿主没有 GLib 主循环；g_bus_own_name 是异步的，它的回调
 * 与 GDBusConnection 的方法派发都挂在调用线程的 thread-default GMainContext 上。
 * 只调 g_bus_own_name 而不迭代上下文，名字永远拿不到、方法永远不会被调用。
 */
struct ControlServiceImpl {
    /** @brief 析构：释放私有 GMainContext（stop() 故意保留它，理由见 control_service.cpp 文件头）。 */
    ~ControlServiceImpl();

    /** @brief 服务线程主体：push context → g_bus_own_name → g_main_loop_run。 */
    void threadMain();

    /** @brief 服务线程上的注销动作：unregister object → unown name → quit loop。 */
    void shutdownOnServiceThread();

    /** @brief g_bus_own_name 的 bus_acquired：在这里注册对象与方法表。 */
    void onBusAcquired(GDBusConnection* connection);

    /** @brief 名称获取成功（此时 gdbus call 才能找到本服务）。 */
    void onNameAcquired(const char* name);

    /** @brief 名称获取失败/丢失：只 WARN，插件继续加载。 */
    void onNameLost(GDBusConnection* connection, const char* name);

    /**
     * @brief 把一个方法调用翻译成事件总线发布，并回复 (b ok, s message)。
     * @param[in] methodName  D-Bus 方法名（"Capture" 等）。
     * @param[in] parameters  入参元组；本接口的方法都不接受参数。
     * @param[out] invocation 回复句柄；调用方保证非空。
     */
    void dispatchMethod(const char* methodName, GVariant* parameters, GDBusMethodInvocation* invocation);

    const ShotHostServices* host = nullptr; /**< 宿主服务表；stop() join 后置空。 */
    std::mutex lifecycleMutex;              /**< 串行化 start/stop（调用方线程）。 */
    std::thread thread;                     /**< 服务线程；join 后不再可 join。 */
    /** running() 的返回值；start() 建线程前置 true，stop() 与线程创建失败时清 false。 */
    std::atomic<bool> runningFlag{false};
    /** 只在服务线程读写：主动关闭过程中的 name_lost 不是异常，不要报 WARN。 */
    bool stopping = false;

    GMainContext* context = nullptr;        /**< 私有上下文；首次 start 创建，对象析构时释放。 */
    GMainLoop* loop = nullptr;              /**< start() 在调用方线程创建；服务线程跑完释放。 */
    GDBusNodeInfo* nodeInfo = nullptr;      /**< 接口描述；注销对象后才能 unref。 */
    GDBusConnection* connection = nullptr;  /**< bus_acquired 里取引用，注销后 unref。 */
    guint ownerId = 0;                      /**< g_bus_own_name 返回的句柄。 */
    guint registrationId = 0;               /**< g_dbus_connection_register_object 的句柄。 */
};

/**
 * @brief 本服务的 D-Bus introspection XML（静态字符串，生命周期 = 进程）。
 * @return NUL 结尾的 XML 文本，不能为 nullptr。
 */
const char* introspectionXml();

/**
 * @brief 本服务的方法分发表。
 *
 * 返回的是静态只读对象：注册对象时把指针交给 GDBus，注销之后仍然有效，
 * 所以返回引用而不是临时值。
 *
 * @return GDBus 接口方法表；生命周期 = 进程。
 */
const GDBusInterfaceVTable& methodVTable();

}  // namespace detail
}  // namespace session_control
