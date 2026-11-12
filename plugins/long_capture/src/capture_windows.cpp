// 插件自带的 Windows 采集层：GetDC(nullptr) + BitBlt → 内存位图 → GetDIBits。
//
// 抓取流程（与 screen_capture 的 GDI 实现同一套系统调用，但只抓目标区域）：
//   SetThreadDpiAwarenessContext 入口把线程 DPI 上下文提到 PMv2
//   GetDC(nullptr)                屏幕 DC（覆盖整个虚拟桌面）
//   CreateCompatibleDC/Bitmap     与屏幕同格式的内存位图
//   BitBlt(SRCCOPY|CAPTUREBLT)    只拷目标矩形，源坐标是绝对屏幕坐标
//   GetDIBits(32bpp, biHeight<0)  按 top-down BGRA 读回
//   BGRA → RGBA 原地转换           stb_image_write 要 RGBA；屏幕 DC 没有 alpha，置 255
//
// 为什么每帧都重建 GDI 对象而不是缓存：一次会话最多 60fps、每帧几十 MB，缓存位图
// 会让 DLL 常驻一块大内存；GDI 对象的创建/释放是微秒级，相对 BitBlt 与拼接可以忽略。
// 句柄用 RAII 收尾，任何失败路径（包括 vector 分配抛 bad_alloc）都不会漏 GDI 对象。
//
// 不合成鼠标光标：长图里固定位置的光标会污染行指纹，滚动时也没有意义。

#include "capture_windows.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// 老 SDK 的 winuser.h 里可能没有 PMv2 上下文（Win10 1703 才有）：值固定是 -4，
// 运行期由系统解释，这里补一个定义，别让 SDK 版本卡住编译。
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 \
    reinterpret_cast<DPI_AWARENESS_CONTEXT>(static_cast<INT_PTR>(-4))
#endif

namespace long_capture {
namespace {

/** @brief 单边最大像素：CreateCompatibleBitmap 对超大位图会失败，提前给出明确原因。 */
constexpr int kMaxCaptureSide = 32767;

/** @brief SetThreadDpiAwarenessContext 的签名；运行期从 user32 解析，避免老系统加载失败。 */
using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);

/** @brief 取 user32 里的 SetThreadDpiAwarenessContext（系统没有时返回 nullptr）。 */
SetThreadDpiAwarenessContextFn setThreadDpiAwarenessContextFn() {
    static const auto function = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    return function;
}

/**
 * @brief 抓帧期间的线程 DPI 上下文守卫：构造时提到 PMv2，析构时恢复。
 *
 * 会话线程不是宿主 main 线程，不能假设它继承了进程的 DPI 感知；用完必须恢复，
 * 否则这个线程后续的 GetSystemMetrics / 建窗口会突然按物理像素算。
 */
class ScopedThreadDpiAwareness {
public:
    ScopedThreadDpiAwareness() : m_set(setThreadDpiAwarenessContextFn()) {
        if (m_set == nullptr) {
            return;  // Win10 1607 之前没有这个入口：保持原上下文继续采集
        }
        m_previous = m_set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    ~ScopedThreadDpiAwareness() {
        if (m_set != nullptr && m_previous != nullptr) {
            m_set(m_previous);
        }
    }

    ScopedThreadDpiAwareness(const ScopedThreadDpiAwareness&) = delete;
    ScopedThreadDpiAwareness& operator=(const ScopedThreadDpiAwareness&) = delete;

private:
    SetThreadDpiAwarenessContextFn m_set = nullptr;
    DPI_AWARENESS_CONTEXT m_previous = nullptr;
};

/** @brief 一次抓帧用到的 GDI 句柄：析构按逆序释放，任何返回路径都不漏。 */
struct GdiHandles {
    GdiHandles() = default;

    HDC screen = nullptr;   /**< GetDC(nullptr) 拿到的屏幕 DC。 */
    HDC memory = nullptr;   /**< CreateCompatibleDC 建的内存 DC。 */
    HBITMAP bitmap = nullptr; /**< CreateCompatibleBitmap 建的位图。 */
    HGDIOBJ previous = nullptr; /**< 位图选进内存 DC 前原来的对象。 */

    ~GdiHandles() {
        if (memory != nullptr && previous != nullptr) {
            SelectObject(memory, previous);
        }
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        if (memory != nullptr) {
            DeleteDC(memory);
        }
        if (screen != nullptr) {
            ReleaseDC(nullptr, screen);
        }
    }

