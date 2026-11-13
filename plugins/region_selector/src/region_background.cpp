// region_background —— 触发瞬间的桌面快照（Win32 GDI）+ 上传成 GL 纹理。
//
// 为什么需要它（踩过的坑，不要删）：
//   覆盖层原来的做法是用 GLFW 的透明帧缓冲把窗口画成半透明，让桌面透出来。真机实测
//   第一次可以，但窗口 hide → show 一轮之后 DWM 的 blur-behind 合成失效，覆盖层变回
//   不透明黑屏（用户看到"整屏黑 + 两条十字线"，且之后再也不会恢复）。
//   现在改成：开框选前先截一张该显示器的桌面快照，上传成纹理，覆盖层用这张纹理当背景
//   画（窗口本身完全不透明）。遮罩/挖空/十字线都画在快照上，行为与"透明覆盖层"等价，
//   但不依赖任何窗口合成特性，hide/show 多少次都稳定。
//
// 坐标：monitorRect 是绝对物理像素（PMv2，与 capture.request 的 region 同一坐标系），
//      BitBlt 的源坐标直接用；GetDIBits 输出 32bpp 自顶向下 BGRA，正好可以
//      glTexImage2D(..., GL_BGRA, GL_UNSIGNED_BYTE)。
//
// 只有本文件包含 <windows.h>：Win32 类型留在平台层。

#include <cstdint>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GLFW/glfw3.h>  // 这里主要用它提供的 GL 1.1 声明（纹理）

// Windows 的 <GL/gl.h> 只到 OpenGL 1.1：GL_CLAMP_TO_EDGE 是 1.2 的常量，必须自己补。
// 不能继续用 GL_CLAMP——它在核心 Profile 里已被移除，会报 GL_INVALID_ENUM，导致
// glGetError 把一次成功的上传误判成失败（Wayland/X11 的灰遮罩就是这么来的）。
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {

bool RegionWindow::Impl::captureBackground() {
    if (monitorRect.width <= 0 || monitorRect.height <= 0) {
        return false;
    }
    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        shot::Logger(host).warn("region_selector：GetDC(NULL) 失败，无法截取覆盖层背景");
        return false;
    }
    HDC memoryDc = CreateCompatibleDC(screenDc);
    if (memoryDc == nullptr) {
        ReleaseDC(nullptr, screenDc);
        shot::Logger(host).warn("region_selector：CreateCompatibleDC 失败，无法截取覆盖层背景");
        return false;
    }
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, monitorRect.width, monitorRect.height);
    if (bitmap == nullptr) {
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        shot::Logger(host).warn("region_selector：CreateCompatibleBitmap 失败，无法截取覆盖层背景");
        return false;
    }

    const HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    // CAPTUREBLT：把分层窗口（含我们自己刚隐藏的覆盖层以外的内容）也纳入快照。
    const BOOL blitted = BitBlt(memoryDc, 0, 0, monitorRect.width, monitorRect.height, screenDc,
                                monitorRect.x, monitorRect.y, SRCCOPY | CAPTUREBLT);
    bool ok = blitted != FALSE;
    std::vector<std::uint8_t> pixels;
    if (ok) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = monitorRect.width;
        // 负高度 = 自顶向下，与 GL 纹理的行序一致（第一行是屏幕最上面）。
        info.bmiHeader.biHeight = -monitorRect.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        pixels.resize(static_cast<std::size_t>(monitorRect.width) *
                      static_cast<std::size_t>(monitorRect.height) * 4u);
        const int scanned = GetDIBits(memoryDc, bitmap, 0, static_cast<UINT>(monitorRect.height),
                                      pixels.data(), &info, DIB_RGB_COLORS);
        ok = scanned == monitorRect.height;
    }

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);

    if (!ok) {
        shot::Logger(host).warn("region_selector：桌面快照失败（BitBlt/GetDIBits），覆盖层背景留空");
        return false;
    }

    // Win32 给的是 BGRA，GL 1.1 没有 GL_BGRA 常量（那是 1.2/扩展）：CPU 换成 RGBA，
    // 省得再依赖扩展装载。分辨率最大 8K，一次换通道可忽略。
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        std::swap(pixels[i], pixels[i + 2]);
    }

    // 上传成 GL 纹理。GL 上下文在 UI 线程上当前有效（createWindow 里 makeCurrent）。
    if (backgroundTexture == 0) {
        glGenTextures(1, &backgroundTexture);
    }
    glBindTexture(GL_TEXTURE_2D, backgroundTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, monitorRect.width, monitorRect.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    backgroundWidth = monitorRect.width;
    backgroundHeight = monitorRect.height;
    shot::Logger(host).info("region_selector：已截取覆盖层背景 " +
                            std::to_string(backgroundWidth) + "x" +
                            std::to_string(backgroundHeight) + "（物理像素）");
    return true;
}

void RegionWindow::Impl::releaseBackgroundTexture() {
    if (backgroundTexture != 0) {
        glDeleteTextures(1, &backgroundTexture);
        backgroundTexture = 0;
    }
    backgroundWidth = 0;
    backgroundHeight = 0;
}

}  // namespace region_selector
