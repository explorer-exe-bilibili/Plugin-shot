// 门户 fd → PipeWire 视频流：连接、协商 BGRx/RGBx、把每帧交给帧缓冲。
//
// PipeWire 侧对象都是 C 结构体，回调签名固定，只能放匿名命名空间的自由函数
// （STYLE.md §6.7 的合法例外）。线程模型见 wayland_pipewire_stream.h：pw_thread_loop
// 自带一个线程，回调都在该线程执行；宿主线程只调用 start/stop/negotiatedSize。
//
// Ubuntu 24.04（PipeWire 1.0.x / libpipewire-0.3）的流程：
//   pw_init → pw_thread_loop_new/start → pw_context_connect_fd(门户 fd)
//   → pw_stream_new + add_listener → pw_stream_connect(node_id, INPUT, AUTOCONNECT)
//   → param_changed 里解析格式/尺寸并请求 MemFd（MemPtr 兜底）buffer
//   → process 里 dequeue → 转 BGRA8 写帧缓冲 → queue。
// 门户 fd 由 PipeWire 接管：文档写明 socket 在断开或出错时由它关闭，本文件不再
// close 第二次（重复 close 可能误伤别的 fd）。

#include "wayland_pipewire_stream.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/thread-loop.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>

#include "shot_plugin_helpers.h"
#include "wayland_frame_buffer.h"

namespace screen_capture {

/**
 * @brief WaylandPipeWireStream 的实现：pw_thread_loop + context/core/stream 与协商结果。
 *
 * 字段分两类：
 *  - PipeWire 对象只在 `pw_thread_loop_lock()` 持有或 loop 线程里访问；
 *  - `streaming/failed/failure/width/height/format` 受 mutex 保护：回调写、start()
 *    的等待与采集线程读，跨线程只能通过这把锁。
 */
struct WaylandPipeWireStreamImpl {
    const ShotHostServices* host = nullptr; /**< 宿主服务表，只用于写日志。 */
    WaylandFrameBuffer* frames = nullptr;   /**< 帧缓冲；生命周期覆盖本对象。 */
    std::int32_t requestedWidth = 0;        /**< 门户报的宽度，仅作协商默认值。 */
    std::int32_t requestedHeight = 0;       /**< 门户报的高度，仅作协商默认值。 */

    struct pw_thread_loop* threadLoop = nullptr; /**< PipeWire 调度线程。 */
    bool loopStarted = false;             /**< start 成功过才需要 stop。 */
    bool pwInitialized = false;           /**< pw_init 与 pw_deinit 必须配对。 */
    struct pw_context* context = nullptr;        /**< 用 thread loop 的 pw_loop 创建。 */
    struct pw_core* core = nullptr;              /**< 通过门户 fd 连接到合成器的 PipeWire。 */
    struct pw_stream* stream = nullptr;          /**< 输入流（从门户节点取画面）。 */
    struct spa_hook streamListener{};            /**< stream 事件监听句柄。 */
    struct pw_stream_events streamEvents{};      /**< 必须活到 stream 销毁。 */

