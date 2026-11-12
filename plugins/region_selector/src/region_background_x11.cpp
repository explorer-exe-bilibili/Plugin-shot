// region_background_x11 —— Linux/X11 版的"覆盖层背景"：XGetImage 整屏快照 + GL 纹理。
//
// 为什么必须用快照而不是透明窗口：
//   这是 Windows 分支用真机踩出来的结论（AGENTS.md §7 坑 #2）：透明帧缓冲在第一次
//   显示时可用，hide → show 一轮之后合成就会失效，变成整屏黑 + 只剩十字线。现在两个
//   平台的覆盖层都是**不透明窗口 + 触发瞬间的桌面快照**：遮罩/挖空/十字线/折角画在
//   快照上，效果等价于"透出桌面"，但不依赖任何窗口合成特性。
//
// 抓哪一块：
//   这里抓**整个 X screen**（root 窗口 0,0 起，DisplayWidth x DisplayHeight），而不是
//   只抓鼠标所在显示器。原因有两个：
//     1. 绘制层可以按覆盖层窗口的实际位置从整屏图里裁一块（UV 子矩形），窗口被 WM
//        挪了几像素也不会错位；
//     2. 请求的显示器矩形在根坐标里可能是负数/超出（窗口被摆放得比屏幕大），整屏图
//        裁剪时天然夹住，不会读到 root 之外。
//   代价是 4K 双屏时一次约 30MB 的临时内存，上传纹理后立刻释放（不常驻）。
//
// 像素格式：XImage 是 ZPixmap，bpp=32 且字节序与客户端一致时按 32 位字直接取
// R/G/B mask；其它少见格式（16/24bpp、字节序相反）退回 XGetPixel（慢但正确）。
// 通道按 mask 归一化到 0~255，不假设具体的 RGB 排列。
//
// 失败语义（需求要求）：XGetImage 失败只 WARN，覆盖层退化为**纯色底 + 遮罩**，
// 拖框/窗口吸附/发布区域都照常工作——背景只是观感，不是功能依赖。
//
// 线程：GL 上下文在 UI 线程上当前有效（createWindow 里 makeCurrent），本函数只在
// UI 线程的 startSelection 里调用。

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

/**
 * @brief 从一个像素值里取出 8 位通道：按 mask 右移后线性归一化到 0~255。
 *
 * @param[in] pixel XImage 里的一个像素值。
 * @param[in] mask  该通道在像素值里的位掩码（来自 XImage::red_mask 等）。
 * @return 0~255；mask 为 0（该 X visual 没有这个通道）时返回 0。
 */
std::uint8_t channelFromMask(unsigned long pixel, unsigned long mask) {
    if (mask == 0) {
        return 0;
    }
    int shift = 0;
    while (((mask >> shift) & 1ul) == 0ul) {
        ++shift;
    }
    const unsigned long value = (pixel & mask) >> shift;
    const unsigned long maximum = mask >> shift;
    if (maximum == 0) {
        return 0;
    }
    return static_cast<std::uint8_t>((value * 255ul + maximum / 2ul) / maximum);
}

/** @brief 宿主字节序（XImage::byte_order 用它判断"数据能不能直接按 32 位字读"）。 */
bool hostIsLittleEndian() {
    const std::uint16_t probe = 1;
    return *reinterpret_cast<const std::uint8_t*>(&probe) == 1;
}

}  // namespace

