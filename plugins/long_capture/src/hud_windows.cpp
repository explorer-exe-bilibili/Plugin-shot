// 长截图 HUD 的 Windows 实现：无边框置顶工具窗 + GDI 自绘 + 独立消息线程。
//
// 窗口样式（需求硬约束）：
//   · WS_POPUP + WS_EX_TOOLWINDOW：不进 Alt+Tab、没有标题栏；
//   · WS_EX_TOPMOST：始终在目标窗口之上；
//   · WS_EX_NOACTIVATE：点击按钮不抢焦点，目标窗口保持前台（用户还要继续滚动）；
//   · 摆放优先顺序：内容区右侧 → 左侧 → 下方 → 上方，与内容区留 12px、完全落在
//     显示器工作区内；四个方向都放不下就**不建窗口**，由调用方降级成托盘气泡。
//
// 绘制：GDI 双缓冲自绘（内存 DC + BitBlt）。缩略图是会话线程按 5Hz 生成的 RGBA
// 预览，HUD 线程把它转成 32bpp DIB，再用 StretchBlt + HALFTONE 缩到预览框里。
//
// 交互：
//   · 「保存」「取消」按钮：WM_LBUTTONUP 命中后直接调用回调（回调只置原子标志）；
//   · 拖动：背景区域按下后 SetCapture 手动搬窗口，松手前每一步都钳制在内容区外；
//   · 鼠标滚轮：转发给目标窗口，避免"滚到 HUD 上目标就不动了"。
//
// 生命周期（AGENTS.md §5.6）：窗口类属于进程、不随 DLL 卸载消失；线程退出前必须
// DestroyWindow + UnregisterClassW，否则禁用→启用会踩悬空 WndProc。
//
// 本文件包含 <windows.h>；公共头 hud.h 不出现任何 Win32 类型。

#include "hud.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include "shot_plugin_helpers.h"

namespace long_capture {
namespace {

// 只服务本文件的常量与辅助函数（STYLE.md §6.7）。

/** @brief 窗口类名：进程内唯一；类不随 DLL 卸载消失，线程退出前必须注销。 */
constexpr wchar_t kWindowClass[] = L"PluginShotLongCaptureHud";

/** @brief 隐藏窗口标题（无边框，只在排障/任务管理器里可见）。 */
constexpr wchar_t kWindowTitle[] = L"Plugin-shot 长截图 HUD";

/** @brief HUD 与内容区之间的最小间距（物理像素，需求约定 12px）。 */
constexpr int kContentMargin = 12;

/** @brief 逻辑尺寸：实际像素 = 逻辑尺寸 × DPI 缩放（只缩放 HUD，不缩放目标坐标）。 */
constexpr int kBaseWidth = 300;
constexpr int kBaseHeight = 250;

/** @brief 线程消息：状态/缩略图更新。 */
constexpr UINT kMsgUpdate = WM_APP + 1;
/** @brief 线程消息：显示保存成功/失败，并启动自动关闭定时器。 */
constexpr UINT kMsgFinal = WM_APP + 2;
/** @brief 线程消息：退出消息循环。 */
constexpr UINT kMsgQuit = WM_APP + 3;

/** @brief 自动关闭定时器 id。 */
constexpr UINT_PTR kCloseTimerId = 1;

/** @brief 保存成功后的展示时间（毫秒，需求约定 2 秒）。 */
constexpr UINT kSavedDisplayMs = 2000;
/** @brief 保存失败后的展示时间（毫秒）。 */
constexpr UINT kFailedDisplayMs = 2500;

/** @brief 按钮索引。 */
constexpr int kButtonNone = -1;
constexpr int kButtonSave = 0;
constexpr int kButtonCancel = 1;

/** @brief 颜色（0xRRGGBB；GDI 用 RGB 宏）。 */
constexpr COLORREF kBackgroundColor = RGB(31, 33, 38);
constexpr COLORREF kPanelColor = RGB(45, 48, 56);
constexpr COLORREF kBorderColor = RGB(76, 154, 255);
constexpr COLORREF kTextColor = RGB(236, 239, 244);
constexpr COLORREF kWarningColor = RGB(255, 183, 77);
constexpr COLORREF kSaveColor = RGB(76, 154, 255);
constexpr COLORREF kCancelColor = RGB(72, 76, 84);

/** @brief UTF-8 → UTF-16；非法字节由系统替换，不抛异常。 */
std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        length);
    return wide;
}

