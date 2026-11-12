#pragma once

// ============================================================================
//  wayland_portal_session_impl.h —— 内部头：WaylandPortalSession 的 PIMPL 状态
//
//  **不是插件对外接口**：只被 wayland_portal_session.cpp 与
//  wayland_portal_callbacks.cpp 包含。公共头 wayland_portal_session.h 只有前置
//  声明与 unique_ptr，不出现 GLib / libportal 类型。
//
//  为什么单独成文件：门户的状态机跨两个 .cpp —— 生命周期/线程在
//  wayland_portal_session.cpp，libportal 的异步回调在 wayland_portal_callbacks.cpp；
//  两个 TU 必须操作同一份 Impl 与状态迁移函数（STYLE.md §6.8：跨 .cpp 共享的
//  内部类型进独立内部头）。
//
//  线程规则：`portal` / `session` / `cancellable` 只由门户线程创建和使用；
//  `state` / `message` / `loop` / `cancellable` 指针 / 结果字段受 mutex 保护，
//  宿主线程只通过 mutex 读结果。任何跨线程迁移都走 markPortalFailed()。
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <libportal/portal-helpers.h>
#include <libportal/portal.h>

#include "plugin_api.h"

namespace screen_capture {

/**
 * @brief WaylandPortalSession 的实现：一个 GLib 线程 + 门户会话状态。
 *
 * 字段分两类（与线程规则对应）：`portal` / `session` 只在门户线程访问；状态、结果
 * 与 GLib 入口指针都在 mutex 下读写，因为 start()/stop() 在宿主线程上执行。
 */
struct WaylandPortalSessionImpl {
    /** @brief 会话生命周期状态；只在 mutex 下读写。 */
    enum class State {
        Idle,     /**< 还没有开始。 */
        Starting, /**< 正在等 CreateSession/SelectSources/Start（可能正在弹框）。 */
        Ready,    /**< 会话已启动，fd 与流信息可取。 */
        Failed,   /**< 本次建立失败；下一次 start() 会重建。 */
    };

    const ShotHostServices* host = nullptr;  /**< 宿主服务表，只用于写日志。 */
    std::thread thread;                      /**< 门户线程；joinable 表示 start 成功过。 */
    std::mutex mutex;                        /**< 保护状态、结果与 GLib 入口指针。 */
    std::condition_variable stateCv;         /**< start() 等状态离开 Starting。 */

    State state = State::Idle;     /**< 当前状态。 */
    std::string message;           /**< Failed 时的中文原因。 */
    bool stopRequested = false;    /**< stop() 置位；线程看到后退出主循环。 */
    bool shareWindow = false;      /**< 本次会话只共享窗口（否则共享整个输出）。 */

    GMainLoop* loop = nullptr;           /**< 门户外层主循环；仅门户线程创建/释放。 */
    GCancellable* cancellable = nullptr; /**< 取消挂起的门户请求；stop() 会 cancel。 */

    XdpPortal* portal = nullptr;   /**< 门户对象；只在门户线程使用。 */
    XdpSession* session = nullptr; /**< 会话对象；只在门户线程使用。 */

    int pipewireFd = -1;       /**< 门户 fd；takePipeWireRemote() 取走后置 -1。 */
    std::uint32_t nodeId = 0;  /**< 流节点 id。 */
    std::int32_t width = 0;    /**< 门户报的 compositor 尺寸（仅协商默认值）。 */
    std::int32_t height = 0;   /**< 同上。 */

    bool closed = false;        /**< 会话是否被外部关闭。 */
    std::string closedReason;   /**< 外部关闭原因（中文）。 */
};

/**
 * @brief 记录失败原因并唤醒 start()（不直接退主循环，定时源会收口）。
 *
 * 可以在门户线程（回调里）或门户线程启动阶段调用；跨线程安全。已经 Ready 时只更新
 * message、不把状态改回 Failed（运行期的 closed 由 closed 标记单独表达）。
 *
 * @param[in] impl    门户实现状态。
 * @param[in] message 中文失败原因。
 */
void markPortalFailed(WaylandPortalSessionImpl& impl, std::string message);

/**
 * @brief 创建 XdpPortal 并发起 ScreenCast 会话创建的异步链（门户线程调用）。
 *
 * 失败时通过 markPortalFailed() 记录原因；成功时后续回调分别在
 * onSessionCreated / onSessionStarted 里推进状态机。调用前 impl.stopRequested 必须
 * 为 false，且 impl.loop / impl.cancellable 已就绪。
 *
 * @param[in] impl 门户实现状态。
 */
void startScreencastSession(WaylandPortalSessionImpl& impl);

}  // namespace screen_capture
