#pragma once

// ============================================================================
//  wayland_frame_buffer.h —— 内部头：PipeWire 帧 → BGRA8 双缓冲
//
//  **不是插件对外接口**：只被本插件的 Wayland 实现包含。这里不出现 PipeWire /
//  GLib 类型；PipeWire 的 process 回调只把"原始像素 + 排列"交给它，契约层只从它
//  取一帧已经排好序的 BGRA8。
//
//  为什么需要它：PipeWire 回调在自己的线程上跑，而 describe/capture 在宿主流水线
//  worker 线程上跑；两边只通过这个加锁的双缓冲交接"最新一帧"，帧内存指针本身不
//  跨线程保存。
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "plugin_api.h"

namespace screen_capture {

/**
 * @brief 最新一帧 BGRA8 的双缓冲：PipeWire 线程写、采集线程等/取。
 *
 * 双缓冲的意义：process 回调先写后台缓冲，写完再一次性换到前台；采集线程读前台
 * 时拿到的永远是完整一帧，不会出现"读到一半被下一帧覆盖"的撕裂画面。
 *
 * @note 所有公开方法都可以从任意线程调用；内部用 mutex 串行化。帧内存由本类自己
 *       分配，capture 返回后不保存宿主 `ShotImage` 的任何指针。
 */
class WaylandFrameBuffer {
public:
    /** @brief 源像素的通道排列（PipeWire 只给 raw video，需要在这里统一成 BGRA8）。 */
    enum class PixelOrder {
        Bgrx, /**< B、G、R、X；X 是填充字节，丢弃后 alpha 置 255。 */
        Rgbx, /**< R、G、B、X；交换 R/B 后 alpha 置 255。 */
        Bgra, /**< B、G、R、A；拷贝后 alpha 也置 255（截图应当不透明）。 */
    };

    /** @brief 一次取帧的结果。 */
    enum class FetchResult {
        Fresh,   /**< 等到了一帧新的。 */
        Stale,   /**< 超时，但缓冲区里至少有一帧旧的（调用方用旧帧 + WARN）。 */
        NoFrame, /**< 还没有任何帧可用。 */
        Mismatch, /**< 帧尺寸与目标图不一致（调用方按契约返回 2）。 */
    };

    /** @brief 构造：没有帧，尺寸为 0。 */
    WaylandFrameBuffer() = default;

    /** @brief 不可拷贝：内部有互斥量与条件变量。 */
    WaylandFrameBuffer(const WaylandFrameBuffer&) = delete;
    /** @brief 不可赋值。 */
    WaylandFrameBuffer& operator=(const WaylandFrameBuffer&) = delete;

    /**
     * @brief 发布一帧原始像素（PipeWire process 回调，在 PipeWire 线程上调用）。
     *
     * 按 order 把 src 转成 BGRA8 写进后台缓冲，完成后交换前台并唤醒等待者。只接受
     * 正的 stride（PipeWire 的视频 buffer 是自上而下打包的）；负 stride 直接丢弃，
     * 避免把倒置的画面交给契约层。
     *
     * @param[in] src    源像素首行首字节；不能为空。
     * @param[in] stride 源行距（字节），必须 >= width*4。
     * @param[in] width  像素宽，>= 1。
     * @param[in] height 像素高，>= 1。
     * @param[in] order  源通道排列。
     * @return true = 已发布；false = 参数不合法或尺寸超出安全范围（该帧被丢弃）。
     * @note 不写日志：process 回调每秒可能被调用几十次，坏帧由上层计数降噪。
     * @warning 内部可能分配帧缓冲（首次或分辨率变化时）；分配失败会捕获异常并返回
     *          false，异常绝不越过 PipeWire 的 C 回调边界。
     */
    bool publish(const std::uint8_t* src, std::int32_t stride, std::int32_t width,
                 std::int32_t height, PixelOrder order) noexcept;

    /**
     * @brief 最多等 timeoutMs 拿一帧新的；超时退回"上一帧"。
     *
     * 只写 dst 的像素：按 dst.stride 逐行写 width*4 字节，不修改 dst 的任何字段。
     * stride 可以为负（自底向上），行地址用 int64 计算，不假设 data 连续。
     *
     * @param[in]  timeoutMs 等待新帧的上限（毫秒）；0 = 不等待，只看当前帧。
     * @param[in]  dst       目标图；调用方已校验过格式与尺寸。
     * @param[out] result    Fresh / Stale / NoFrame / Mismatch 细分结果。
     * @return true = 已经写入像素（Fresh 或 Stale）；false = 没有可用帧或尺寸不一致。
     * @note 每次调用都从"进入时的帧序号"开始等：期间发布过任意一帧就算 Fresh。
     */
    bool fetch(std::int32_t timeoutMs, const ShotImage& dst, FetchResult& result);

    /**
     * @brief 清空帧与尺寸（会话重建时调用，避免把上一次会话的旧画面当"上一帧"）。
     */
    void reset() noexcept;

private:
    /**
     * @brief 把前台缓冲按目标 stride 拷进 dst（调用方必须持有 m_mutex）。
     * @param[in] front  前台缓冲首行像素。
     * @param[in] width  帧宽（已与 dst.width 一致）。
     * @param[in] height 帧高（已与 dst.height 一致）。
     * @param[in] dst    目标图；只写像素。
     */
    static void copyFrontTo(const std::uint8_t* front, std::int32_t width, std::int32_t height,
                            const ShotImage& dst);

    std::mutex m_mutex;                     /**< 保护下面所有字段。 */
    std::condition_variable m_frameCv;      /**< 发布新帧时唤醒 fetch。 */
    std::vector<std::uint8_t> m_buffers[2]; /**< 双缓冲；m_front 指向最新一帧。 */
    std::int32_t m_front = 0;               /**< 前台缓冲下标：0 或 1。 */
    std::int32_t m_width = 0;               /**< 当前帧宽；0 = 还没有帧。 */
    std::int32_t m_height = 0;              /**< 当前帧高；0 = 还没有帧。 */
    std::uint64_t m_generation = 0;         /**< 每发布一帧 +1，作为 fetch 的变更信号。 */
};

}  // namespace screen_capture