/** @brief 把模板里的一个占位符替换成文本（如 "{path}"）。 */
std::string replaceToken(std::string text, const char* token, const std::string& value) {
    if (token == nullptr || token[0] == '\0') {
        return text;
    }
    const std::string key(token);
    std::size_t position = text.find(key);
    while (position != std::string::npos) {
        text.replace(position, key.size(), value);
        position = text.find(key, position + value.size());
    }
    return text;
}

/** @brief 取当前系统 DPI 缩放（只用于 HUD 尺寸与字号，不参与任何坐标换算）。 */
double systemScale() {
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return 1.0;
    }
    const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    if (dpi <= 0) {
        return 1.0;
    }
    const double scale = static_cast<double>(dpi) / 96.0;
    return std::clamp(scale, 1.0, 2.5);
}

/** @brief 矩形是否完全落在工作区内。 */
bool fitsWorkArea(const RECT& rect, const RECT& work) {
    return rect.left >= work.left && rect.top >= work.top && rect.right <= work.right &&
           rect.bottom <= work.bottom;
}

/** @brief 把矩形限制在工作区内（宽高不变）。 */
void clampToWorkArea(RECT& rect, const RECT& work) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int workLeft = static_cast<int>(work.left);
    const int workTop = static_cast<int>(work.top);
    const int workRight = static_cast<int>(work.right);
    const int workBottom = static_cast<int>(work.bottom);
    rect.left = std::clamp(static_cast<int>(rect.left), workLeft,
                           std::max(workLeft, workRight - width));
    rect.top = std::clamp(static_cast<int>(rect.top), workTop,
                          std::max(workTop, workBottom - height));
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
}

/**
 * @brief 判断矩形是否在内容区之外且至少留 kContentMargin。
 * @param[in] rect    HUD 矩形。
 * @param[in] content 内容区（物理像素）。
 * @return true = 不侵占内容区。
 */
bool outsideContent(const RECT& rect, const RECT& content) {
    return rect.right <= content.left - kContentMargin ||
           rect.left >= content.right + kContentMargin ||
           rect.bottom <= content.top - kContentMargin ||
           rect.top >= content.bottom + kContentMargin;
}

/**
 * @brief 拖动时把 HUD 钳制在内容区外 + 工作区内；都放不下时保持原位。
 * @param[in,out] rect HUD 期望矩形（原地修正）。
 * @param[in]     work 工作区。
 * @param[in]     content 内容区。
 */
void clampOutsideContent(RECT& rect, const RECT& work, const RECT& content) {
    clampToWorkArea(rect, work);
    if (outsideContent(rect, content)) {
        return;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    // 优先按"离哪一侧更近"贴到内容区外；贴过去会超出工作区就换另一侧。
    const int leftX = static_cast<int>(content.left) - kContentMargin - width;
    const int rightX = static_cast<int>(content.right) + kContentMargin;
    const int topY = static_cast<int>(content.top) - kContentMargin - height;
    const int bottomY = static_cast<int>(content.bottom) + kContentMargin;
    const bool canLeft = leftX >= work.left;
    const bool canRight = rightX + width <= work.right;
    const bool canTop = topY >= work.top;
    const bool canBottom = bottomY + height <= work.bottom;

    const int distanceLeft = std::abs(rect.left - leftX);
    const int distanceRight = std::abs(rect.left - rightX);
    const int distanceTop = std::abs(rect.top - topY);
    const int distanceBottom = std::abs(rect.top - bottomY);
    const int best = std::min({canLeft ? distanceLeft : 1 << 30,
                               canRight ? distanceRight : 1 << 30,
                               canTop ? distanceTop : 1 << 30,
                               canBottom ? distanceBottom : 1 << 30});
    if (best == (1 << 30)) {
        return;  // 四周都放不下：保持原位，由用户自己拖回去
    }
    if (canLeft && distanceLeft == best) {
        rect.left = leftX;
    } else if (canRight && distanceRight == best) {
        rect.left = rightX;
    } else if (canTop && distanceTop == best) {
        rect.top = topY;
    } else {
        rect.top = bottomY;
    }
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
}

}  // namespace

/**
 * @brief Hud 的实现：窗口、线程、GDI 资源、缩略图与按钮/拖动状态。
 *
 * 线程模型：start() 起线程并等窗口就绪；会话线程通过 setStatus/setThumbnail/
 * showSaved/showFailure 投递更新（只拷数据 + PostMessage）；stop() 投递退出消息并 join。
 */
