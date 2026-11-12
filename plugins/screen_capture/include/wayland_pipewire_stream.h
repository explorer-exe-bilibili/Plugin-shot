#pragma once

// ============================================================================
//  wayland_pipewire_stream.h —— 内部头：通过门户 fd 接收 PipeWire 视频流
//
//  **不是插件对外接口**：只被本插件的 Wayland 实现包含。公共头看不到
//  pipewire / spa 类型；这里只暴露 bool + 中文原因与协商尺寸。
//
//  数据路径：门户给的 fd → pw_context_connect_fd → 绑定门户的流节点 →
//  param_changed 协商 BGRx/RGBx（MemFd 首选，MemPtr 兜底）→ process 回调把
//  最新一帧交给 WaylandFrameBuffer。PipeWire 的 loop 由 pw_thread_loop 在
//  自己的线程里跑，回调不在宿主线程上执行。
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>

#include "plugin_api.h"

namespace screen_capture {

class WaylandFrameBuffer;
/** @brief WaylandPipeWireStream 的实现结构；定义在 .cpp，避免 pipewire 头进公共头。 */
struct WaylandPipeWireStreamImpl;

/**
 * @brief 门户流节点的 PipeWire 接收端：连接、协商、把帧写进帧缓冲。
 *
 * 线程模型：`pw_thread_loop` 从 start() 起拥有一个专属线程；param_changed /
 * process / state_changed 都在该线程上执行（PipeWire 保证回调期间持有 loop 锁）。
 * start() 在返回前等协商完成，之后帧由回调异步写入帧缓冲，采集线程只从
 * WaylandFrameBuffer 取帧——两库之间没有共享的 PipeWire 对象。
 *
 * @note 生命周期比门户会话短：会话关闭后流节点会消失，所以 stop() 要先于
 *       WaylandPortalSession::stop()。
 */
class WaylandPipeWireStream {
public:
    /**
     * @brief 绑定宿主服务表（只用于写日志）。
     * @param[in] host 宿主服务表；可为 nullptr（日志静默丢弃）。
     */
    explicit WaylandPipeWireStream(const ShotHostServices* host) noexcept;

    /**
     * @brief 停流、销毁 PipeWire 对象并 join 线程；重复调用是空操作。
     * @note 必须在卸载 DLL 之前调用：process 回调是本 DLL 里的代码。
     */
    ~WaylandPipeWireStream();

    /** @brief 不可拷贝：持有线程与 PipeWire 对象。 */
    WaylandPipeWireStream(const WaylandPipeWireStream&) = delete;
    /** @brief 不可赋值。 */
    WaylandPipeWireStream& operator=(const WaylandPipeWireStream&) = delete;

    /**
     * @brief 停流、销毁 PipeWire 对象并 join 线程；重复调用是空操作。
     * @note 必须在 `WaylandPortalSession::stop()` 之前调用（会话关闭后流节点会消失，
     *       先停流才能让 process 回调在 DLL 卸载前彻底停下）。
     */
    void stop() noexcept;

    /**
     * @brief 通过门户 fd 连接 PipeWire，订阅 nodeId 并等到协商完成。
     *
     * 进入后先做 `pw_init` 与 `pw_thread_loop_new/start`，再在 loop 锁内创建
     * context/core/stream 并 connect；最后等 param_changed 给出格式与尺寸、流进入
     * `STREAMING`（最长 5 秒），避免把"连接失败"拖到第一次 capture 才暴露。
     *
     * @param[in] pipewireFd     门户 Remote fd；**所有权交给 PipeWire**（成功后由
     *                           `pw_core_disconnect` 关闭，失败路径见实现注释）。
     * @param[in] nodeId         门户流节点 id。
     * @param[in] requestedWidth 协商默认宽度（门户报的 compositor 尺寸，仅默认值）。
     * @param[in] requestedHeight 协商默认高度。
     * @param[in] frames         帧缓冲；生命周期必须覆盖本对象，不能为空。
     * @param[out] error         失败原因（中文）；成功时清空。
     * @return true = 流已进入 STREAMING 且协商尺寸可用；false = 连接/协商失败。
     */
    bool start(int pipewireFd, std::uint32_t nodeId, std::int32_t requestedWidth,
               std::int32_t requestedHeight, WaylandFrameBuffer& frames, std::string& error);

    /**
     * @brief 查询 PipeWire 实际协商出的帧尺寸（describe 的尺寸来源）。
     * @param[out] width  协商宽度；未协商时为 0。
     * @param[out] height 协商高度；未协商时为 0。
     * @return true = 已协商出正尺寸；false = 还没有可用的协商结果。
     * @note 门户报的尺寸是 compositor 坐标，HiDPI 下可能不等于 buffer 像素尺寸；
     *       契约要求 describe 与 capture 用同一块缓冲，所以这里以 PipeWire 为准。
     */
    bool negotiatedSize(std::int32_t& width, std::int32_t& height) const;

    /**
     * @brief 流是否已经失败（错误状态、被合成器断开、协商到不支持的格式）。
     * @param[out] reason 失败原因（中文，只在返回 true 时写入）。
     * @return true = 已失败；false = 正常或还没有启动。
     * @note 非阻塞；采集线程在取帧前查一次，避免对已断开的流一直等"新帧"。
     */
    bool failure(std::string& reason) const;

private:
    std::unique_ptr<WaylandPipeWireStreamImpl> m_impl; /**< 线程与 PipeWire 对象。 */
};

}  // namespace screen_capture