    std::mutex mutex;              /**< 保护下面这些跨线程状态。 */
    std::condition_variable stateCv; /**< 状态/协商变化时唤醒 start() 的等待。 */
    bool streaming = false;        /**< 流是否进入 STREAMING。 */
    bool failed = false;           /**< 流是否已失败（错误状态/断开/格式不支持）。 */
    std::string failure;           /**< failed 时的中文原因。 */
    enum spa_video_format format = SPA_VIDEO_FORMAT_UNKNOWN; /**< 协商到的像素排列。 */
    std::int32_t width = 0;        /**< 协商到的帧宽。 */
    std::int32_t height = 0;       /**< 协商到的帧高。 */
    bool formatLogged = false;     /**< 协商日志只打一次，避免重协商刷屏。 */
};

namespace {

// start() 等协商的外观上限：超过还没进入 STREAMING 就按失败处理，别让第一次
// capture 才暴露"流根本没连上"。
constexpr std::int32_t kStreamStartTimeoutMs = 5000;

// 单个维度的上限：4K/8K 都覆盖；再大就当成坏协商结果，避免帧缓冲分配失控。
constexpr std::int32_t kMaxDimension = 16384;

/** @brief 把 spa 的像素排列映射成帧缓冲能处理的顺序。 */
bool pixelOrderOf(enum spa_video_format format, WaylandFrameBuffer::PixelOrder& order) {
    switch (format) {
        case SPA_VIDEO_FORMAT_BGRx:
            order = WaylandFrameBuffer::PixelOrder::Bgrx;
            return true;
        case SPA_VIDEO_FORMAT_RGBx:
            order = WaylandFrameBuffer::PixelOrder::Rgbx;
            return true;
        case SPA_VIDEO_FORMAT_BGRA:
            order = WaylandFrameBuffer::PixelOrder::Bgra;
            return true;
        default:
            return false;
    }
}

/** @brief 把协商到的格式写成日志可读的名字。 */
const char* formatName(enum spa_video_format format) {
    switch (format) {
        case SPA_VIDEO_FORMAT_BGRx:
            return "BGRx";
        case SPA_VIDEO_FORMAT_RGBx:
            return "RGBx";
        case SPA_VIDEO_FORMAT_BGRA:
            return "BGRA";
        default:
            return "未知";
    }
}

/** @brief 标记流失败并唤醒 start()；调用方不必再自己 notify。 */
void markStreamFailed(WaylandPipeWireStreamImpl& impl, std::string reason) {
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.failed = true;
        impl.streaming = false;
        impl.failure = std::move(reason);
    }
    impl.stateCv.notify_all();
}

// ---- 下面的回调签名由 PipeWire 固定，只能是自由函数（STYLE.md §6.7 例外） ----

void onStreamStateChanged(void* data, enum pw_stream_state oldState, enum pw_stream_state state,
                          const char* error) {
    auto& impl = *static_cast<WaylandPipeWireStreamImpl*>(data);
    if (state == PW_STREAM_STATE_ERROR) {
        std::string message = "PipeWire 流进入错误状态";
        if (error != nullptr) {
            message += "：";
            message += error;
        }
        shot::Logger(impl.host).error("screen_capture: " + message);
        markStreamFailed(impl, message);
        return;
    }
    if (state == PW_STREAM_STATE_STREAMING) {
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.streaming = true;
        }
        impl.stateCv.notify_all();
        shot::Logger(impl.host).info("screen_capture: PipeWire 流已进入 STREAMING");
        return;
    }
    if (state == PW_STREAM_STATE_UNCONNECTED && oldState == PW_STREAM_STATE_STREAMING) {
        // 合成器结束共享/节点消失：门户的 closed 信号可能晚一步到，这里先按失败
        // 处理，下一次 capture 立刻给出明确原因而不是一直等新帧。
        const std::string message = "PipeWire 流已断开（合成器结束了共享）";
        shot::Logger(impl.host).warn("screen_capture: " + message);
        markStreamFailed(impl, message);
    }
}

