// Windows GDI 平台层：只做系统调用，不判断契约返回码。
//
// 抓整个虚拟桌面的流水线：
//   GetDC(NULL)                    拿屏幕 DC（覆盖整个虚拟桌面）
//   CreateCompatibleDC/Bitmap      建内存 DC + 与屏幕同格式的位图
//   BitBlt(SRCCOPY|CAPTUREBLT)     把屏幕像素拷到位图，源坐标 = 虚拟桌面左上角
//   GetDIBits(32bpp, biHeight < 0) 按 top-down BGRA 读回像素
//   alpha 置 255                   屏幕 DC 没有 alpha，留 0 会让 PNG 整张透明
//   按 dst->stride 逐行写           stride 可负 / 可大于 width*4，绝不假设连续
//   逆序释放句柄                    DeleteObject / DeleteDC / ReleaseDC
//
// 进程若 DPI-unaware，GetSystemMetrics 与 BitBlt 都在同一套 DWM 虚拟化坐标里取，
// 两者一致所以像素仍然对得上，只是内容是被放大过的逻辑像素。

#include "screen_capture_impl.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace screen_capture {

bool GdiDesktopCapture::queryGeometry(ShotCaptureInfo& info) {
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
                                  shot::Logger log) {
    // 调用方（ScreenCapture）已经保证尺寸一致；这里只按"C++ 里不信任入参"再兜一层。
    const int width = target.width;
    const int height = target.height;
    if (width <= 0 || height <= 0) {
        log.error("screen_capture: 抓屏尺寸非法");
        return false;
    }

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
    // CAPTUREBLT 把分层窗口一起抓进来（否则被遮挡的层窗口会丢内容）。
    // TODO: 光标合成：BitBlt/CAPTUREBLT 都不会把鼠标光标画进结果，v1 契约也不描述
    // 光标位置；等 ABI 追加光标语义后再用 GetCursorInfo + DrawIconEx 叠加（要做热点
    // 坐标与 DPI 换算，不能直接画在 (0,0)）。
    bool ok = BitBlt(memoryDc, 0, 0, width, height, screenDc, target.origin_x, target.origin_y,
                     SRCCOPY | CAPTUREBLT) != 0;
    if (!ok) {
        logFailure(log, "BitBlt", GetLastError());
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
