// Windows GDI 平台层：只做系统调用，不判断契约返回码。
//
// 抓整个虚拟桌面的流水线：
//   SetThreadDpiAwarenessContext  入口先把线程 DPI 上下文提到 PER_MONITOR_AWARE_V2
//   GetDC(NULL)                    拿屏幕 DC（覆盖整个虚拟桌面）
//   CreateCompatibleDC/Bitmap      建内存 DC + 与屏幕同格式的位图
//   BitBlt(SRCCOPY|CAPTUREBLT)     把屏幕像素拷到位图，源坐标 = 虚拟桌面左上角
//   DrawIconEx                     可选：把当前鼠标光标叠到位图上（BitBlt 不含光标）
//   GetDIBits(32bpp, biHeight < 0) 按 top-down BGRA 读回像素
//   alpha 置 255                   屏幕 DC 没有 alpha，留 0 会让 PNG 整张透明
//   按 dst->stride 逐行写           stride 可负 / 可大于 width*4，绝不假设连续
//   逆序释放句柄                    DeleteObject / DeleteDC / ReleaseDC
//
// DPI 保证：宿主未必在进程清单里声明 DPI aware（也可能是从别的入口被拉起的），
// 所以本文件不信任宿主的 DPI 状态——queryGeometry / captureTo 每次进入都显式把
// 调用线程的 DPI 上下文提到 PER_MONITOR_AWARE_V2，让 GetSystemMetrics 与 BitBlt
// 落在同一套物理像素坐标系里；设置失败只记 warn、保持原上下文继续采集。
// 实测（主屏 150% 2560x1600 @0,0 + 副屏 100% 1024x768 @2560,0）：DPI-unaware 线程
// 看到的虚拟桌面是 3584x1067，提升后才是物理值 3584x1600。混合 DPI 下 DWM 会按
// 每块屏各自的缩放虚拟化，宽度也不一定是物理值，这正是不能依赖宿主 DPI 状态的原因。

#include "screen_capture_impl.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

// 老 SDK 的 winuser.h 里可能还没有 PER_MONITOR_AWARE_V2 这个上下文（Win10 1703 才有）：
// 它的值固定是 -4，运行期由系统解释，这里补一个定义，别让 SDK 版本卡住编译。
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 \
    reinterpret_cast<DPI_AWARENESS_CONTEXT>(static_cast<INT_PTR>(-4))
#endif

namespace screen_capture {
namespace {

// SetThreadDpiAwarenessContext 是 Win10 1607（1703 才有 PMv2 上下文）之后才有的导出
// 函数。静态导入它会让插件在更老的系统上连 DLL 都加载不起来，所以运行期从 user32
// 解析；指针为空就退化成"不改变线程 DPI 上下文"，采集继续（见下面的 RAII 注释）。
using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);

SetThreadDpiAwarenessContextFn setThreadDpiAwarenessContextFn() {
    // user32 一定已经随本 DLL 的静态导入（GetDC/GetSystemMetrics）加载，不会碰磁盘。
    static const auto fn = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    return fn;
}

// 采集期间的线程 DPI 上下文守卫：构造时提到 PMv2，析构时恢复成调用前的值。
//
// 为什么不能只设一次进程级：插件收到的是宿主线程，进程可能已经 DPI aware，也可能是
// DPI-unaware（没有清单、被别的宿主加载、以后换入口）。只改线程上下文是插件唯一能
// 自己保证的、不影响宿主的部分；用完必须恢复，否则宿主这个线程后续的 GetSystemMetrics
// / 建窗口会突然按物理像素算，UI 布局会错位。
//
// 这个 RAII 小工具只服务于本文件的两段系统调用，属于实现细节，所以放匿名命名空间、
// 不放进 screen_capture_impl.h：内部头只该暴露"抓屏"这个职责，不该带上 DPI 上下文类型。
class ScopedThreadDpiAwareness {
public:
    explicit ScopedThreadDpiAwareness(shot::Logger log) : m_set(setThreadDpiAwarenessContextFn()) {
        if (m_set == nullptr) {
            // Win10 1607 之前没有这个函数：保持原上下文，采集按老坐标走（只 warn）。
            log.warn("screen_capture: 当前系统没有 SetThreadDpiAwarenessContext"
                     "（需要 Win10 1607+），按线程原有 DPI 上下文采集");
            return;
        }
        const DPI_AWARENESS_CONTEXT previous =
            m_set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (previous == nullptr) {
            // 1607 上 PMv2 这个值还不被接受：不改上下文，只把原因写进日志。
            char message[160] = {};
            std::snprintf(message, sizeof(message),
                          "screen_capture: SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2) "
                          "失败（GetLastError=%lu），按线程原有 DPI 上下文采集",
                          static_cast<unsigned long>(GetLastError()));
            log.warn(message);
            return;
        }
        m_previous = previous;
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

}  // namespace

bool GdiDesktopCapture::queryGeometry(ShotCaptureInfo& info, shot::Logger log) {
    // 必须在第一次 GetSystemMetrics 之前提升 DPI 上下文，否则 DPI-unaware 宿主线程
    // 拿到的是 DWM 虚拟化后的逻辑尺寸（混合 DPI 时连宽高比都不对）。
    const ScopedThreadDpiAwareness dpiAwareness(log);
    const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    // 远程会话断开 / 没有任何显示器时宽或高为 0：这是"没有可采集的屏幕"，不是失败。
    if (width <= 0 || height <= 0) {
        return false;
    }
    info.width = width;
    info.height = height;
    info.origin_x = originX;
    info.origin_y = originY;
    return true;
}

bool GdiDesktopCapture::captureTo(const ShotImage& dst, const ShotCaptureInfo& target,
                                  bool includeCursor, shot::Logger log) {
    // 调用方（ScreenCapture）已经保证尺寸一致；这里只按"C++ 里不信任入参"再兜一层。
    const int width = target.width;
    const int height = target.height;
    if (width <= 0 || height <= 0) {
        log.error("screen_capture: 抓屏尺寸非法");
        return false;
    }

    // 和 queryGeometry 一样：BitBlt 的源坐标/DIB 尺寸必须是同一套物理像素坐标系，
    // 所以这里也显式提升线程 DPI 上下文（describe 与 capture 可能在不同线程上）。
    const ScopedThreadDpiAwareness dpiAwareness(log);
    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        logFailure(log, "GetDC(nullptr)", GetLastError());
        return false;
    }

