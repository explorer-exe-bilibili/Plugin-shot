// 长截图选区边框的 Windows 实现：一个只画 2px 描边的置顶窗口。
//
// 窗口样式（需求硬约束）：
//   · WS_POPUP + WS_EX_TOOLWINDOW：没有标题栏、不进 Alt+Tab；
//   · WS_EX_TOPMOST：始终压在目标窗口上方；
//   · WS_EX_NOACTIVATE：显示与命中都不抢焦点，目标窗口保持前台（用户还要继续滚动）；
//   · WS_EX_TRANSPARENT：鼠标事件穿透，描边不挡用户操作；
//   · ShowWindow(SW_SHOWNOACTIVATE)：显示时不激活。
//
// 只画边框：SetWindowRgn 把窗口区域设成"外矩形 - 内矩形"，中间是真正的空区域，
// WM_PAINT 用主题色填满整块客户区后就只剩 2px 描边。为什么不用 WS_EX_LAYERED +
// 半透明遮罩：半透明整窗会闪、会挡操作，在 DWM 合成下还可能留下黑帧；区域法只
// 保留描边，内容区边缘一个像素都不遮。
//
// 几何：窗口矩形 = 内容区外扩 kBorderWidth，全部是物理像素。创建线程先
// SetThreadDpiAwarenessContext(PMv2) 再建窗，保证窗口坐标与采集/框选坐标一致；
// 窗口创建完立即恢复线程原上下文（窗口自身的 DPI 上下文在创建时已确定）。
//
// 生命周期：窗口类属于进程、不随 DLL 卸载消失，stop() 必须 DestroyWindow +
// UnregisterClassW（AGENTS.md §5.6 / §7 第 6 条）；start() 与 stop() 只在会话
// 线程上成对调用，窗口不会跨线程销毁。
//
// 为什么不起独立消息线程：边框没有交互，只需要创建时同步画一次。会话线程不泵
// 消息，所以 ShowWindow 后立刻 UpdateWindow 把 WM_PAINT 同步走完，之后描边留在
// DWM 的重定向表面里。不为 2px 描边额外起线程，也不做 hide/show 抖动。

#include "border_window.h"

#include <cstdint>
#include <cstdio>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "shot_plugin_helpers.h"

// 老 SDK 的 winuser.h 里可能没有 PMv2 上下文（Win10 1703 才有）：值固定是 -4，
// 运行期由系统解释，这里补一个定义，别让 SDK 版本卡住编译。
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 \
    reinterpret_cast<DPI_AWARENESS_CONTEXT>(static_cast<INT_PTR>(-4))
#endif

namespace long_capture {
namespace {

/** @brief 窗口类名：进程内唯一；类不随 DLL 卸载消失，stop() 必须注销。 */
constexpr wchar_t kWindowClass[] = L"PluginShotLongCaptureBorder";

/** @brief 窗口标题（无边框，只在排障/任务管理器里可见）。 */
constexpr wchar_t kWindowTitle[] = L"Plugin-shot 长截图选区边框";

/** @brief SetThreadDpiAwarenessContext 的签名；运行期从 user32 解析，避免老系统加载失败。 */
using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);

/** @brief 取 user32 里的 SetThreadDpiAwarenessContext（系统没有时返回 nullptr）。 */
SetThreadDpiAwarenessContextFn setThreadDpiAwarenessContextFn() {
    static const auto function = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    return function;
}

/**
 * @brief 建窗期间的线程 DPI 上下文守卫：构造时提到 PMv2，析构时恢复。
 *
 * 会话线程不是宿主 main 线程，不能假设它继承了进程的 DPI 感知；用完必须恢复，
 * 否则这个线程后续的 GetSystemMetrics / 建窗口会突然按物理像素算（与
 * capture_windows.cpp 同一套兜底做法）。
 */
class ScopedThreadDpiAwareness {
public:
    ScopedThreadDpiAwareness() : m_set(setThreadDpiAwarenessContextFn()) {
        if (m_set == nullptr) {
            return;  // Win10 1607 之前没有这个入口：保持原上下文继续建窗
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

/** @brief 0xRRGGBB → COLORREF（GDI 的 RGB 宏是 0x00BBGGRR）。 */
COLORREF toColorRef(std::uint32_t rgb) noexcept {
    return RGB(static_cast<BYTE>((rgb >> 16) & 0xFFu), static_cast<BYTE>((rgb >> 8) & 0xFFu),
               static_cast<BYTE>(rgb & 0xFFu));
}

/** @brief 0xRRGGBB → "#RRGGBB"，只用于日志。 */
std::string colorToHex(std::uint32_t rgb) {
    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "#%06X", static_cast<unsigned>(rgb & 0xFFFFFFu));
    return buffer;
}

}  // namespace

std::uint32_t parseThemeColorRgb(std::string_view text, std::uint32_t fallback) {
    std::size_t index = 0;
    if (!text.empty() && text.front() == '#') {
        index = 1;
    }
    if (text.size() - index != 6) {
        return fallback;
    }
    std::uint32_t value = 0;
    for (; index < text.size(); ++index) {
        const char ch = text[index];
        int digit = -1;
        if (ch >= '0' && ch <= '9') {
            digit = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            digit = ch - 'a' + 10;
        } else if (ch >= 'A' && ch <= 'F') {
            digit = ch - 'A' + 10;
        } else {
            return fallback;
        }
        value = value * 16u + static_cast<std::uint32_t>(digit);
    }
    return value;
}

struct BorderWindow::Impl {
    /** @brief 宿主服务表，只用来写日志；可能为 nullptr。 */
    const ShotHostServices* host = nullptr;
    /** @brief 边框窗口句柄；nullptr = 当前没有窗口。 */
    HWND hwnd = nullptr;
    /** @brief 当前进程实例（窗口类注册用）。 */
    HINSTANCE instance = nullptr;
    /** @brief 描边颜色（GDI 的 COLORREF）。 */
    COLORREF color = RGB(76, 154, 255);
    /** @brief 本对象是否注册过窗口类；stop() 只注销自己注册的那一份。 */
    bool classRegistered = false;