struct Hud::Impl {
    const ShotHostServices* host = nullptr;
    const shot::I18n* i18n = nullptr;
    ScreenRect content;
    std::uintptr_t targetHwnd = 0;
    ActionCallback onSave = nullptr;
    ActionCallback onCancel = nullptr;
    void* user = nullptr;

    std::thread thread;
    mutable std::mutex mutex;
    std::condition_variable readyCv;
    bool ready = false;
    bool failed = false;
    bool accepting = false;
    HWND hwnd = nullptr;

    // ---- 窗口几何（只在 HUD 线程读写；hudX/hudY 也由拖动更新） ----
    int hudX = 0;
    int hudY = 0;
    int hudWidth = 0;
    int hudHeight = 0;
    double scale = 1.0;
    RECT workArea{};
    RECT contentRect{};

    // ---- 会话线程投递的内容（mutex 保护） ----
    std::string statusMain;
    std::string statusWarning;
    std::vector<std::uint8_t> thumb;
    int thumbWidth = 0;
    int thumbHeight = 0;
    bool thumbDirty = false;
    int finalMode = 0;  // 0 = 无，1 = 已保存，2 = 失败
    std::string finalText;

    // ---- GDI 资源（只在 HUD 线程访问） ----
    HFONT titleFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT buttonFont = nullptr;
    HDC thumbDc = nullptr;
    HBITMAP thumbBitmap = nullptr;
    HGDIOBJ thumbOldBitmap = nullptr;
    int thumbDibWidth = 0;
    int thumbDibHeight = 0;
    UINT_PTR closeTimer = 0;

    // ---- 交互状态（只在 HUD 线程访问） ----
    int pressedButton = kButtonNone;
    bool dragging = false;
    POINT dragStartCursor{};
    POINT dragStartWindow{};

    void run();
    void messageLoop();
    bool computePlacement();
    bool registerWindowClass(HINSTANCE instance);
    bool ensureWindowClass(HINSTANCE instance);
    void unregisterWindowClass(HINSTANCE instance);
    void createFonts();
    void destroyFonts();
    void rebuildThumbDib();
    void releaseThumbDib();
    void paint();
    void paintButton(HDC dc, const RECT& rect, const std::wstring& text, COLORREF color);
    RECT saveButtonRect() const;
    RECT cancelButtonRect() const;
    void forwardWheel(WPARAM wparam);
    void showFinalOnThread();
    void invoke(ActionCallback callback);

    static LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
};

bool Hud::Impl::registerWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DROPSHADOW;
    windowClass.lpfnWndProc = &Impl::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = nullptr;  // 双缓冲自绘，不需要背景刷
    return RegisterClassExW(&windowClass) != 0;
}