    HDC memoryDc = CreateCompatibleDC(screenDc);
    if (memoryDc == nullptr) {
        logFailure(log, "CreateCompatibleDC", GetLastError());
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
    if (bitmap == nullptr) {
        logFailure(log, "CreateCompatibleBitmap", GetLastError());
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    if (previous == nullptr || previous == HGDI_ERROR) {
        logFailure(log, "SelectObject", GetLastError());
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    // 源坐标用虚拟桌面左上角：主屏左上角不一定是 (0,0)，左侧或上方的副屏会让
    // origin 为负；BitBlt 的源坐标就是系统坐标，负坐标也照常工作。
    // CAPTUREBLT 把分层窗口一起抓进来（否则被遮挡的层窗口会丢内容），但它和
    // SRCCOPY 都不含鼠标光标，所以光标要在位图读回之前单独画上去。
    bool ok = BitBlt(memoryDc, 0, 0, width, height, screenDc, target.origin_x, target.origin_y,
                     SRCCOPY | CAPTUREBLT) != 0;
    if (!ok) {
        logFailure(log, "BitBlt", GetLastError());
    } else if (includeCursor) {
        // 必须画在 BitBlt 之后、GetDIBits 之前：此时位图还选在 memoryDc 里，
        // DrawIconEx 直接改的就是接下来要读回的像素。
        composeCursor(memoryDc, target, log);
    }

    // GetDIBits 要求位图没有被任何 DC 选中：先把原来的选中对象放回去。
    SelectObject(memoryDc, previous);

    if (ok) {
        ok = copyPixels(memoryDc, bitmap, dst, log);
    }

    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    return ok;
}

void GdiDesktopCapture::composeCursor(HDC memoryDc, const ShotCaptureInfo& target,
                                      shot::Logger log) {
    CURSORINFO cursor = {};
    cursor.cbSize = sizeof(CURSORINFO);  // 不填 cbSize，GetCursorInfo 会直接失败
    if (GetCursorInfo(&cursor) == 0) {
        logFailure(log, "GetCursorInfo", GetLastError());
        return;
    }
    // 光标此刻不可见（全屏视频/锁屏/远程会话/程序主动隐藏）：截图里本来就不该有光标。
    if ((cursor.flags & CURSOR_SHOWING) == 0) {
        return;
    }

    ICONINFO icon = {};
    if (GetIconInfo(cursor.hCursor, &icon) == 0) {
        logFailure(log, "GetIconInfo", GetLastError());
        return;
    }

    // 图像坐标 = 屏幕坐标 - 虚拟桌面原点：ptScreenPos 是系统坐标，origin 可能非零、
    // 甚至为负。两个数都是 int，但相减/减热点仍用 int64 做中间结果，避免极端坐标
    // （多屏摆出几万像素）下先溢出再取绝对值。
    const std::int64_t imageX = static_cast<std::int64_t>(cursor.ptScreenPos.x) - target.origin_x;
    const std::int64_t imageY = static_cast<std::int64_t>(cursor.ptScreenPos.y) - target.origin_y;
    // DrawIconEx 要的是图标左上角，ptScreenPos 是热点位置，必须减去热点偏移：
    // 箭头光标的热点是 (0,0)，但 I 型/十字光标不是，不减会整体画偏。
    const std::int64_t drawX = imageX - static_cast<std::int64_t>(icon.xHotspot);
    const std::int64_t drawY = imageY - static_cast<std::int64_t>(icon.yHotspot);
    // Win32 只收 int 坐标：把 64 位中间结果夹进 int 范围再传，不让窄化转换产生未定义行为。
    const int drawLeft = static_cast<int>(std::clamp<std::int64_t>(
        drawX, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    const int drawTop = static_cast<int>(std::clamp<std::int64_t>(
        drawY, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));

    // width/height 传 0 = 使用系统默认图标尺寸；DI_NORMAL 同时应用 XOR 掩码与彩色图标。
    if (DrawIconEx(memoryDc, drawLeft, drawTop, cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL) == 0) {
        logFailure(log, "DrawIconEx", GetLastError());
    }

    // GetIconInfo 成功时返回的掩码/彩色位图由调用方释放；hCursor 本身是系统光标，
    // 不归我们释放。无论 DrawIconEx 成功与否都必须释放，否则每次抓屏漏两个 GDI 对象，
    // 长时间常驻截图会累积。
    if (icon.hbmMask != nullptr) {
        DeleteObject(icon.hbmMask);
    }
    if (icon.hbmColor != nullptr) {
        DeleteObject(icon.hbmColor);
    }
}

bool GdiDesktopCapture::copyPixels(HDC deviceDc, HBITMAP bitmap, const ShotImage& dst,
                                   shot::Logger log) {
    const std::int32_t width = dst.width;
    const std::int32_t height = dst.height;
    const std::int64_t rowBytes = static_cast<std::int64_t>(width) * 4;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    // 负高度 = top-down DIB：读回的第一行就是屏幕最上面一行，与 ShotImage 的行序
    // 一致（第 0 行在 data），这样正向 stride 不用再翻转一次。
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;  // 32bpp；BI_RGB 下内存顺序就是 B、G、R、A
    info.bmiHeader.biCompression = BI_RGB;

    const std::int64_t stride = dst.stride;
    if (stride == rowBytes) {
        // 宿主最常见的就是紧密缓冲（stride = width*4）：GetDIBits 直接写进 dst->data，
        // 省掉一次整屏拷贝。
        if (GetDIBits(deviceDc, bitmap, 0, static_cast<UINT>(height), dst.data, &info,
                      DIB_RGB_COLORS) == 0) {
            logFailure(log, "GetDIBits", GetLastError());
            return false;
        }
        setAlphaOpaque(dst.data, width, height, stride);
        return true;
    }

    // 其它 stride（负 = 自底向上，或大于 width*4 = 行尾填充）：GetDIBits 只会写紧密
    // 连续的行，所以先落到临时缓冲，再按 dst->stride 逐行拷贝。
    const std::size_t bufferBytes = static_cast<std::size_t>(rowBytes) *
                                    static_cast<std::size_t>(height);
    // 截屏期间抛 bad_alloc 会越过 ABI 边界，所以这里用 nothrow 分配 + 判空。
    std::unique_ptr<std::uint8_t[]> scratch(new (std::nothrow) std::uint8_t[bufferBytes]);
    if (!scratch) {
        log.error("screen_capture: 分配行拷贝缓冲失败");
        return false;
    }
    if (GetDIBits(deviceDc, bitmap, 0, static_cast<UINT>(height), scratch.get(), &info,
                  DIB_RGB_COLORS) == 0) {
        logFailure(log, "GetDIBits", GetLastError());
        return false;
    }

    for (std::int32_t y = 0; y < height; ++y) {
        std::uint8_t* row = dst.data + static_cast<std::ptrdiff_t>(y) * stride;
        const std::uint8_t* source = scratch.get() + static_cast<std::ptrdiff_t>(y) * rowBytes;
        std::memcpy(row, source, static_cast<std::size_t>(rowBytes));
    }
    setAlphaOpaque(dst.data, width, height, stride);
    return true;
}

void GdiDesktopCapture::setAlphaOpaque(std::uint8_t* data, std::int32_t width, std::int32_t height,
                                       std::int64_t stride) {
    for (std::int32_t y = 0; y < height; ++y) {
        std::uint8_t* row = data + static_cast<std::ptrdiff_t>(y) * stride;
        for (std::int32_t x = 0; x < width; ++x) {
            row[x * 4 + 3] = 255;  // BGRA8 的第 4 字节是 A
        }
    }
}

void GdiDesktopCapture::logFailure(shot::Logger log, const char* step, DWORD error) {
    // 固定缓冲 + snprintf：系统调用已经失败时，不再引入一次可能抛异常的字符串分配。
    char message[160] = {};
    std::snprintf(message, sizeof(message), "screen_capture: GDI %s 失败（GetLastError=%lu）", step,
                  static_cast<unsigned long>(error));
    log.error(message);
}

}  // namespace screen_capture