void onStreamParamChanged(void* data, std::uint32_t id, const struct spa_pod* param) {
    auto& impl = *static_cast<WaylandPipeWireStreamImpl*>(data);
    if (param == nullptr || id != SPA_PARAM_Format) {
        return;
    }

    struct spa_video_info info{};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) {
        return;
    }
    if (info.media_type != SPA_MEDIA_TYPE_video || info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0) {
        return;
    }

    const enum spa_video_format format =
        static_cast<enum spa_video_format>(info.info.raw.format);
    const std::int32_t width = static_cast<std::int32_t>(info.info.raw.size.width);
    const std::int32_t height = static_cast<std::int32_t>(info.info.raw.size.height);
    WaylandFrameBuffer::PixelOrder order = WaylandFrameBuffer::PixelOrder::Bgrx;
    if (!pixelOrderOf(format, order) || width <= 0 || height <= 0 || width > kMaxDimension ||
        height > kMaxDimension) {
        const std::string message =
            std::string("PipeWire 协商到不支持的画面格式（") + formatName(format) + "，" +
            std::to_string(width) + "x" + std::to_string(height) + "）";
        shot::Logger(impl.host).error("screen_capture: " + message);
        markStreamFailed(impl, message);
        return;
    }

    bool logFormat = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.format = format;
        impl.width = width;
        impl.height = height;
        logFormat = !impl.formatLogged;
        impl.formatLogged = true;
    }
    impl.stateCv.notify_all();
    if (logFormat) {
        shot::Logger(impl.host).info("screen_capture: PipeWire 协商到 " + std::string(formatName(format)) +
                                     " " + std::to_string(width) + "x" + std::to_string(height));
    }

    // 告诉合成器我们能消费哪种 buffer 内存：MemFd 首选（跨进程共享、无需拷贝到
    // 中间缓冲），MemPtr 作为兜底（少数合成器/旧版本只给匿名内存）。两者在
    // process 里都表现为 datas[0].data，映射由 PW_STREAM_FLAG_MAP_BUFFERS 完成。
    std::uint8_t paramsBuffer[256] = {};
    struct spa_pod_builder builder{};
    builder.data = paramsBuffer;
    builder.size = sizeof(paramsBuffer);
    const struct spa_pod* params[1];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemFd) | (1u << SPA_DATA_MemPtr))));
    pw_stream_update_params(impl.stream, params, 1);
}

void onStreamProcess(void* data) {
    auto& impl = *static_cast<WaylandPipeWireStreamImpl*>(data);
    if (impl.stream == nullptr || impl.frames == nullptr) {
        return;
    }
    struct pw_buffer* buffer = pw_stream_dequeue_buffer(impl.stream);
    if (buffer == nullptr) {
        return;  // 没有空闲 buffer：丢这一帧，别阻塞合成器
    }

    struct spa_buffer* spaBuffer = buffer->buffer;
    if (spaBuffer != nullptr && spaBuffer->n_datas > 0) {
        struct spa_data& plane = spaBuffer->datas[0];
        std::int32_t width = 0;
        std::int32_t height = 0;
        WaylandFrameBuffer::PixelOrder order = WaylandFrameBuffer::PixelOrder::Bgrx;
        bool orderSupported = false;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            width = impl.width;
            height = impl.height;
            orderSupported = pixelOrderOf(impl.format, order);
        }
        if (plane.data != nullptr && width > 0 && height > 0 && orderSupported) {
            const struct spa_chunk* chunk = plane.chunk;
            const std::uint32_t offset = (chunk != nullptr) ? chunk->offset : 0;
            // 打包视频用 chunk->stride；缺失时按 4 字节/像素回退。
            const std::int32_t stride =
                (chunk != nullptr && chunk->stride > 0) ? chunk->stride : width * 4;
            // 防止坏 buffer 让转换读越界：最后一行至少要 width*4 字节。
            const std::uint64_t needed = static_cast<std::uint64_t>(offset) +
                                         static_cast<std::uint64_t>(height - 1) * stride +
                                         static_cast<std::uint64_t>(width) * 4;
            if (stride >= width * 4 && needed <= plane.maxsize) {
                const auto* pixels = static_cast<const std::uint8_t*>(plane.data) + offset;
                // TODO: 每帧都做整帧 BGRA8 转换（4K@60 下是整帧拷贝）会白白占 CPU；
                // 等真机验证后可以按"有没有 capture 在等新帧"降频，先保证语义简单。
                impl.frames->publish(pixels, stride, width, height, order);
            }
        }
    }
    pw_stream_queue_buffer(impl.stream, buffer);
}

/** @brief 停掉 PipeWire 对象；调用方不能持有 thread loop 锁。 */
void destroyPipeWire(WaylandPipeWireStreamImpl& impl) {
    if (impl.threadLoop != nullptr) {
        if (impl.loopStarted) {
            pw_thread_loop_stop(impl.threadLoop);  // 先停调度线程，之后不会再有回调
            impl.loopStarted = false;
        }
        if (impl.stream != nullptr) {
            pw_stream_destroy(impl.stream);
            impl.stream = nullptr;
        }
        if (impl.core != nullptr) {
            pw_core_disconnect(impl.core);  // 同时关闭门户 fd（connect_fd 接管）
            impl.core = nullptr;
        }
        if (impl.context != nullptr) {
            pw_context_destroy(impl.context);
            impl.context = nullptr;
        }
        pw_thread_loop_destroy(impl.threadLoop);
        impl.threadLoop = nullptr;
    }
    if (impl.pwInitialized) {
        pw_deinit();
        impl.pwInitialized = false;
    }
    impl.frames = nullptr;
}

}  // namespace

