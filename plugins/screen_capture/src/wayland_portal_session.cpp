// ScreenCast 门户的生命周期：启动线程、等 Ready/Failed、停止并清理 GLib 对象。
//
// 异步链（CreateSession → SelectSources → Start → OpenPipeWireRemote）在
// wayland_portal_callbacks.cpp；这个文件只负责线程与状态，避免一个 .cpp 同时承担
// "状态机"和"平台调用"两件事（STYLE.md §6.8）。
//
// 主循环退出用挂在自家 GMainContext 上的 100ms 定时源轮询 stopRequested/Failed：
// 从宿主线程直接 g_main_loop_quit() 与门户外层 g_main_loop_run() 之间有启动/退出
// 时序竞争（quit 可能发生在 run 真正开始之前），用主循环自己的源收口最稳。

#include "wayland_portal_session.h"

#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

#include <unistd.h>

#include <libportal/portal-helpers.h>
#include <libportal/portal.h>

#include "shot_plugin_helpers.h"
#include "wayland_portal_session_impl.h"

namespace screen_capture {
namespace {

// 停止检查的轮询周期：足够快地响应 destroy，又不会让空转开销可感知。
constexpr std::uint32_t kStopPollIntervalMs = 100;

/** @brief 会话是否该退出主循环（stop 请求或启动失败）。 */
bool shouldQuit(WaylandPortalSessionImpl& impl) {
    std::lock_guard<std::mutex> lock(impl.mutex);
    return impl.stopRequested || impl.state == WaylandPortalSessionImpl::State::Failed;
}

/**
 * @brief 主循环里的停止检查源：stopRequested 或 Failed 时退出循环。
 * @return GLib 源回调约定：继续返回 G_SOURCE_CONTINUE，退出返回 G_SOURCE_REMOVE。
 */
gboolean pollStop(gpointer data) {
    auto& impl = *static_cast<WaylandPortalSessionImpl*>(data);
    if (!shouldQuit(impl)) {
        return G_SOURCE_CONTINUE;
    }
    if (impl.loop != nullptr) {
        g_main_loop_quit(impl.loop);
    }
    return G_SOURCE_REMOVE;
}

/**
 * @brief 门户线程主体：建 GMainContext/GMainLoop，跑完异步链，最后清理 GLib 对象。
 *
 * XdpPortal 必须在本线程、推入线程默认上下文之后创建（startScreencastSession 在
 * 本线程被调用）：它内部的 GDBus 连接会在该上下文里投递回调。主循环在 Ready 之后
 * 继续运行，用来接收会话的 closed 信号。
 */
void portalThreadMain(WaylandPortalSessionImpl& impl) {
    GMainContext* context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GMainLoop* loop = g_main_loop_new(context, FALSE);
    GCancellable* cancellable = g_cancellable_new();

    GSource* stopSource = g_timeout_source_new(kStopPollIntervalMs);
    g_source_set_callback(stopSource, &pollStop, &impl, nullptr);
    g_source_attach(stopSource, context);

    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.loop = loop;
        impl.cancellable = cancellable;
    }

    bool run = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.stopRequested && impl.state == WaylandPortalSessionImpl::State::Starting) {
            run = true;
        }
    }

    if (run) {
        startScreencastSession(impl);
        g_main_loop_run(loop);
    } else {
        // 停止请求在进入主循环之前就到了：别让 start() 白等到超时。
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            if (impl.state == WaylandPortalSessionImpl::State::Starting) {
                impl.state = WaylandPortalSessionImpl::State::Failed;
                impl.message = "Wayland 门户会话在启动前被停止";
            }
        }
        impl.stateCv.notify_all();
    }

    // ---- 退出清理：顺序与创建相反；此时本线程已经不再分发任何回调 ----
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.loop = nullptr;
        impl.cancellable = nullptr;
        if (impl.state == WaylandPortalSessionImpl::State::Starting) {
            // stop() 在建立过程中把线程叫停：必须让 start() 的等待结束，
            // 否则它要白等到超时（销毁插件时表现为卡顿）。
            impl.state = WaylandPortalSessionImpl::State::Failed;
            if (impl.message.empty()) {
                impl.message = "Wayland 门户会话在建立过程中被停止";
            }
        }
    }
    impl.stateCv.notify_all();
    if (impl.session != nullptr) {
        // 先断 closed 信号再 Close：teardown 期间不应该再走回调写日志。
        g_signal_handlers_disconnect_by_data(impl.session, &impl);
        xdp_session_close(impl.session);
        g_object_unref(impl.session);
        impl.session = nullptr;
    }
    if (impl.pipewireFd >= 0) {
        close(impl.pipewireFd);  // 没被 takePipeWireRemote 取走（start 失败/停止）
        impl.pipewireFd = -1;
    }
    if (impl.portal != nullptr) {
        g_object_unref(impl.portal);
        impl.portal = nullptr;
    }
    g_source_unref(stopSource);  // 上下文还持有 attach 的那份引用
    g_main_loop_unref(loop);
    g_object_unref(cancellable);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
}

}  // namespace

