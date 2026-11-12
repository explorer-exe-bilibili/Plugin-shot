#pragma once

// ============================================================================
//  include/capture_frame.h —— 一帧 RGBA 截屏（纯数据，不依赖任何平台头）
//
//  为什么用 RGBA 而不是 stb 解码出来的原始通道数：拼接只需要"把新行原样拷进
//  画布"，统一成 4 字节/像素之后行拷贝就是一次 memcpy；最终写 PNG 也不需要
//  再转换通道顺序（stb 解码与写回都按 RGBA 走）。截图不带透明信息，alpha 恒为
//  255，但保留它比逐像素挑通道便宜。
// ============================================================================

#include <cstdint>
#include <vector>

namespace long_capture {

/**
 * @brief 一帧解码后的截屏：紧凑行，每像素 4 字节（R、G、B、A）。
 *
 * @note 一帧由 ScreenGrabber（Windows GDI）直接抓进内存，不经过任何落盘文件；贴进画布后
 *       必须立刻 release()，否则 60ms 一帧会在内存里堆叠几十 MB。
 */
struct CaptureFrame {
    int width = 0;                     /**< 宽度（物理像素）。 */
    int height = 0;                    /**< 高度（物理像素）。 */
    std::vector<std::uint8_t> rgba;    /**< height × width × 4 字节，行间无 padding。 */

    /** @brief 是否是一帧可用图像（尺寸为正且像素缓冲对得上）。 */
    bool empty() const noexcept {
        return width <= 0 || height <= 0 ||
               rgba.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    }

    /** @brief 取第 y 行的首地址；调用方自己保证 y 在范围内。 */
    const std::uint8_t* row(int y) const noexcept {
        return rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
    }

    /**
     * @brief 真正释放像素内存（swap 一个空 vector，连 capacity 一起还给 allocator）。
     *
     * 只 clear() 的话 capacity 还挂着，长会话里每一帧都占着一整帧的内存；
     * 贴完新行立刻 release() 才能把峰值压到"画布 + 一帧"。
     */
    void release() noexcept {
        std::vector<std::uint8_t>().swap(rgba);
        width = 0;
        height = 0;
    }
};

}  // namespace long_capture
