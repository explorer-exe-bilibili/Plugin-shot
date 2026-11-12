// Linux/Wayland 采集编排：环境判断 → 门户会话 → PipeWire 流 → 帧缓冲。
//
// 这一层只负责"把一个可用的采集通道准备好"并给出错误原因，契约返回码与入参校验在
// screen_capture_linux.cpp。两段式契约要求 describe 与 capture 用同一块尺寸：
// 这里的宽高一律取 PipeWire 实际协商结果（门户报的是 compositor 坐标，HiDPI 下
// 可能不是像素尺寸）。
//
// 线程：ensureReady/fetchFrame 由宿主采集 worker 线程调用；门户的 GLib 线程与
// PipeWire 的 loop 线程都由插件创建、由 teardown 停掉并 join，宿主线程不会被拉进
// 任何一个库的调度循环。

#include "wayland_capture.h"

#include <cctype>
#include <cstdlib>
#include <string>

#include "shot_plugin_helpers.h"

namespace screen_capture {
namespace {

// 首次截屏会弹 GNOME 的共享选择框。120 秒足够用户读完提示再点确认；超时后取消
// 挂起的 D-Bus 请求并给出可行动的错误，而不是让采集线程无限等下去。
constexpr std::int32_t kPortalStartTimeoutMs = 120000;

}  // namespace

WaylandCapture::WaylandCapture(const ShotHostServices* host) noexcept
    : m_host(host), m_portal(host), m_stream(host) {}

WaylandCapture::~WaylandCapture() {
    std::lock_guard<std::mutex> lock(m_mutex);
    teardown();
}

bool WaylandCapture::isWaylandSession() {
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (waylandDisplay != nullptr && waylandDisplay[0] != '\0') {
        return true;
    }
    // 某些会话只有 display manager 写的 XDG_SESSION_TYPE；大小写不敏感比较，
    // 不同发行版/登录管理器的大小写不完全一致。
    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType == nullptr) {
        return false;
    }
    std::string type(sessionType);
    for (char& character : type) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return type == "wayland";
}

std::int32_t WaylandCapture::ensureReady(std::int32_t& width, std::int32_t& height,
                                         std::string& error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    error.clear();
    const bool wantWindow = m_captureWindow.load(std::memory_order_relaxed);

    if (m_state == State::Ready) {
        if (wantWindow != m_sessionWindowSource) {
            // source 改了：旧会话的流类型不对，按"下一次截屏生效"重建（会重新弹框）。
            shot::Logger(m_host).info(
                "screen_capture: Wayland 共享类型已改变，结束旧门户会话并重新建立");
            teardown();
            m_state = State::Idle;
        } else {
            std::string closedReason;
            if (m_portal.isClosed(closedReason)) {
                const std::string reason = "Wayland 门户会话已结束：" + closedReason;
                teardown();
                m_state = State::Failed;
                m_error = reason;
                error = reason;
                return 3;
            }
            std::string streamError;
            if (m_stream.failure(streamError)) {
                teardown();
                m_state = State::Failed;
                m_error = streamError;
                error = streamError;
                return 3;
            }
            width = m_width;
            height = m_height;
            return 0;
        }
    }

    if (m_state == State::Failed) {
        if (m_notWayland) {
            // 环境不是 Wayland 不会自己变；继续重试只会白弹一次会话。
            error = m_error;
            return 3;
        }
        shot::Logger(m_host).info(
            "screen_capture: 上一次 Wayland 采集失败，重新建立门户会话（会再次弹出授权/选择框）");
        teardown();
        m_state = State::Idle;
    }

    if (!isWaylandSession()) {
        m_notWayland = true;
        m_state = State::Failed;
        m_error = "当前不是 Wayland 会话；X11 支持见后续任务";
        error = m_error;
        return 3;
    }

    // ---- 1) 门户会话：CreateSession + SelectSources + Start（可能弹框） ----
    std::string portalError;
    if (!m_portal.start(kPortalStartTimeoutMs, wantWindow, portalError)) {
        m_state = State::Failed;
        m_error = portalError;
        error = portalError;
        return 3;
    }

    int fd = -1;
    std::uint32_t nodeId = 0;
    std::int32_t portalWidth = 0;
    std::int32_t portalHeight = 0;
    if (!m_portal.takePipeWireRemote(fd, nodeId, portalWidth, portalHeight, portalError)) {
        m_portal.stop();
        m_state = State::Failed;
        m_error = portalError;
        error = portalError;
        return 3;
    }

    // ---- 2) PipeWire 接收端：fd 所有权从这里交给 PipeWire ----
    std::string pipewireError;
    if (!m_stream.start(fd, nodeId, portalWidth, portalHeight, m_frames, pipewireError)) {
        m_portal.stop();
        m_state = State::Failed;
        m_error = pipewireError;
        error = pipewireError;
        return 3;
    }

    std::int32_t negotiatedWidth = 0;
    std::int32_t negotiatedHeight = 0;
    if (!m_stream.negotiatedSize(negotiatedWidth, negotiatedHeight)) {
        // start() 成功时必然已经协商出尺寸；这里只是防御实现状态不一致。
        m_stream.stop();
        m_portal.stop();
        m_state = State::Failed;
        m_error = "PipeWire 流已启动但没有协商出画面尺寸";
        error = m_error;
        return 3;
    }

    m_width = negotiatedWidth;
    m_height = negotiatedHeight;
    m_sessionWindowSource = wantWindow;
    m_state = State::Ready;
    m_error.clear();
    width = m_width;
    height = m_height;
    shot::Logger(m_host).info("screen_capture: Wayland 采集已就绪（共享 " +
                              std::string(wantWindow ? "单个窗口" : "整个输出") + "，节点 " +
                              std::to_string(nodeId) + "，协商尺寸 " + std::to_string(width) +
                              "x" + std::to_string(height) + "）");
    return 0;
}

