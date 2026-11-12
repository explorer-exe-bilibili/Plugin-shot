#pragma once

// ============================================================================
//  plugins/session_control/include/control_service.h —— D-Bus 会话服务
//
//  在会话总线上暴露
//    bus name   org.pluginshot.Control
//    object     /org/pluginshot/Control
//    interface  org.pluginshot.Control
//  方法 Capture() / CaptureRegion() / OpenSettings() / CaptureLong() 都返回
//  (b ok, s message)，实现只是把方法翻译成事件总线主题——不直接调用任何插件。
//
//  线程模型：宿主没有 GLib 主循环，而 GDBus 的异步回调（g_bus_own_name 的
//  bus_acquired / name_acquired / name_lost，以及方法调用）必须在**被迭代的**
//  GMainContext 上派发。所以 start() 起一条专用线程：线程里 push 私有
//  GMainContext、调用 g_bus_own_name、跑 GMainLoop；所有 GLib 回调都在这条线程
//  上执行，事件发布本身任意线程可调（document/EVENT_BUS.md §3）。
//
//  生命周期：stop() 在服务线程里 g_bus_unown_name + 注销对象、退出循环并 join；
//  返回后本对象不再持有任何 GLib 回调，DLL 可以安全卸载。bus name 被另一个
//  宿主占用时只 WARN（本插件其它功能与配置照常可用），不拒绝加载。
// ============================================================================

#include <cstdint>
#include <memory>

#include "plugin_api.h"

namespace session_control {
namespace detail {
struct ControlServiceImpl;
}  // namespace detail

/**
 * @brief 会话总线上的控制服务：把 D-Bus 方法调用翻译成事件总线发布。
 *
 * 只在 Linux 构建（CMakeLists 里 `UNIX AND NOT APPLE`）；其它平台不会编译这个
 * 插件。一个实例对应一条专用线程与一个私有的 GMainContext。
 */
class ControlService {
public:
    /** @brief 构造：不启动线程、不碰 D-Bus；只准备实现状态。 */
    ControlService();

    /** @brief 析构：兜底调用 stop()，保证线程已 join、没有残留回调。 */
    ~ControlService();

    /** @brief 不可拷贝：内部持有线程与 GLib 资源。 */
    ControlService(const ControlService&) = delete;
    /** @brief 不可赋值。 */
    ControlService& operator=(const ControlService&) = delete;

    /**
     * @brief 启动服务线程并异步注册 D-Bus 名称；重复调用是空操作。
     *
     * 不等待名称获取完成：被占用时 name_lost 回调里只 WARN。线程创建失败
     * （例如系统线程耗尽）返回 false，调用方继续加载插件即可。
     *
     * @param[in] host 宿主服务表；不能为空（没有事件总线时方法调用会返回失败原因）。
     * @return true = 线程已启动（不代表名称已获取）；false = 参数为空或线程创建失败。
     */
    bool start(const ShotHostServices* host);

    /**
     * @brief 注销 D-Bus 名称与对象、退出服务线程并 join；重复调用是空操作。
     *
     * 返回后不会再有任何 GLib 回调落到本插件，可以安全卸载 DLL。私有
     * GMainContext 故意保留到对象析构：g_bus_get 的会话连接是进程级 singleton，
     * 它的消息派发固定挂在创建时的上下文上，保留上下文才能让随后的 start()
     * （配置 enabled 关掉再打开）继续收到方法调用。
     */
    void stop();

    /** @brief 服务线程当前是否已启动（名称是否获取成功看日志）。 */
    bool running() const noexcept;

private:
    std::unique_ptr<detail::ControlServiceImpl> m_impl; /**< 专用线程 + GMainContext。 */
};

}  // namespace session_control
