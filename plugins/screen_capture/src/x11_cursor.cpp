// XFixes 光标合成（Linux/X11 平台层的一个关注点，单独成文件）。
//
// X11 抓 root window 从来不包含鼠标指针，所以 include_cursor 打开时要在桌面像素
// 转换完之后单独叠一次。XFixes 的 GetCursorImage 一次给回三样东西：
//   · 光标位图（ARGB，颜色已预乘 alpha）；
//   · 热点在位图内的偏移（xhot / yhot）；
//   · 热点在 root window 上的当前位置（x / y）。
// 光标左上角 = (x - xhot, y - yhot)，这就是要贴到 shot 帧上的位置。热点对箭头
// 是 (0,0)，但 I 型/十字/手型不是，不减会整体画偏（与 Windows 的 GetIconInfo
// 必须减 xHotspot/yHotspot 是同一个坑）。
//
// 失败语义与 Windows 一致：光标只是锦上添花，取不到就 WARN 后跳过，抓屏仍然成功。
// XFixes 扩展缺失（Xvfb、精简 server）也走这条路径。

#include "x11_capture.h"

#include <cstddef>
#include <cstdint>

#include <X11/extensions/Xfixes.h>

namespace screen_capture {
namespace {

/**
 * @brief 把一个预乘 alpha 的通道叠到目标像素上。
 *
 * XFixes 规范写明颜色分量已经乘过 alpha，所以公式是 out = src + dst * (1 - a)，
 * 不能再乘一次 src；否则半透明光标边缘会明显偏暗。a = 255 时走调用方的快速
 * 赋值分支，不经过这里。
 *
 * @param[in] source      预乘后的源通道值（0..255）。
 * @param[in] destination 桌面像素的通道值（0..255）。
 * @param[in] alpha       源 alpha（0..254）。
 * @return 混合后的通道值；理论上不会超过 255，仍做一次防御性夹取。
 */
std::uint8_t blendPremultiplied(std::uint8_t source, std::uint8_t destination,
                                std::uint32_t alpha) {
    const std::uint32_t blended =
        static_cast<std::uint32_t>(source) +
        (static_cast<std::uint32_t>(destination) * (255U - alpha) + 127U) / 255U;
    return static_cast<std::uint8_t>(blended > 255U ? 255U : blended);
}

}  // namespace

void X11Capture::compositeCursor(Display* display, const ShotImage& dst, shot::Logger log) {
    int eventBase = 0;
    int errorBase = 0;
    if (XFixesQueryExtension(display, &eventBase, &errorBase) == 0) {
        log.warn("screen_capture: X11 XFixes 扩展不可用，跳过鼠标光标合成（抓屏照常成功）");
        return;
    }

    XFixesCursorImage* cursor = XFixesGetCursorImage(display);
    if (cursor == nullptr) {
        log.warn("screen_capture: X11 XFixesGetCursorImage 失败，跳过鼠标光标合成");
        return;
    }
    if (cursor->width == 0 || cursor->height == 0 || cursor->pixels == nullptr) {
        XFree(cursor);
        log.warn("screen_capture: X11 XFixes 返回的光标位图为空，跳过鼠标光标合成");
        return;
    }

    // x/y 是热点在 root window 上的位置；xhot/yhot 是热点在位图内的偏移。X screen
    // 的原点就是 (0,0)，所以不用再减 describe 的 origin（与多屏合并后的坐标一致）。
    const std::int64_t left = static_cast<std::int64_t>(cursor->x) -
                              static_cast<std::int64_t>(cursor->xhot);
    const std::int64_t top = static_cast<std::int64_t>(cursor->y) -
                             static_cast<std::int64_t>(cursor->yhot);
    const std::int32_t width = dst.width;
    const std::int32_t height = dst.height;
    const std::int64_t cursorWidth = cursor->width;

    for (std::int64_t row = 0; row < cursor->height; ++row) {
        const std::int64_t targetY = top + row;
        // 光标有一半在屏幕外时越界裁剪：只跳过越界的像素行/列，不整体放弃。
        if (targetY < 0 || targetY >= height) {
            continue;
        }
        std::uint8_t* targetRow = dst.data + static_cast<std::ptrdiff_t>(targetY) * dst.stride;
        for (std::int64_t column = 0; column < cursorWidth; ++column) {
            const std::int64_t targetX = left + column;
            if (targetX < 0 || targetX >= width) {
                continue;
            }
            // pixels 是 unsigned long 数组：协议只填低 32 位 ARGB，64 位平台必须截断。
            const std::uint32_t argb =
                static_cast<std::uint32_t>(cursor->pixels[row * cursorWidth + column]);
            const std::uint32_t alpha = argb >> 24;
            if (alpha == 0) {
                continue;  // 全透明像素：不动桌面
            }
            std::uint8_t* pixel = targetRow + static_cast<std::ptrdiff_t>(targetX) * 4;
            const std::uint8_t blue = static_cast<std::uint8_t>(argb);
            const std::uint8_t green = static_cast<std::uint8_t>(argb >> 8);
            const std::uint8_t red = static_cast<std::uint8_t>(argb >> 16);
            if (alpha == 255) {
                pixel[0] = blue;
                pixel[1] = green;
                pixel[2] = red;
            } else {
                pixel[0] = blendPremultiplied(blue, pixel[0], alpha);
                pixel[1] = blendPremultiplied(green, pixel[1], alpha);
                pixel[2] = blendPremultiplied(red, pixel[2], alpha);
            }
            pixel[3] = 255;  // 截图应当不透明
        }
    }
    XFree(cursor);
}

}  // namespace screen_capture