std::int32_t WaylandCapture::fetchFrame(ShotImage& dst, std::int32_t timeoutMs,
                                        std::string& error) {
    error.clear();
    std::string streamError;
    if (m_stream.failure(streamError)) {
        error = streamError;
        return 3;
    }

    WaylandFrameBuffer::FetchResult result = WaylandFrameBuffer::FetchResult::NoFrame;
    if (!m_frames.fetch(timeoutMs, dst, result)) {
        if (result == WaylandFrameBuffer::FetchResult::Mismatch) {
            error = "Wayland 流的帧尺寸与目标图不一致（合成器可能改了缩放或输出）";
            return 2;
        }
        error = "Wayland 门户的 PipeWire 流还没有产出首帧";
        return 3;
    }
    if (result == WaylandFrameBuffer::FetchResult::Stale) {
        shot::Logger(m_host).warn("screen_capture: 200ms 内没有新帧，使用上一帧");
    }
    return 0;
}

void WaylandCapture::warnCursorLimitation() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_cursorWarned) {
        return;
    }
    m_cursorWarned = true;
    // 不假装支持：Wayland 没有给应用读光标/合成光标的通用接口，embedded cursor
    // 是否真的画进画面由合成器决定，插件既不能检测也不能补画。
    shot::Logger(m_host).warn(
        "screen_capture: Wayland 下光标由门户以 embedded 模式交给合成器，插件无法检测、补画或去掉；"
        "画面是否含光标取决于合成器，include_cursor 只在 Windows 生效");
}

void WaylandCapture::setCaptureWindow(bool window) noexcept {
    m_captureWindow.store(window, std::memory_order_relaxed);
}

void WaylandCapture::teardown() {
    // 顺序：先停 PipeWire（它的节点属于门户会话），再结束会话，最后清帧；
    // 反过来会让 PipeWire 回调在会话销毁后还访问已失效的节点。
    m_stream.stop();
    m_portal.stop();
    m_frames.reset();
    m_width = 0;
    m_height = 0;
    m_sessionWindowSource = false;
}

}  // namespace screen_capture