bool Hud::Impl::ensureWindowClass(HINSTANCE instance) {
    if (registerWindowClass(instance)) {
        return true;
    }
    if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        shot::Logger(host).error("[long_capture] 注册 HUD 窗口类失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
        return false;
    }
    // 进程里已有同名类：可能是上一次运行留下的悬空 WndProc，先注销再注册自己这一份。
    if (!UnregisterClassW(kWindowClass, instance) || !registerWindowClass(instance)) {
        shot::Logger(host).error("[long_capture] 清理/重新注册 HUD 窗口类失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
        return false;
    }
    shot::Logger(host).warn("[long_capture] 已清掉进程里遗留的同名 HUD 窗口类并重新注册");
    return true;
}

void Hud::Impl::unregisterWindowClass(HINSTANCE instance) {
    if (!UnregisterClassW(kWindowClass, instance)) {
        shot::Logger(host).warn("[long_capture] 注销 HUD 窗口类失败（错误码 " +
                                std::to_string(GetLastError()) + "）");
    }
}

void Hud::Impl::createFonts() {
    const int titleHeight = -static_cast<int>(std::lround(16.0 * scale));
    const int bodyHeight = -static_cast<int>(std::lround(12.0 * scale));
    const int buttonHeight = -static_cast<int>(std::lround(13.0 * scale));
    const wchar_t* family = L"Microsoft YaHei UI";
    titleFont = CreateFontW(titleHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    bodyFont = CreateFontW(bodyHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, family);
    buttonFont = CreateFontW(buttonHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
}

void Hud::Impl::destroyFonts() {
    if (titleFont != nullptr) {
        DeleteObject(titleFont);
        titleFont = nullptr;
    }
    if (bodyFont != nullptr) {
        DeleteObject(bodyFont);
        bodyFont = nullptr;
    }
    if (buttonFont != nullptr) {
        DeleteObject(buttonFont);
        buttonFont = nullptr;
    }
}

void Hud::Impl::releaseThumbDib() {
    if (thumbDc != nullptr) {
        if (thumbBitmap != nullptr) {
            // 先把 DIB 从内存 DC 里换出来，否则 DeleteObject 会失败（位图仍被选中）。
            if (thumbOldBitmap != nullptr) {
                SelectObject(thumbDc, thumbOldBitmap);
                thumbOldBitmap = nullptr;
            }
            DeleteObject(thumbBitmap);
            thumbBitmap = nullptr;
        }
        DeleteDC(thumbDc);
        thumbDc = nullptr;
    } else if (thumbBitmap != nullptr) {
        DeleteObject(thumbBitmap);
        thumbBitmap = nullptr;
    }
    thumbDibWidth = 0;
    thumbDibHeight = 0;
}

void Hud::Impl::rebuildThumbDib() {
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pixels = thumb;
        width = thumbWidth;
        height = thumbHeight;
        thumbDirty = false;
    }
    releaseThumbDib();
    if (pixels.empty() || width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U) {
        return;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // 负数 = 自上而下，与 RGBA 行序一致
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(hwnd);
    HDC memory = CreateCompatibleDC(screen);
    if (screen != nullptr) {
        ReleaseDC(hwnd, screen);
    }
    if (memory == nullptr) {
        return;
    }
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
        return;
    }
    // 必须把 DIB 选进内存 DC：StretchBlt 的源 DC 读的是"当前选中的位图"，
    // 不选的话源 DC 还是默认的 1x1 单色位图，缩略图会是一片空白。
    thumbOldBitmap = SelectObject(memory, bitmap);
    if (thumbOldBitmap == nullptr || thumbOldBitmap == HGDI_ERROR) {
        thumbOldBitmap = nullptr;
        DeleteObject(bitmap);
        DeleteDC(memory);
        return;
    }
    auto* destination = static_cast<std::uint8_t*>(bits);
    // DIB 的 32bpp BI_RGB 内存布局是 B,G,R,A；预览是 R,G,B,A，逐像素换 R/B。
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* source =
            pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U;
        std::uint8_t* target =
            destination + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U;
        for (int x = 0; x < width; ++x) {
            target[x * 4 + 0] = source[x * 4 + 2];
            target[x * 4 + 1] = source[x * 4 + 1];
            target[x * 4 + 2] = source[x * 4 + 0];
            target[x * 4 + 3] = 255;
        }
    }
    thumbDc = memory;
    thumbBitmap = bitmap;
    thumbDibWidth = width;
    thumbDibHeight = height;
}

RECT Hud::Impl::saveButtonRect() const {
    const int padding = static_cast<int>(std::lround(10.0 * scale));
    const int gap = static_cast<int>(std::lround(8.0 * scale));
    const int height = static_cast<int>(std::lround(30.0 * scale));
    const int top = hudHeight - padding - height;
    const int width = (hudWidth - padding * 2 - gap) / 2;
    return RECT{padding, top, padding + width, top + height};
}

RECT Hud::Impl::cancelButtonRect() const {
    const RECT save = saveButtonRect();
    const int gap = static_cast<int>(std::lround(8.0 * scale));
    const int padding = static_cast<int>(std::lround(10.0 * scale));
    return RECT{save.right + gap, save.top, hudWidth - padding, save.bottom};
}

void Hud::Impl::paintButton(HDC dc, const RECT& rect, const std::wstring& text,
                            COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
              static_cast<int>(6.0 * scale), static_cast<int>(6.0 * scale));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HGDIOBJ oldFont = SelectObject(dc, buttonFont);
    RECT textRect = rect;
    DrawTextW(dc, text.c_str(), -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void Hud::Impl::paint() {
    PAINTSTRUCT paintStruct{};
    HDC windowDc = BeginPaint(hwnd, &paintStruct);
    if (windowDc == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        EndPaint(hwnd, &paintStruct);
        return;
    }

    HDC memory = CreateCompatibleDC(windowDc);
    HBITMAP buffer = CreateCompatibleBitmap(windowDc, width, height);
    if (memory == nullptr || buffer == nullptr) {
        if (memory != nullptr) {
            DeleteDC(memory);
        }
        if (buffer != nullptr) {
            DeleteObject(buffer);
        }
        EndPaint(hwnd, &paintStruct);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(memory, buffer);

    HBRUSH background = CreateSolidBrush(kBackgroundColor);
    FillRect(memory, &client, background);
    DeleteObject(background);

    const int padding = static_cast<int>(std::lround(10.0 * scale));
    const int gap = static_cast<int>(std::lround(8.0 * scale));
    const int titleHeight = static_cast<int>(std::lround(24.0 * scale));
    const int statusHeight = static_cast<int>(std::lround(18.0 * scale));
    const RECT saveRect = saveButtonRect();
    const RECT cancelRect = cancelButtonRect();

    // 标题
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, kTextColor);
    HGDIOBJ oldFont = SelectObject(memory, titleFont);
    RECT titleRect{padding, padding, width - padding, padding + titleHeight};
    const std::wstring title =
        toWide((i18n != nullptr) ? i18n->tr("long.hud.title", "长截图") : "长截图");
    DrawTextW(memory, title.c_str(), -1, &titleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(memory, oldFont);

    // 缩略图框
    const RECT thumbRect{padding, padding + titleHeight + gap, width - padding,
                         saveRect.top - gap - statusHeight * 2 - gap};
    HBRUSH panel = CreateSolidBrush(kPanelColor);
    FillRect(memory, &thumbRect, panel);
    DeleteObject(panel);
    HPEN border = CreatePen(PS_SOLID, 1, kBorderColor);
    HGDIOBJ oldPen = SelectObject(memory, border);
    HGDIOBJ oldBrush = SelectObject(memory, GetStockObject(NULL_BRUSH));
    Rectangle(memory, thumbRect.left, thumbRect.top, thumbRect.right, thumbRect.bottom);
    SelectObject(memory, oldBrush);
    SelectObject(memory, oldPen);
    DeleteObject(border);

    if (thumbDc != nullptr && thumbDibWidth > 0 && thumbDibHeight > 0) {
        const int availableWidth = thumbRect.right - thumbRect.left - padding * 2;
        const int availableHeight = thumbRect.bottom - thumbRect.top - padding * 2;
        if (availableWidth > 0 && availableHeight > 0) {
            const double fit = std::min(static_cast<double>(availableWidth) / thumbDibWidth,
                                        static_cast<double>(availableHeight) / thumbDibHeight);
            const int drawWidth = std::max(1, static_cast<int>(std::lround(thumbDibWidth * fit)));
            const int drawHeight = std::max(1, static_cast<int>(std::lround(thumbDibHeight * fit)));
            const int drawX = thumbRect.left + (thumbRect.right - thumbRect.left - drawWidth) / 2;
            const int drawY = thumbRect.top + (thumbRect.bottom - thumbRect.top - drawHeight) / 2;
            SetStretchBltMode(memory, HALFTONE);
            SetBrushOrgEx(memory, 0, 0, nullptr);
            StretchBlt(memory, drawX, drawY, drawWidth, drawHeight, thumbDc, 0, 0, thumbDibWidth,
                       thumbDibHeight, SRCCOPY);
        }
    } else {
        SetTextColor(memory, RGB(150, 155, 165));
        oldFont = SelectObject(memory, bodyFont);
        RECT waitingRect = thumbRect;
        const std::wstring waiting =
            toWide((i18n != nullptr) ? i18n->tr("long.hud.waiting", "等待画面…") : "等待画面…");
        DrawTextW(memory, waiting.c_str(), -1, &waitingRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(memory, oldFont);
    }

    // 状态行与告警行
    std::string mainText;
    std::string warningText;
    {
        std::lock_guard<std::mutex> lock(mutex);
        mainText = statusMain;
        warningText = statusWarning;
    }
    SetTextColor(memory, kTextColor);
    oldFont = SelectObject(memory, bodyFont);
    RECT mainRect{padding, thumbRect.bottom + gap, width - padding,
                  thumbRect.bottom + gap + statusHeight};
    DrawTextW(memory, toWide(mainText).c_str(), -1, &mainRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (!warningText.empty()) {
        SetTextColor(memory, kWarningColor);
        RECT warningRect{padding, mainRect.bottom, width - padding, mainRect.bottom + statusHeight};
        DrawTextW(memory, toWide(warningText).c_str(), -1, &warningRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(memory, oldFont);

    // 按钮
    const std::wstring saveText =
        toWide((i18n != nullptr) ? i18n->tr("long.hud.save", "保存") : "保存");
    const std::wstring cancelText =
        toWide((i18n != nullptr) ? i18n->tr("long.hud.cancel", "取消") : "取消");
    paintButton(memory, saveRect, saveText, kSaveColor);
    paintButton(memory, cancelRect, cancelText, kCancelColor);

    BitBlt(windowDc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(buffer);
    DeleteDC(memory);
    EndPaint(hwnd, &paintStruct);
}

void Hud::Impl::forwardWheel(WPARAM wparam) {
    const HWND target = reinterpret_cast<HWND>(targetHwnd);
    if (target == nullptr || IsWindow(target) == FALSE) {
        return;
    }
    // 滚轮消息的 lParam 是屏幕坐标；HUD 在内容区外，直接转发会让目标按"内容区外"
    // 的光标位置滚动。改成目标客户区中心，滚轮总是作用在内容上。
    RECT client{};
    if (GetClientRect(target, &client) == FALSE) {
        return;
    }
    POINT center{(client.left + client.right) / 2, (client.top + client.bottom) / 2};
    ClientToScreen(target, &center);
    PostMessageW(target, WM_MOUSEWHEEL, wparam, MAKELPARAM(center.x, center.y));
}

void Hud::Impl::invoke(ActionCallback callback) {
    if (callback != nullptr) {
        callback(user);
    }
}

void Hud::Impl::showFinalOnThread() {
    int mode = 0;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex);
        mode = finalMode;
        text = finalText;
    }
    if (mode == 0) {
        return;
    }
    std::string message;
    if (mode == 1) {
        const char* fallback = "已保存：{path}";
        const char* templateText =
            (i18n != nullptr) ? i18n->tr("long.hud.saved", fallback) : fallback;
        message = replaceToken(templateText, "{path}", text);
    } else {
        const char* fallback = "保存失败：{reason}";
        const char* templateText =
            (i18n != nullptr) ? i18n->tr("long.hud.failed", fallback) : fallback;
        message = replaceToken(templateText, "{reason}", text);
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        statusMain = message;
        statusWarning.clear();
    }
    if (closeTimer != 0) {
        KillTimer(hwnd, closeTimer);
    }
    closeTimer = SetTimer(hwnd, kCloseTimerId, (mode == 1) ? kSavedDisplayMs : kFailedDisplayMs,
                          nullptr);
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool Hud::Impl::computePlacement() {
    contentRect = RECT{content.x, content.y, content.right(), content.bottom()};
    const HMONITOR monitor = MonitorFromRect(&contentRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) {
        shot::Logger(host).warn("[long_capture] 取显示器工作区失败，HUD 不显示");
        return false;
    }
    workArea = info.rcWork;

    const int width = static_cast<int>(std::lround(kBaseWidth * scale));
    const int height = static_cast<int>(std::lround(kBaseHeight * scale));
    hudWidth = width;
    hudHeight = height;

    // 候选顺序：右 → 左 → 下 → 上；垂直于内容区方向尽量居中。
    const int centeredY = std::clamp(content.y + (content.height - height) / 2,
                                     static_cast<int>(workArea.top),
                                     std::max(static_cast<int>(workArea.top),
                                              static_cast<int>(workArea.bottom) - height));
    const int centeredX = std::clamp(content.x + (content.width - width) / 2,
                                     static_cast<int>(workArea.left),
                                     std::max(static_cast<int>(workArea.left),
                                              static_cast<int>(workArea.right) - width));
    const RECT candidates[] = {
        RECT{content.right() + kContentMargin, centeredY, content.right() + kContentMargin + width,
             centeredY + height},
        RECT{content.x - kContentMargin - width, centeredY, content.x - kContentMargin,
             centeredY + height},
        RECT{centeredX, content.bottom() + kContentMargin, centeredX + width,
             content.bottom() + kContentMargin + height},
        RECT{centeredX, content.y - kContentMargin - height, centeredX + width,
             content.y - kContentMargin},
    };
    for (const RECT& candidate : candidates) {
        if (fitsWorkArea(candidate, workArea) && outsideContent(candidate, contentRect)) {
            hudX = candidate.left;
            hudY = candidate.top;
            shot::Logger(host).info("[long_capture] HUD 位置 " + std::to_string(hudX) + "," +
                                    std::to_string(hudY) + " 尺寸 " + std::to_string(hudWidth) +
                                    "x" + std::to_string(hudHeight));
            return true;
        }
    }
    shot::Logger(host).warn("[long_capture] 内容区四周都放不下 HUD，降级为托盘气泡提示");
    return false;
}

void Hud::Impl::run() {
    scale = systemScale();
    if (!computePlacement()) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;
            ready = false;
        }
        readyCv.notify_all();
        return;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    bool ok = ensureWindowClass(instance);
    const bool classRegistered = ok;
    HWND wnd = nullptr;
    if (ok) {
        wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kWindowClass,
                              kWindowTitle, WS_POPUP, hudX, hudY, hudWidth, hudHeight, nullptr,
                              nullptr, instance, this);
        if (wnd == nullptr) {
            shot::Logger(host).error("[long_capture] 创建 HUD 窗口失败（错误码 " +
                                     std::to_string(GetLastError()) + "）");
            ok = false;
        }
    }
    if (ok) {
        createFonts();
        SetWindowPos(wnd, HWND_TOPMOST, hudX, hudY, hudWidth, hudHeight,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(wnd, SW_SHOWNOACTIVATE);
        {
            std::lock_guard<std::mutex> lock(mutex);
            hwnd = wnd;
            ready = true;
        }
        readyCv.notify_all();
        shot::Logger(host).info("[long_capture] HUD 已显示");
        messageLoop();
    }

    if (closeTimer != 0) {
        KillTimer(wnd, closeTimer);
        closeTimer = 0;
    }
    destroyFonts();
    releaseThumbDib();
    if (wnd != nullptr) {
        DestroyWindow(wnd);
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        hwnd = nullptr;
        ready = false;
    }
    if (classRegistered) {
        unregisterWindowClass(instance);
    }
    if (!ok) {
        std::lock_guard<std::mutex> lock(mutex);
        failed = true;
        readyCv.notify_all();
    }
    shot::Logger(host).info("[long_capture] HUD 线程已退出");
}

void Hud::Impl::messageLoop() {
    MSG msg{};
    for (;;) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK Hud::Impl::windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(wnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
        return DefWindowProcW(wnd, msg, wparam, lparam);
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(wnd, msg, wparam, lparam);
    }
    try {
        switch (msg) {
            case WM_PAINT:
                self->paint();
                return 0;
            case WM_ERASEBKGND:
                return 1;  // 双缓冲绘制，不需要系统擦背景（避免闪烁）
            case kMsgUpdate:
                if (self->thumbDirty) {
                    self->rebuildThumbDib();
                }
                InvalidateRect(wnd, nullptr, FALSE);
                return 0;
            case kMsgFinal:
                self->showFinalOnThread();
                return 0;
            case kMsgQuit:
                DestroyWindow(wnd);
                return 0;
            case WM_TIMER:
                if (wparam == kCloseTimerId) {
                    KillTimer(wnd, kCloseTimerId);
                    self->closeTimer = 0;
                    DestroyWindow(wnd);
                    return 0;
                }
                break;
            case WM_LBUTTONDOWN: {
                const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                RECT saveRect = self->saveButtonRect();
                RECT cancelRect = self->cancelButtonRect();
                if (PtInRect(&saveRect, point) != FALSE) {
                    self->pressedButton = kButtonSave;
                    SetCapture(wnd);
                    InvalidateRect(wnd, nullptr, FALSE);
                } else if (PtInRect(&cancelRect, point) != FALSE) {
                    self->pressedButton = kButtonCancel;
                    SetCapture(wnd);
                    InvalidateRect(wnd, nullptr, FALSE);
                } else {
                    self->dragging = true;
                    GetCursorPos(&self->dragStartCursor);
                    RECT windowRect{};
                    GetWindowRect(wnd, &windowRect);
                    self->dragStartWindow = POINT{windowRect.left, windowRect.top};
                    SetCapture(wnd);
                }
                return 0;
            }
            case WM_MOUSEMOVE:
                if (self->dragging) {
                    POINT cursor{};
                    GetCursorPos(&cursor);
                    RECT desired{self->dragStartWindow.x + (cursor.x - self->dragStartCursor.x),
                                 self->dragStartWindow.y + (cursor.y - self->dragStartCursor.y),
                                 0, 0};
                    desired.right = desired.left + self->hudWidth;
                    desired.bottom = desired.top + self->hudHeight;
                    clampOutsideContent(desired, self->workArea, self->contentRect);
                    self->hudX = desired.left;
                    self->hudY = desired.top;
                    SetWindowPos(wnd, HWND_TOPMOST, self->hudX, self->hudY, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE);
                }
                return 0;
            case WM_LBUTTONUP: {
                const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                if (self->dragging) {
                    self->dragging = false;
                    ReleaseCapture();
                } else if (self->pressedButton != kButtonNone) {
                    RECT targetRect = (self->pressedButton == kButtonSave)
                                          ? self->saveButtonRect()
                                          : self->cancelButtonRect();
                    const int pressed = self->pressedButton;
                    self->pressedButton = kButtonNone;
                    ReleaseCapture();
                    InvalidateRect(wnd, nullptr, FALSE);
                    if (PtInRect(&targetRect, point) != FALSE) {
                        if (pressed == kButtonSave) {
                            self->invoke(self->onSave);
                        } else {
                            self->invoke(self->onCancel);
                        }
                    }
                }
                return 0;
            }
            case WM_MOUSEWHEEL:
                self->forwardWheel(wparam);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            case WM_CLOSE:
                DestroyWindow(wnd);
                return 0;
            default:
                break;
        }
    } catch (...) {
        shot::Logger(self->host).warn("[long_capture] HUD 窗口消息处理异常，已忽略该消息");
        return 0;
    }
    return DefWindowProcW(wnd, msg, wparam, lparam);
}

Hud::Hud() : m_impl(std::make_unique<Impl>()) {}

Hud::~Hud() { stop(); }

bool Hud::start(const ShotHostServices* host, const shot::I18n* i18n, const ScreenRect& content,
                std::uintptr_t targetHwnd, ActionCallback onSave, ActionCallback onCancel,
                void* user) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->thread.joinable()) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->host = host;
        m_impl->i18n = i18n;
        m_impl->content = content;
        m_impl->targetHwnd = targetHwnd;
        m_impl->onSave = onSave;
        m_impl->onCancel = onCancel;
        m_impl->user = user;
        m_impl->ready = false;
        m_impl->failed = false;
        m_impl->accepting = true;
        m_impl->hwnd = nullptr;
        m_impl->statusMain.clear();
        m_impl->statusWarning.clear();
        m_impl->thumb.clear();
        m_impl->thumbWidth = 0;
        m_impl->thumbHeight = 0;
        m_impl->thumbDirty = false;
        m_impl->finalMode = 0;
        m_impl->finalText.clear();
    }
    try {
        m_impl->thread = std::thread(&Impl::run, m_impl.get());
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->accepting = false;
        shot::Logger(host).warn("[long_capture] 创建 HUD 线程失败，降级为托盘气泡提示");
        return false;
    }

    bool ready = false;
    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        m_impl->readyCv.wait(lock, [this]() { return m_impl->ready || m_impl->failed; });
        ready = m_impl->ready && !m_impl->failed;
    }
    if (!ready && m_impl->thread.joinable()) {
        m_impl->thread.join();  // 初始化失败时线程已经/即将退出，这里收干净
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->accepting = false;
    }
    return ready;
}

void Hud::stop() {
    if (!m_impl) {
        return;
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->accepting = false;
        wnd = m_impl->hwnd;
    }
    if (wnd != nullptr) {
        PostMessageW(wnd, kMsgQuit, 0, 0);
    }
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->hwnd = nullptr;
        m_impl->ready = false;
        m_impl->failed = false;
        m_impl->thumb.clear();
        m_impl->thumbDirty = false;
    }
}

bool Hud::active() const noexcept {
    if (!m_impl) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->accepting && m_impl->thread.joinable();
}

void Hud::setStatus(const std::string& main, const std::string& warning) {
    if (!m_impl) {
        return;
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->statusMain = main;
        m_impl->statusWarning = warning;
        wnd = m_impl->hwnd;
    }
    if (wnd != nullptr) {
        PostMessageW(wnd, kMsgUpdate, 0, 0);
    }
}

void Hud::setThumbnail(const std::uint8_t* rgba, int width, int height) {
    if (!m_impl || rgba == nullptr || width <= 0 || height <= 0) {
        return;
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->thumb.assign(rgba, rgba + static_cast<std::size_t>(width) *
                                              static_cast<std::size_t>(height) * 4U);
        m_impl->thumbWidth = width;
        m_impl->thumbHeight = height;
        m_impl->thumbDirty = true;
        wnd = m_impl->hwnd;
    }
    if (wnd != nullptr) {
        PostMessageW(wnd, kMsgUpdate, 0, 0);
    }
}

void Hud::showSaved(const std::string& utf8Path) {
    if (!m_impl) {
        return;
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->finalMode = 1;
        m_impl->finalText = utf8Path;
        wnd = m_impl->hwnd;
    }
    if (wnd != nullptr) {
        PostMessageW(wnd, kMsgFinal, 0, 0);
    }
}

void Hud::showFailure(const std::string& utf8Reason) {
    if (!m_impl) {
        return;
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->finalMode = 2;
        m_impl->finalText = utf8Reason;
        wnd = m_impl->hwnd;
    }
    if (wnd != nullptr) {
        PostMessageW(wnd, kMsgFinal, 0, 0);
    }
}

}  // namespace long_capture