    /** @brief 注册窗口类；同名类已存在时返回 false 并保留 GetLastError。 */
    bool registerWindowClass();

    /**
     * @brief 确保窗口类可用：同名类已存在时先注销再注册自己这一份。
     *
     * 进程里可能有上一次运行留下的同名类（类不随 DLL 卸载消失）。不清理的话，
     * 注册失败后窗口根本建不出来；清理动作只发生在开始会话时，代价可以忽略。
     */
    bool ensureWindowClass();

    /** @brief 注销本对象注册的窗口类。 */
    void unregisterWindowClass();

    /** @brief 把窗口区域设成"外矩形 - 内矩形"：只保留 kBorderWidth 宽的描边。 */
    bool applyBorderRegion(int width, int height);

    /** @brief 销毁窗口并注销窗口类；可重复调用。 */
    void stop();

    /** @brief 窗口过程：只处理绘制、擦背景与鼠标穿透。 */
    static LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
};

bool BorderWindow::Impl::registerWindowClass() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Impl::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    // 背景刷留空：整窗由 WM_PAINT 用主题色填满，系统擦背景只会多闪一下。
    windowClass.hbrBackground = nullptr;
    return RegisterClassExW(&windowClass) != 0;
}

bool BorderWindow::Impl::ensureWindowClass() {
    if (registerWindowClass()) {
        classRegistered = true;
        return true;
    }
    if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        shot::Logger(host).error("[long_capture] 注册选区边框窗口类失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
        return false;
    }
    // 进程里已有同名类：可能是上一次运行留下的悬空 WndProc，先注销再注册自己这一份。
    if (!UnregisterClassW(kWindowClass, instance) || !registerWindowClass()) {
        shot::Logger(host).error("[long_capture] 清理/重新注册选区边框窗口类失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
        return false;
    }
    classRegistered = true;
    shot::Logger(host).warn("[long_capture] 已清掉进程里遗留的同名选区边框窗口类并重新注册");
    return true;
}

void BorderWindow::Impl::unregisterWindowClass() {
    if (!classRegistered) {
        return;
    }
    classRegistered = false;
    if (!UnregisterClassW(kWindowClass, instance)) {
        shot::Logger(host).warn("[long_capture] 注销选区边框窗口类失败（错误码 " +
                                std::to_string(GetLastError()) + "）");
    }
}

bool BorderWindow::Impl::applyBorderRegion(int width, int height) {
    HRGN outer = CreateRectRgn(0, 0, width, height);
    if (outer == nullptr) {
        return false;
    }
    // 内容区宽高都 > 0（start 已校验），所以内矩形至少 1x1，不会退化成无效区域。
    const int innerRight = width - kBorderWidth;
    const int innerBottom = height - kBorderWidth;
    HRGN inner = CreateRectRgn(kBorderWidth, kBorderWidth, innerRight, innerBottom);
    if (inner == nullptr) {
        DeleteObject(outer);
        return false;
    }
    if (CombineRgn(outer, outer, inner, RGN_DIFF) == ERROR) {
        DeleteObject(inner);
        DeleteObject(outer);
        return false;
    }
    DeleteObject(inner);
    // SetWindowRgn 成功后区域归系统所有，不能再删；失败时仍由调用方负责释放。
    if (SetWindowRgn(hwnd, outer, TRUE) == 0) {
        DeleteObject(outer);
        return false;
    }
    return true;
}

void BorderWindow::Impl::stop() {
    if (hwnd != nullptr) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    unregisterWindowClass();
    instance = nullptr;
}

LRESULT CALLBACK BorderWindow::Impl::windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
    switch (msg) {
        case WM_PAINT: {
            // 客户区被窗口区域裁成 2px 描边，整块填主题色即可；不需要双缓冲。
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(wnd, &paint);
            if (dc != nullptr) {
                RECT client{};
                GetClientRect(wnd, &client);
                HBRUSH brush = CreateSolidBrush(self->color);
                if (brush != nullptr) {
                    FillRect(dc, &client, brush);
                    DeleteObject(brush);
                }
            }
            EndPaint(wnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // WM_PAINT 会填满整块描边，不需要系统擦背景（避免闪烁）
        case WM_NCHITTEST:
            return HTTRANSPARENT;  // 双保险：描边本身也不吃鼠标
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // 绝不因为点击/悬停激活自己
        default:
            return DefWindowProcW(wnd, msg, wparam, lparam);
    }
}

BorderWindow::BorderWindow(const ShotHostServices* host) : m_impl(std::make_unique<Impl>()) {
    m_impl->host = host;
}

BorderWindow::~BorderWindow() {
    // 析构与 start() 同线程：调用方把 BorderWindow 放在会话函数栈上即可。
    stop();
}

bool BorderWindow::start(const ScreenRect& content, std::uint32_t colorRgb) {
    stop();  // 防御：重复 start 时先收掉旧窗口，避免留下两个边框
    if (!content.valid()) {
        shot::Logger(m_impl->host).warn("[long_capture] 选区边框未显示：内容区为空");
        return false;
    }

    // 外扩 kBorderWidth：描边完全画在内容区之外，不遮内容边缘、也不进抓取范围。
    const int x = content.x - kBorderWidth;
    const int y = content.y - kBorderWidth;
    const int width = content.width + kBorderWidth * 2;
    const int height = content.height + kBorderWidth * 2;

    m_impl->color = toColorRef(colorRgb);
    {
        // 窗口坐标 = 物理像素：建窗/设区域前把线程 DPI 上下文兜到 PMv2，之后恢复。
        const ScopedThreadDpiAwareness dpiAwareness;
        m_impl->instance = GetModuleHandleW(nullptr);
        if (!m_impl->ensureWindowClass()) {
            m_impl->instance = nullptr;
            return false;
        }
        m_impl->hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kWindowClass,
            kWindowTitle, WS_POPUP, x, y, width, height, nullptr, nullptr, m_impl->instance,
            m_impl.get());
        if (m_impl->hwnd == nullptr) {
            shot::Logger(m_impl->host).error("[long_capture] 创建选区边框窗口失败（错误码 " +
                                             std::to_string(GetLastError()) + "）");
            m_impl->stop();
            return false;
        }
        if (!m_impl->applyBorderRegion(width, height)) {
            shot::Logger(m_impl->host).error("[long_capture] 设置选区边框区域失败（错误码 " +
                                             std::to_string(GetLastError()) + "）");
            m_impl->stop();
            return false;
        }
        // 直接以不激活方式显示，再同步画一次；不 hide/show，不做淡入淡出。
        ShowWindow(m_impl->hwnd, SW_SHOWNOACTIVATE);
        // 会话线程不泵消息：显式标脏 + UpdateWindow 让 WM_PAINT 在这里同步走完，
        // 否则边框要等到下一次（可能永远不会发生的）消息循环才会被画出来。
        InvalidateRect(m_impl->hwnd, nullptr, FALSE);
        UpdateWindow(m_impl->hwnd);
    }

    shot::Logger(m_impl->host).info("[long_capture] 选区边框 " + std::to_string(width) + "x" +
                                    std::to_string(height) + " @" + std::to_string(x) + "," +
                                    std::to_string(y) + "，颜色 " + colorToHex(colorRgb));
    return true;
}

void BorderWindow::stop() {
    if (m_impl != nullptr) {
        m_impl->stop();
    }
}

bool BorderWindow::active() const noexcept {
    return m_impl != nullptr && m_impl->hwnd != nullptr;
}

}  // namespace long_capture