bool RegionWindow::Impl::x11CaptureBackground() {
    const shot::Logger log(host);
    // 上一次会话的纹理可能还挂在 Impl 上（窗口与纹理跨会话复用）：先释放，这样本次
    // 快照失败时绘制层看到的是"没有背景"，而不是把上一次的旧桌面画出来。
    releaseBackgroundTexture();
    Display* display = glfwGetX11Display();
    if (display == nullptr) {
        log.warn("region_selector：X11 Display 不可用，无法截取覆盖层背景（退化为纯色遮罩）");
        return false;
    }
    const int screen = DefaultScreen(display);
    const Window root = RootWindow(display, screen);
    const int screenWidth = DisplayWidth(display, screen);
    const int screenHeight = DisplayHeight(display, screen);
    if (screenWidth <= 0 || screenHeight <= 0) {
        log.warn("region_selector：X11 root 窗口尺寸非法，无法截取覆盖层背景（退化为纯色遮罩）");
        return false;
    }

    // 覆盖层此刻还是隐藏（未 map）的，所以这张图就是用户看到的桌面。
    XImage* image = XGetImage(display, root, 0, 0, static_cast<unsigned int>(screenWidth),
                              static_cast<unsigned int>(screenHeight), AllPlanes, ZPixmap);
    if (image == nullptr) {
        log.warn("region_selector：XGetImage 截取 X11 桌面失败（退化为纯色遮罩），"
                 "框选与发布区域不受影响");
        return false;
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(screenWidth) *
                                     static_cast<std::size_t>(screenHeight) * 4u);
    const bool nativeByteOrder = (image->byte_order == LSBFirst) == hostIsLittleEndian();
    if (image->bits_per_pixel == 32 && image->bytes_per_line >= screenWidth * 4 &&
        nativeByteOrder) {
        // 常见路径：32bpp ZPixmap，直接按行按 32 位字取（memcpy 避免未对齐读取）。
        for (int y = 0; y < screenHeight; ++y) {
            const auto* row = reinterpret_cast<const unsigned char*>(image->data) +
                              static_cast<std::size_t>(y) *
                                  static_cast<std::size_t>(image->bytes_per_line);
            std::uint8_t* destination =
                pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(screenWidth) * 4u;
            for (int x = 0; x < screenWidth; ++x) {
                std::uint32_t word = 0;
                std::memcpy(&word, row + static_cast<std::size_t>(x) * 4u, sizeof(word));
                destination[x * 4 + 0] = channelFromMask(word, image->red_mask);
                destination[x * 4 + 1] = channelFromMask(word, image->green_mask);
                destination[x * 4 + 2] = channelFromMask(word, image->blue_mask);
                destination[x * 4 + 3] = 0xFFu;
            }
        }
    } else {
        // 少见格式（16/24bpp、大端服务器等）：交给 Xlib 取像素值，慢但一定正确。
        for (int y = 0; y < screenHeight; ++y) {
            std::uint8_t* destination =
                pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(screenWidth) * 4u;
            for (int x = 0; x < screenWidth; ++x) {
                const unsigned long pixel = XGetPixel(image, x, y);
                destination[x * 4 + 0] = channelFromMask(pixel, image->red_mask);
                destination[x * 4 + 1] = channelFromMask(pixel, image->green_mask);
                destination[x * 4 + 2] = channelFromMask(pixel, image->blue_mask);
                destination[x * 4 + 3] = 0xFFu;
            }
        }
    }
    XDestroyImage(image);

    // 上传成 GL 纹理。GL 上下文在 UI 线程上当前有效；行序自顶向下，与纹理 v=0 在
    // 顶部的约定一致，不需要翻转。
    // 先清掉之前调用可能留下的 GL 错误（例如 ImGui 初始化阶段的试探）：否则下面的
    // glGetError 会把旧错误当成"这次纹理上传失败"，误删一张本来可用的纹理。
    while (glGetError() != GL_NO_ERROR) {
    }
    if (backgroundTexture == 0) {
        glGenTextures(1, &backgroundTexture);
    }
    glBindTexture(GL_TEXTURE_2D, backgroundTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, screenWidth, screenHeight, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        // 超过 GPU 纹理上限（多屏 8K）等：删掉半成品，按"没有背景"处理，绘制层会用纯色底。
        glDeleteTextures(1, &backgroundTexture);
        backgroundTexture = 0;
        log.warn("region_selector：X11 快照纹理上传失败（超过 GPU 纹理上限？），"
                 "覆盖层退化为纯色遮罩");
        return false;
    }
    backgroundWidth = screenWidth;
    backgroundHeight = screenHeight;
    log.info("region_selector：已截取覆盖层背景（X11 整屏快照 " +
             std::to_string(backgroundWidth) + "x" + std::to_string(backgroundHeight) +
             "，按覆盖层窗口位置裁剪显示）");
    return true;
}

}  // namespace region_selector