WaylandPipeWireStream::WaylandPipeWireStream(const ShotHostServices* host) noexcept
    : m_impl(new (std::nothrow) WaylandPipeWireStreamImpl()) {
    if (m_impl != nullptr) {
        m_impl->host = host;
    }
}

WaylandPipeWireStream::~WaylandPipeWireStream() {
    stop();
}

bool WaylandPipeWireStream::start(int pipewireFd, std::uint32_t nodeId, std::int32_t requestedWidth,
                                  std::int32_t requestedHeight, WaylandFrameBuffer& frames,
                                  std::string& error) {
    error.clear();
    if (m_impl == nullptr) {
        if (pipewireFd >= 0) {
            close(pipewireFd);
        }
        error = "无法创建 PipeWire 流状态（内存不足）";
        return false;
    }
    if (pipewireFd < 0 || requestedWidth <= 0 || requestedHeight <= 0) {
        if (pipewireFd >= 0) {
            close(pipewireFd);
        }
        error = "Wayland PipeWire 流的参数不合法（fd 或门户尺寸）";
        return false;
    }

    // 防御性：对象正常情况下是新的；如果上一次没停干净，先收掉。
    stop();
    m_impl->frames = &frames;
    m_impl->requestedWidth = requestedWidth;
    m_impl->requestedHeight = requestedHeight;

    bool loopLocked = false;
    auto fail = [&](std::string reason, bool fdOwnedByPipeWire) -> bool {
        if (loopLocked) {
            pw_thread_loop_unlock(m_impl->threadLoop);
            loopLocked = false;
        }
        if (!fdOwnedByPipeWire && pipewireFd >= 0) {
            close(pipewireFd);
        }
        destroyPipeWire(*m_impl);
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->streaming = false;
            m_impl->width = 0;
            m_impl->height = 0;
            m_impl->format = SPA_VIDEO_FORMAT_UNKNOWN;
            m_impl->formatLogged = false;
        }
        error = std::move(reason);
        return false;
    };

    pw_init(nullptr, nullptr);
    m_impl->pwInitialized = true;

    m_impl->threadLoop = pw_thread_loop_new("screen-capture", nullptr);
    if (m_impl->threadLoop == nullptr) {
        return fail("无法创建 PipeWire 线程循环（pw_thread_loop_new 失败）", false);
    }
    if (pw_thread_loop_start(m_impl->threadLoop) < 0) {
        return fail("无法启动 PipeWire 线程循环（pw_thread_loop_start 失败）", false);
    }
    m_impl->loopStarted = true;

    // 所有 PipeWire 调用都在 loop 锁内做：thread loop 的其它线程不会在对象创建
    // 到一半时开始分发回调。
    pw_thread_loop_lock(m_impl->threadLoop);
    loopLocked = true;

    struct pw_loop* loop = pw_thread_loop_get_loop(m_impl->threadLoop);
    m_impl->context = pw_context_new(loop, nullptr, 0);
    if (m_impl->context == nullptr) {
        return fail("无法创建 PipeWire context", false);
    }
    // 从这里开始 fd 归 PipeWire：文档写明 socket 在断开或出错时由它关闭。
    m_impl->core = pw_context_connect_fd(m_impl->context, pipewireFd, nullptr, 0);
    if (m_impl->core == nullptr) {
        return fail("无法通过门户 fd 连接 PipeWire（pw_context_connect_fd 失败）", true);
    }

    m_impl->stream = pw_stream_new(
        m_impl->core, "screen-capture",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (m_impl->stream == nullptr) {
        return fail("无法创建 PipeWire 输入流（pw_stream_new 失败）", true);
    }

    m_impl->streamEvents.version = PW_VERSION_STREAM_EVENTS;
    m_impl->streamEvents.state_changed = &onStreamStateChanged;
    m_impl->streamEvents.param_changed = &onStreamParamChanged;
    m_impl->streamEvents.process = &onStreamProcess;
    pw_stream_add_listener(m_impl->stream, &m_impl->streamListener, &m_impl->streamEvents,
                           m_impl.get());

    // 枚举格式：BGRx / RGBx 覆盖 GNOME 与 wlroots 系合成器；BGRA 作为少数后端的
    // 兜底（帧缓冲同样会把它按 BGRA8 收下）。尺寸用"门户尺寸为默认、1..16384 为
    // 范围"的 choice：门户尺寸是 compositor 坐标，HiDPI 下可能不是最终像素尺寸，
    // 所以最终以 param_changed 协商出的尺寸为准（describe 也用它）。
    std::uint8_t paramsBuffer[1024] = {};
    struct spa_pod_builder builder{};
    builder.data = paramsBuffer;
    builder.size = sizeof(paramsBuffer);
    const struct spa_rectangle defaultSize{static_cast<std::uint32_t>(requestedWidth),
                                           static_cast<std::uint32_t>(requestedHeight)};
    const struct spa_rectangle minSize{1, 1};
    const struct spa_rectangle maxSize{static_cast<std::uint32_t>(kMaxDimension),
                                       static_cast<std::uint32_t>(kMaxDimension)};
    // C++ 下 spa_pod_builder_add_object 这个 GCC 语句表达式宏返回 void*，
    // 直接赋给 const spa_pod* 会报 invalid conversion；显式 static_cast 即可。
    const struct spa_pod* params[1];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
                               SPA_VIDEO_FORMAT_BGRA),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minSize, &maxSize)));

    const std::int32_t connectResult = pw_stream_connect(
        m_impl->stream, PW_DIRECTION_INPUT, nodeId,
        static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        params, 1);
    if (connectResult < 0) {
        return fail("无法连接门户的 PipeWire 流节点（pw_stream_connect 失败）", true);
    }

    pw_thread_loop_unlock(m_impl->threadLoop);
    loopLocked = false;

    // 等协商完成与 STREAMING：错误状态会被回调置成 failed 并唤醒这里。
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->stateCv.wait_for(lock, std::chrono::milliseconds(kStreamStartTimeoutMs), [this] {
        return m_impl->failed || (m_impl->streaming && m_impl->width > 0 && m_impl->height > 0);
    });
    if (m_impl->failed) {
        const std::string reason =
            m_impl->failure.empty() ? "PipeWire 流协商失败（原因未知）" : m_impl->failure;
        lock.unlock();
        return fail(reason, true);
    }
    if (!m_impl->streaming || m_impl->width <= 0 || m_impl->height <= 0) {
        lock.unlock();
        return fail("PipeWire 流在 5 秒内没有完成协商/进入 STREAMING", true);
    }
    lock.unlock();

    shot::Logger(m_impl->host).info("screen_capture: PipeWire 流已就绪（节点 " +
                                    std::to_string(nodeId) + "）");
    return true;
}

void WaylandPipeWireStream::stop() noexcept {
    if (m_impl == nullptr) {
        return;
    }
    destroyPipeWire(*m_impl);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->streaming = false;
    m_impl->failed = false;
    m_impl->failure.clear();
    m_impl->format = SPA_VIDEO_FORMAT_UNKNOWN;
    m_impl->width = 0;
    m_impl->height = 0;
    m_impl->formatLogged = false;
    m_impl->requestedWidth = 0;
    m_impl->requestedHeight = 0;
}

bool WaylandPipeWireStream::negotiatedSize(std::int32_t& width, std::int32_t& height) const {
    if (m_impl == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->width <= 0 || m_impl->height <= 0) {
        return false;
    }
    width = m_impl->width;
    height = m_impl->height;
    return true;
}

bool WaylandPipeWireStream::failure(std::string& reason) const {
    if (m_impl == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->failed) {
        return false;
    }
    reason = m_impl->failure.empty() ? "PipeWire 流已失败（没有给出原因）" : m_impl->failure;
    return true;
}

}  // namespace screen_capture
