#pragma once

// ============================================================================
//  wayland_portal_session.h —— 内部头：xdg-desktop-portal ScreenCast 会话
//
//  **不是插件对外接口**：只被本插件的 Wayland 实现包含。公共头 screen_capture.h
//  只前置声明 WaylandCapture，看不到任何 libportal / GLib 类型。
//
//  职责：用 xdg-desktop-portal 的 ScreenCast 门户（libportal 封装）建立一次
//  屏幕共享会话，拿回三样东西：PipeWire 远程 fd、流节点 id、门户报的尺寸。
//  GNOME 会在 start 阶段弹系统授权/选择框；整个过程是异步的，由本类自己的
//  GLib 线程驱动，宿主线程只阻塞等待结果或超时。
//
//  为什么用自己的线程：libportal 的异步回调投递到"发起调用时的 thread-default
//  GMainContext"，而宿主没有 GLib 主循环、插件的 IO 回调也不保证在哪个线程；
//  把 xdp_portal_new() 与所有门户调用都压在这个线程的私有 GMainContext 里，
//  门户对象就只属于一个线程，跨线程只需要交接触发器与结果（mutex 保护）。
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>

#include "plugin_api.h"

namespace screen_capture {

/** @brief WaylandPortalSession 的实现结构；定义在 .cpp，避免 GLib 类型进头文件。 */
struct WaylandPortalSessionImpl;

/**
 * @brief 一次 ScreenCast 门户会话：启动、交付 PipeWire 远程连接、结束。
 *
 * 状态机（全部迁移都发生在本类的 GLib 线程上）：
 *
 *   Idle → Starting →（CreateSession + SelectSources）→（Start，弹授权/选择框）
 *        → Ready（保存 fd / node id / 尺寸；继续跑主循环接收 closed 信号）
 *        → Failed（用户取消、超时、门户不可用或其它 D-Bus 错误）
 *
 * `start()` 在调用线程（宿主流水线 worker）上阻塞等待上面这段流程结束；Ready 之后
 * 主循环继续运行，以便感知合成器撤销共享。`stop()` 取消未完成的请求、退出主循环、
 * 关闭会话并 join 线程；destroy 路径必须调用它。
 *
 * @note 一个实例只服务一个插件实例；不做跨实例共享，也不缓存授权（persist=None，
 *       每次重建会话都会重新弹框）。
 */
class WaylandPortalSession {
public:
    /**
     * @brief 绑定宿主服务表（只用于写日志）。
     * @param[in] host 宿主服务表；可为 nullptr（老宿主，日志静默丢弃）。
     */
    explicit WaylandPortalSession(const ShotHostServices* host) noexcept;

    /**
     * @brief 退出主循环、结束会话并 join 门户线程；重复调用是空操作。
     * @note 必须在卸载 DLL 之前调用，否则门户线程里的 libportal 回调可能落在
     *       已卸载的代码上。
     */
    ~WaylandPortalSession();

    /** @brief 不可拷贝：持有线程与 GLib 对象。 */
    WaylandPortalSession(const WaylandPortalSession&) = delete;
    /** @brief 不可赋值。 */
    WaylandPortalSession& operator=(const WaylandPortalSession&) = delete;

    /**
     * @brief 建立会话并等它 Ready（会弹 GNOME 的共享选择框）。
     *
     * 成功返回时尺寸信息已经由 `takePipeWireRemote()` 可读；失败时 `error` 是
     * 中文原因（区分用户取消、超时、门户不可用与其它门户错误）。
     *
     * @param[in]  timeoutMs   等用户操作的上限（毫秒）；超时后取消挂起的 D-Bus 请求。
     * @param[in]  shareWindow true = 只允许选单个窗口；false = 只允许选整个输出
     *                         （对应 shot.configurable 的 source = window / monitor）。
     * @param[out] error       失败原因（中文）；成功时清空。
     * @return true = 会话已启动；false = 未建立（可以再次 start 重试）。
     * @note 阻塞调用；只在采集流水线 worker 线程上调用。
     */
    bool start(std::int32_t timeoutMs, bool shareWindow, std::string& error);

    /**
     * @brief 取走门户打开的 PipeWire 远程 fd 与流信息（所有权转移一次）。
     *
     * fd 交给 PipeWire 的 `pw_context_connect_fd()` 后由 PipeWire 负责关闭，本类
     * 不再持有；调用方不能 close 第二次。
     *
     * @param[out] fd           成功时是 PipeWire 远程连接的 fd（>= 0）。
     * @param[out] nodeId       门户的流节点 id，作为 `pw_stream_connect` 的 target。
     * @param[out] width        门户报的 compositor 坐标尺寸（仅作协商默认值）。
     * @param[out] height       同上。
     * @param[out] error        失败原因（中文）；成功时清空。
     * @return true = 已交付；false = 会话未就绪、已关闭或 fd 已被取走。
     * @note 只能调用一次；`width/height` 不是最终的像素尺寸——Wayland 的 HiDPI /
     *       分数缩放下门户尺寸与 PipeWire buffer 尺寸可能不同，describe 必须用
     *       PipeWire 协商出的尺寸（见 WaylandPipeWireStream::negotiatedSize）。
     */
    bool takePipeWireRemote(int& fd, std::uint32_t& nodeId, std::int32_t& width,
                            std::int32_t& height, std::string& error);

    /**
     * @brief 会话是否被外部关闭（用户撤销共享、输出被拔掉/关闭）。
     * @param[out] reason 关闭原因（中文，只在返回 true 时写入）。
     * @return true = 已关闭；false = 仍有效或从未就绪。
     * @note 非阻塞；采集线程在每次复用会话前查一次。
     */
    bool isClosed(std::string& reason) const;

    /**
     * @brief 停止会话：取消未完成的请求、退出门户线程主循环、关闭会话并 join。
     * @note 重复调用是空操作；DLL 卸载前必须调用（析构也会调用一次）。
     */
    void stop() noexcept;

private:
    std::unique_ptr<WaylandPortalSessionImpl> m_impl; /**< 线程与门户状态；析构时 join。 */
};

}  // namespace screen_capture
