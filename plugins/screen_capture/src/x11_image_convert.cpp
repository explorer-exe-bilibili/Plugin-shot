// XImage → BGRA8 的像素转换（Linux/X11 平台层的一个关注点，单独成文件）。
//
// X server 给回来的布局随 visual 与 server 字节序变化：常见的是 32bpp / 24bpp、
// LSBFirst（x86）、掩码 0x00RRGGBB；也有 MSBFirst 的机器、16bpp（RGB565）以及
// 30-bit 深色的布局。所以这里按 XImage 的 red/green/blue mask 做通用映射，
// 只有最常见的 32/24bpp LSB 走整行快路径。
//
// 目标图按 dst->stride 行寻址（stride 可负、可大于 width*4），alpha 一律置 255：
// 屏幕像素没有可用的 alpha，留 0 会让保存出的 PNG 整张透明（与 Windows 的
// GetDIBits 路径同一处理）。

#include "x11_capture.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace screen_capture {
namespace {

/**
 * @brief 按 server 字节序把 bytesPerPixel 个字节拼成一个像素值。
 * @param[in] source        像素首地址。
 * @param[in] bytesPerPixel 2 / 3 / 4。
 * @param[in] byteOrder     XImage::byte_order（LSBFirst / MSBFirst）。
 * @return 未做通道映射的原始像素值。
 */
std::uint32_t readPixel(const std::uint8_t* source, int bytesPerPixel, int byteOrder) {
    std::uint32_t value = 0;
    if (byteOrder == LSBFirst) {
        for (int index = 0; index < bytesPerPixel; ++index) {
            value |= static_cast<std::uint32_t>(source[index]) << (8 * index);
        }
        return value;
    }
    for (int index = 0; index < bytesPerPixel; ++index) {
        value = (value << 8) | static_cast<std::uint32_t>(source[index]);
    }
    return value;
}

/**
 * @brief 把一个通道从 mask 指定的位置提取出来并线性映射到 8 位。
 *
 * 用 mask 而不是固定位移：16bpp（5/6 位通道）与 30-bit（10 位通道）都能覆盖，
 * 不必为每种 depth 写一个分支。mask 必须非 0（调用方已校验三个 mask 都存在）。
 *
 * @param[in] pixel 原始像素值。
 * @param[in] mask  该通道在像素值里的掩码。
 * @return 0..255 的通道值。
 */
std::uint8_t scaleChannel(std::uint32_t pixel, unsigned long mask) {
    unsigned int shift = 0;
    while ((mask & (1UL << shift)) == 0) {
        ++shift;
    }
    const std::uint32_t maxValue = static_cast<std::uint32_t>(mask >> shift);
    const std::uint32_t value = (pixel & static_cast<std::uint32_t>(mask)) >> shift;
    // 四舍五入而不是截断：5/6/10 位通道直接截断会让灰色渐变出现系统性偏暗。
    return static_cast<std::uint8_t>((value * 255U + maxValue / 2U) / maxValue);
}

}  // namespace

bool X11Capture::convertToBgra(const XImage& image, const ShotImage& dst, shot::Logger log) {
    if (image.format != ZPixmap) {
        log.error("screen_capture: X11 XImage 不是 ZPixmap，无法转换成 BGRA8");
        return false;
    }
    if (image.data == nullptr) {
        log.error("screen_capture: X11 XImage 没有像素数据");
        return false;
    }
    // XImage 的 width/height 是 int（Xlib 内部也按有符号处理），直接和 dst 比，
    // 不要转 unsigned —— 尺寸已经在契约层校验为正数。
    if (image.width != dst.width || image.height != dst.height) {
        log.error("screen_capture: X11 XImage 尺寸与目标图不一致（" +
                  std::to_string(image.width) + "x" + std::to_string(image.height) + " vs " +
                  std::to_string(dst.width) + "x" + std::to_string(dst.height) + "）");
        return false;
    }
    if (image.bytes_per_line <= 0) {
        log.error("screen_capture: X11 XImage 的 bytes_per_line 非法");
        return false;
    }
    const int bytesPerPixel = image.bits_per_pixel / 8;
    if (bytesPerPixel < 2 || bytesPerPixel > 4) {
        log.error("screen_capture: X11 不支持的 bits_per_pixel（" +
                  std::to_string(image.bits_per_pixel) + "），无法转换成 BGRA8");
        return false;
    }
    if (image.red_mask == 0 || image.green_mask == 0 || image.blue_mask == 0) {
        // 8 位伪彩色/灰度 visual 没有 TrueColor 掩码；XGetImage 能抓到，但没有
        // 通用的调色板转换（root window 在现代桌面上都是 TrueColor）。
        log.error("screen_capture: X11 visual 没有 TrueColor 通道掩码（可能是伪彩色），"
                  "无法转换成 BGRA8");
        return false;
    }

    // 最常见的 server 布局：32bpp / 24bpp、LSBFirst、掩码 0x00RRGGBB。这时字节序
    // 恰好是 B、G、R（32bpp 多一个 X），整行直接搬，避免每像素移位/除法。
    const bool fast32 = image.bits_per_pixel == 32 && image.byte_order == LSBFirst &&
                        image.red_mask == 0xFF0000UL && image.green_mask == 0xFF00UL &&
                        image.blue_mask == 0xFFUL;
    const bool fast24 = image.bits_per_pixel == 24 && image.byte_order == LSBFirst &&
                        image.red_mask == 0xFF0000UL && image.green_mask == 0xFF00UL &&
                        image.blue_mask == 0xFFUL;

    const std::int32_t width = dst.width;
    const std::int32_t height = dst.height;
    for (std::int32_t y = 0; y < height; ++y) {
        const auto* sourceRow = reinterpret_cast<const std::uint8_t*>(image.data) +
                                static_cast<std::ptrdiff_t>(y) * image.bytes_per_line +
                                static_cast<std::ptrdiff_t>(image.xoffset) * bytesPerPixel;
        std::uint8_t* targetRow = dst.data + static_cast<std::ptrdiff_t>(y) * dst.stride;
        if (fast32) {
            for (std::int32_t x = 0; x < width; ++x) {
                const std::uint8_t* source = sourceRow + static_cast<std::ptrdiff_t>(x) * 4;
                std::uint8_t* target = targetRow + static_cast<std::ptrdiff_t>(x) * 4;
                target[0] = source[0];
                target[1] = source[1];
                target[2] = source[2];
                target[3] = 255;
            }
            continue;
        }
        if (fast24) {
            for (std::int32_t x = 0; x < width; ++x) {
                const std::uint8_t* source = sourceRow + static_cast<std::ptrdiff_t>(x) * 3;
                std::uint8_t* target = targetRow + static_cast<std::ptrdiff_t>(x) * 4;
                target[0] = source[0];
                target[1] = source[1];
                target[2] = source[2];
                target[3] = 255;
            }
            continue;
        }
        for (std::int32_t x = 0; x < width; ++x) {
            const std::uint32_t pixel =
                readPixel(sourceRow + static_cast<std::ptrdiff_t>(x) * bytesPerPixel,
                          bytesPerPixel, image.byte_order);
            std::uint8_t* target = targetRow + static_cast<std::ptrdiff_t>(x) * 4;
            target[0] = scaleChannel(pixel, image.blue_mask);
            target[1] = scaleChannel(pixel, image.green_mask);
            target[2] = scaleChannel(pixel, image.red_mask);
            target[3] = 255;
        }
    }
    return true;
}

}  // namespace screen_capture