WaylandPortalSession::WaylandPortalSession(const ShotHostServices* host) noexcept
    : m_impl(new (std::nothrow) WaylandPortalSessionImpl()) {
    if (m_impl != nullptr) {
        m_impl->host = host;
    }
}

WaylandPortalSession::~WaylandPortalSession() {
    stop();
}

bool WaylandPortalSession::start(std::int32_t timeoutMs, bool shareWindow, std::string& error) {
    error.clear();
    if (m_impl == nullptr) {
        error = "无法创建 Wayland 门户会话状态（内存不足）";
        return false;
    }

    // 上一次失败/停止后重新开始：先回收旧线程，保证同一时刻只有一个门户线程。
    stop();

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->state = WaylandPortalSessionImpl::State::Starting;
        m_impl->message.clear();
        m_impl->stopRequested = false;
        m_impl->shareWindow = shareWindow;
        m_impl->closed = false;
        m_impl->closedReason.clear();
        m_impl->pipewireFd = -1;
        m_impl->nodeId = 0;
        m_impl->width = 0;
        m_impl->height = 0;
    }

    try {
        m_impl->thread = std::thread(&portalThreadMain, std::ref(*m_impl));
    } catch (const std::exception& exception) {
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->state = WaylandPortalSessionImpl::State::Failed;
            m_impl->message = std::string("无法创建 Wayland 门户线程：") + exception.what();
        }
        error = m_impl->message;
        return false;
    }

    std::unique_lock<std::mutex> lock(m_impl->mutex);
    const bool signalled = m_impl->stateCv.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [this] {
            return m_impl->state != WaylandPortalSessionImpl::State::Starting;
        });
    if (!signalled) {
        const std::string message = "等待 Wayland 门户授权超时（" +
                                    std::to_string(timeoutMs / 1000) +
                                    " 秒）；请重试并在弹出的共享选择框里确认";
        m_impl->stopRequested = true;
        if (m_impl->cancellable != nullptr) {
            g_cancellable_cancel(m_impl->cancellable);
        }
        lock.unlock();
        if (m_impl->thread.joinable()) {
            m_impl->thread.join();
        }
        {
            std::lock_guard<std::mutex> stateLock(m_impl->mutex);
            m_impl->state = WaylandPortalSessionImpl::State::Failed;
            m_impl->message = message;
            m_impl->stopRequested = false;
        }
        error = message;
        return false;
    }

    if (m_impl->state == WaylandPortalSessionImpl::State::Ready) {
        lock.unlock();
        return true;
    }

    error = m_impl->message.empty() ? "Wayland 门户会话创建失败（门户没有给出原因）"
                                    : m_impl->message;
    lock.unlock();
    // 失败后线程会看到 Failed 自行退出主循环；join 回收，保留 message 供调用方读取。
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    return false;
}

void WaylandPortalSession::stop() noexcept {
    if (m_impl == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->stopRequested = true;
        if (m_impl->cancellable != nullptr) {
            g_cancellable_cancel(m_impl->cancellable);
        }
    }
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->state = WaylandPortalSessionImpl::State::Idle;
    m_impl->message.clear();
    m_impl->stopRequested = false;
    m_impl->closed = false;
    m_impl->closedReason.clear();
    m_impl->pipewireFd = -1;
    m_impl->nodeId = 0;
    m_impl->width = 0;
    m_impl->height = 0;
}

bool WaylandPortalSession::takePipeWireRemote(int& fd, std::uint32_t& nodeId, std::int32_t& width,
                                              std::int32_t& height, std::string& error) {
    error.clear();
    if (m_impl == nullptr) {
        error = "Wayland 门户会话状态不存在（内存不足）";
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->state != WaylandPortalSessionImpl::State::Ready) {
        error = m_impl->message.empty() ? "Wayland 门户会话尚未就绪" : m_impl->message;
        return false;
    }
    if (m_impl->closed) {
        error = m_impl->closedReason;
        return false;
    }
    if (m_impl->pipewireFd < 0) {
        error = "门户的 PipeWire fd 已经被取走";
        return false;
    }
    fd = m_impl->pipewireFd;
    m_impl->pipewireFd = -1;
    nodeId = m_impl->nodeId;
    width = m_impl->width;
    height = m_impl->height;
    return true;
}

bool WaylandPortalSession::isClosed(std::string& reason) const {
    if (m_impl == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->closed) {
        return false;
    }
    reason = m_impl->closedReason;
    return true;
}

}  // namespace screen_capture
