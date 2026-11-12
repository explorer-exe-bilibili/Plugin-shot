// PipeWire 帧 → BGRA8 双缓冲的实现：只做排列转换、发布与按 stride 取帧。
//
// 这里不出现 PipeWire / GLib 类型：process 回调把原始指针与 PixelOrder 传进来，
// 转换和线程同步都收在这个文件里，协议层与契约层都不需要知道像素怎么排。

#include "wayland_frame_buffer.h"

#include <chrono>
#include <cstddef>
#include <cstring>

namespace screen_capture {
namespace {

// 一口气接受的最大边：4K/8K 都够用；越界帧直接丢弃，避免合成器发来的坏尺寸把
// width*height*4 撑爆（std::vector 会抛 bad_alloc，而这里在 C 回调路径上）。
constexpr std::int32_t kMaxDimension = 16384;

// 一层 BGRA8 的行字节数；用 int64 算，避免极端宽高下 int 乘法溢出。
std::int64_t rowBytesOf(std::int32_t width) {
    return static_cast<std::int64_t>(width) * 4;
}

// 源帧是否在可安全处理的范围内：正 stride 且一行装得下、边长不超过上限。
bool isFrameUsable(const std::uint8_t* src, std::int32_t stride, std::int32_t width,
                   std::int32_t height) {
    if (src == nullptr || width <= 0 || height <= 0 || stride <= 0) {
        return false;
    }
    if (width > kMaxDimension || height > kMaxDimension) {
        return false;
    }
    return static_cast<std::int64_t>(stride) >= rowBytesOf(width);
}

}  // namespace

bool WaylandFrameBuffer::publish(const std::uint8_t* src, std::int32_t stride, std::int32_t width,
                                 std::int32_t height, PixelOrder order) noexcept {
    if (!isFrameUsable(src, stride, width, height)) {
        return false;
    }

    // 分配/扩容可能抛 bad_alloc。这个函数会被 PipeWire 的 C 回调调用，异常绝不能
    // 越过 C 边界，所以就地吞掉并报告"这一帧丢弃"（下一帧会再试）。
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::size_t pixelBytes =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        const std::int32_t back = m_front ^ 1;
        if (m_buffers[back].size() != pixelBytes) {
            m_buffers[back].resize(pixelBytes);
        }

        std::uint8_t* out = m_buffers[back].data();
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
        for (std::int32_t y = 0; y < height; ++y) {
            const std::uint8_t* inRow = src + static_cast<std::int64_t>(y) * stride;
            std::uint8_t* outRow = out + static_cast<std::size_t>(y) * rowBytes;
            switch (order) {
                case PixelOrder::Bgrx:
                case PixelOrder::Bgra: {
                    // BGRX/BGRA 的 B/G/R 与合约顺序一致，先整行拷过来再把 alpha 置
                    // 255：屏幕画面应当不透明，GetDIBits 式的 alpha=0 会让 PNG 整张
                    // 透明（Windows 版踩过同一个坑）。
                    std::memcpy(outRow, inRow, rowBytes);
                    for (std::int32_t x = 0; x < width; ++x) {
                        outRow[static_cast<std::size_t>(x) * 4 + 3] = 255;
                    }
                    break;
                }
                case PixelOrder::Rgbx: {
                    // RGBx 的 R/B 位置相反，需要逐像素交换；G 与 alpha 直接写。
                    for (std::int32_t x = 0; x < width; ++x) {
                        const std::uint8_t* in = inRow + static_cast<std::size_t>(x) * 4;
                        std::uint8_t* pixel = outRow + static_cast<std::size_t>(x) * 4;
                        pixel[0] = in[2];
                        pixel[1] = in[1];
                        pixel[2] = in[0];
                        pixel[3] = 255;
                    }
                    break;
                }
                default:
                    return false;  // 枚举之外的排列不猜
            }
        }

        m_front = back;
        m_width = width;
        m_height = height;
        ++m_generation;
    } catch (...) {
        return false;
    }

    // 条件变量不归 try 管：发布成功后再唤醒等待者；失败时保持沉默。
    m_frameCv.notify_all();
    return true;
}

bool WaylandFrameBuffer::fetch(std::int32_t timeoutMs, const ShotImage& dst, FetchResult& result) {
    std::unique_lock<std::mutex> lock(m_mutex);
    const std::uint64_t startGeneration = m_generation;
    if (timeoutMs > 0 && m_generation == startGeneration) {
        m_frameCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this, startGeneration] { return m_generation != startGeneration; });
    }

    if (m_width <= 0 || m_height <= 0 || m_buffers[m_front].empty()) {
        result = FetchResult::NoFrame;
        return false;
    }
    if (dst.width != m_width || dst.height != m_height) {
        result = FetchResult::Mismatch;
        return false;
    }

    copyFrontTo(m_buffers[m_front].data(), m_width, m_height, dst);
    result = (m_generation != startGeneration) ? FetchResult::Fresh : FetchResult::Stale;
    return true;
}

void WaylandFrameBuffer::reset() noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffers[0].clear();
    m_buffers[1].clear();
    m_front = 0;
    m_width = 0;
    m_height = 0;
    m_generation = 0;
}

void WaylandFrameBuffer::copyFrontTo(const std::uint8_t* front, std::int32_t width,
                                     std::int32_t height, const ShotImage& dst) {
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    for (std::int32_t y = 0; y < height; ++y) {
        // stride 可以为负（自底向上）或大于 width*4（行尾填充），所以按行寻址，
        // 绝不能把 dst.data 当成连续块。
        std::uint8_t* dstRow = dst.data + static_cast<std::int64_t>(y) * dst.stride;
        std::memcpy(dstRow, front + static_cast<std::size_t>(y) * rowBytes, rowBytes);
    }
}

}  // namespace screen_capture
