#pragma once

// ============================================================================
//  wayland_capture.h —— 内部头：Linux/Wayland 采集编排
//
//  **不是插件对外接口**：ScreenCapture（公共头）在非 Windows 平台持有一个本对象，
//  把契约层的 describe/capture 转成"门户会话 + PipeWire 流 + 帧缓冲"三件事。
//
//  分层：screen_capture_linux.cpp 负责契约校验与返回码；本类负责环境判断、
//  会话生命周期与失败分类；wayland_portal_session / wayland_pipewire_stream /
//  wayland_frame_buffer 各自只管一个协议层。
//
//  线程：宿主在采集流水线 worker 线程上同步调用 ensureReady/fetchFrame；本类内部
//  再拥有两个线程（门户的 GLib 线程、PipeWire 的 loop 线程）。两个线程都由插件
//  创建、由 destroy 路径 join，绝不把 GLib / PipeWire 的调度循环放到宿主线程上。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "plugin_api.h"
#include "wayland_frame_buffer.h"
#include "wayland_pipewire_stream.h"
#include "wayland_portal_session.h"

namespace screen_capture {

/**
 * @brief Linux/Wayland 采集编排：环境检测、会话重建、取帧与失败分类。
 *
 * 会话是**懒建立**的：插件加载时不弹任何系统对话框，第一次 describe/capture 才
 * 建立门户会话并弹 GNOME 的共享选择框。一次会话被复用直到：合成器关闭它、用户
 * 取消、超时或 PipeWire 失败；之后下一次 describe（新的一次截屏请求）会重建会话
 * 并重新弹框。环境不是 Wayland 时不重试（会话状态不会自己变），直接返回明确错误。
 */
class WaylandCapture {
public:
    /**
     * @brief 绑定宿主服务表（只用于写日志），会话此时还没有建立。
     * @param[in] host 宿主服务表；可为 nullptr（老宿主，日志静默丢弃）。
     */
    explicit WaylandCapture(const ShotHostServices* host) noexcept;

    /** @brief 停流、结束门户会话并 join 两个后台线程。 */
    ~WaylandCapture();

    /** @brief 不可拷贝：持有线程与管道 fd 的所有权。 */
    WaylandCapture(const WaylandCapture&) = delete;
    /** @brief 不可赋值。 */
    WaylandCapture& operator=(const WaylandCapture&) = delete;

    /**
     * @brief 确保门户会话与 PipeWire 流已就绪，并给出 describe 应报告的尺寸。
     *
     * 尺寸来自 PipeWire 实际协商结果（WaylandPortalSession::takePipeWireRemote 的
     * 门户尺寸只作为协商默认值）。失败分四类：不是 Wayland 会话、门户不可用/拒绝、
     * 用户取消/超时、PipeWire 连接/协商失败；都写进 `error` 并返回契约返回码 3。
     *
     * @param[out] width  协商出的帧宽（成功时 >= 1）。
     * @param[out] height 协商出的帧高（成功时 >= 1）。
     * @param[out] error  失败原因（中文）；成功时清空。
     * @return 0 = 就绪；3 = 采集通道不可用（已分类，原因见 error）。
     * @note 首次调用可能阻塞到用户在系统选择框里确认（最长 120 秒）；宿主本来就在
     *       采集 worker 线程上同步调用，不占 main 线程。
     */
    std::int32_t ensureReady(std::int32_t& width, std::int32_t& height, std::string& error);

    /**
     * @brief 等一帧新的（最多 200ms），写进宿主缓冲。
     *
     * 超时但缓冲区里有旧帧时返回 0（并写一条 WARN）；没有任何帧或帧尺寸
     * 与目标图不一致时返回 2/3，宿主不会使用这次写入的内容。
     *
     * @param[in,out] dst       宿主目标图；只写像素，不改结构体字段。
     * @param[in]     timeoutMs 等新帧的上限（毫秒），契约层传 200。
     * @param[out]    error     失败原因（中文）；成功时清空。
     * @return 0 = 已写入（可能是上一帧）；2 = 帧尺寸与目标图不一致；3 = 还没有首帧。
     */
    std::int32_t fetchFrame(ShotImage& dst, std::int32_t timeoutMs, std::string& error);

    /**
     * @brief 提示一次 Wayland 的光标限制（每个插件实例只 WARN 一次）。
     *
     * Wayland 不允许应用自己读光标或把光标合成进别人的帧：门户按 embedded 模式
     * 把光标交给合成器，画面里有没有光标由合成器决定。所以 include_cursor 在
     * Wayland 上没有可执行语义，只能如实提示，不能假装支持。
     */
    void warnCursorLimitation();

    /**
     * @brief 设置门户要共享的类型（`shot.configurable` 的 source）。
     *
     * 只影响下一次建立/重建会话；当前会话类型不同的时候，ensureReady 会先停掉旧
     * 会话再重建（会重新弹系统授权/选择框）。
     *
     * @param[in] window true = 单个窗口（XDP_OUTPUT_WINDOW）；false = 整个输出。
     * @note 可能在宿主 main 线程（set_config）调用、采集 worker 线程读；用 atomic 保存。
     */
    void setCaptureWindow(bool window) noexcept;

private:
    /** @brief 会话生命周期状态；只在 m_mutex 下读写。 */
    enum class State {
        Idle,   /**< 还没有建立会话。 */
        Ready,  /**< 门户与 PipeWire 都就绪。 */
        Failed, /**< 上一次建立/运行失败；下一次请求按需重建。 */
    };

    /**
     * @brief 运行时探测当前桌面会话是不是 Wayland。
     *
     * `WAYLAND_DISPLAY` 最直接；`XDG_SESSION_TYPE` 是 display manager 写的提示，
     * 精简环境里可能只有其中一个，所以两个都看（大小写不敏感）。
     *
     * @return true = Wayland；false = X11 / 无显示会话 / 环境变量缺失。
     */
    static bool isWaylandSession();

    /**
     * @brief 停流、结束会话、清空帧；调用方必须持有 m_mutex。
     *
     * 顺序有讲究：先停 PipeWire 流（它的节点属于会话），再结束门户会话，最后清帧；
     * 反过来会让 PipeWire 回调在会话销毁后还访问已失效的节点。
     */
    void teardown();

    const ShotHostServices* m_host = nullptr; /**< 宿主服务表，只用于写日志。 */
    WaylandFrameBuffer m_frames;              /**< 最新帧的双缓冲。 */
    WaylandPortalSession m_portal;            /**< 门户会话（自己的 GLib 线程）。 */
    WaylandPipeWireStream m_stream;           /**< PipeWire 接收端（自己的 loop 线程）。 */

    mutable std::mutex m_mutex; /**< 串行化 ensureReady/fetchFrame 与状态读写。 */
    State m_state = State::Idle;
    std::atomic<bool> m_captureWindow{false}; /**< source 配置快照（true = window）。 */
    bool m_sessionWindowSource = false;       /**< 当前会话用的 source（m_mutex 下读写）。 */
    bool m_notWayland = false;  /**< 环境判定失败是永久性的，不反复弹框重试。 */
    bool m_cursorWarned = false; /**< 光标限制只提示一次，避免每次截图刷日志。 */
    std::string m_error;        /**< 最近一次失败原因（中文）。 */
    std::int32_t m_width = 0;   /**< 当前流协商宽度。 */
    std::int32_t m_height = 0;  /**< 当前流协商高度。 */
};

}  // namespace screen_capture