    GdiHandles(const GdiHandles&) = delete;
    GdiHandles& operator=(const GdiHandles&) = delete;
};

/** @brief 把一步系统调用的失败原因写进 error（固定缓冲，避免失败路径再分配字符串）。 */
void setFailure(std::string& error, const char* step) {
    char message[160] = {};
    std::snprintf(message, sizeof(message), "GDI %s 失败（GetLastError=%lu）", step,
                  static_cast<unsigned long>(GetLastError()));
    error = message;
}

/** @brief BGRA8 → RGBA8 原地转换，并把 alpha 置 255（屏幕 DC 不提供 alpha）。 */
void convertBgraToRgba(std::uint8_t* pixels, int width, int height) {
    const std::size_t count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t index = 0; index < count; ++index) {
        std::uint8_t* pixel = pixels + index * 4U;
        const std::uint8_t blue = pixel[0];
        pixel[0] = pixel[2];
        pixel[2] = blue;
        pixel[3] = 255;
    }
}

}  // namespace

bool ScreenGrabber::capture(const ScreenRect& screenRect, CaptureFrame& out, std::string& error) {
    out.release();
    error.clear();
    if (screenRect.width <= 0 || screenRect.height <= 0) {
        error = "目标区域尺寸非法（宽高必须大于 0）";
        return false;
    }
    if (screenRect.width > kMaxCaptureSide || screenRect.height > kMaxCaptureSide) {
        error = "目标区域过大，超过单边 " + std::to_string(kMaxCaptureSide) + " 像素";
        return false;
    }

    // 先把帧缓冲准备好：vector 分配可能抛 bad_alloc，放在任何 GDI 对象之前，
    // 失败时不需要清理句柄。
    CaptureFrame frame;
    frame.width = screenRect.width;
    frame.height = screenRect.height;
    frame.rgba.resize(static_cast<std::size_t>(screenRect.width) *
                      static_cast<std::size_t>(screenRect.height) * 4U);

    const ScopedThreadDpiAwareness dpiAwareness;
    GdiHandles handles;
    handles.screen = GetDC(nullptr);
    if (handles.screen == nullptr) {
        setFailure(error, "GetDC(nullptr)");
        return false;
    }
    handles.memory = CreateCompatibleDC(handles.screen);
    if (handles.memory == nullptr) {
        setFailure(error, "CreateCompatibleDC");
        return false;
    }
    handles.bitmap = CreateCompatibleBitmap(handles.screen, screenRect.width, screenRect.height);
    if (handles.bitmap == nullptr) {
        setFailure(error, "CreateCompatibleBitmap");
        return false;
    }
    handles.previous = SelectObject(handles.memory, handles.bitmap);
    if (handles.previous == nullptr || handles.previous == HGDI_ERROR) {
        handles.previous = nullptr;
        setFailure(error, "SelectObject");
        return false;
    }

    // 源坐标就是绝对屏幕坐标：主屏左上角不一定是 (0,0)，左侧/上方的副屏会让坐标
    // 为负；BitBlt 的源坐标照常接受负值。CAPTUREBLT 把分层窗口一起抓进来。
    if (BitBlt(handles.memory, 0, 0, screenRect.width, screenRect.height, handles.screen,
               screenRect.x, screenRect.y, SRCCOPY | CAPTUREBLT) == 0) {
        setFailure(error, "BitBlt");
        return false;
    }

    // GetDIBits 要求位图没有被任何 DC 选中：先把原来的对象放回去。
    SelectObject(handles.memory, handles.previous);
    handles.previous = nullptr;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = screenRect.width;
    // 负高度 = top-down DIB：读回的第一行就是屏幕最上面一行，与 CaptureFrame 的行序一致。
    info.bmiHeader.biHeight = -screenRect.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;  // 32bpp；BI_RGB 下内存顺序是 B、G、R、A
    info.bmiHeader.biCompression = BI_RGB;
    if (GetDIBits(handles.memory, handles.bitmap, 0, static_cast<UINT>(screenRect.height),
                  frame.rgba.data(), &info, DIB_RGB_COLORS) == 0) {
        setFailure(error, "GetDIBits");
        return false;
    }

    convertBgraToRgba(frame.rgba.data(), frame.width, frame.height);
    out = std::move(frame);
    return true;
}

void ScreenGrabber::virtualDesktopOrigin(int& originX, int& originY) {
    originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
}

}  // namespace long_capture
