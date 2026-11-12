// 托盘气泡提示的 Windows 实现：隐藏消息窗口 + 隐藏占位托盘图标 + Shell 气泡。
//
// 为什么需要它：HUD 在"内容区四周都放不下"时不显示（内容区几乎占满屏幕），此时
// 用户需要知道"现在可以滚动、Enter 保存 / Esc 取消"。托盘气泡是唯一不占内容区、
// 也不会被截进长图的提示方式。
//
// 为什么要有隐藏占位图标（真机实测，与 notify 插件同一结论）：
//   · 不做 NIM_ADD 直接 NIM_MODIFY(NIF_INFO)：Shell 返回失败，系统不显示气泡；
//   · NIM_ADD 带 NIF_STATE + NIS_HIDDEN：图标存在但通知区域不可见，NIF_INFO 正常显示。
//
// 线程模型：show() 起一个短生命周期线程并等图标添加完成；气泡 4 秒后由定时器收尾，
// 线程自己删除图标、销毁窗口、注销窗口类并退出。dismiss() 可提前收尾（join）。
//
// 本文件包含 <windows.h>/<shellapi.h>；公共头 tray_hint.h 不出现任何 Win32 类型。

#include "tray_hint.h"

#include <condition_variable>
#include <cstddef>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>

#include "shot_plugin_helpers.h"

namespace long_capture {
namespace {

// 只服务本文件的常量与辅助函数（STYLE.md §6.7）。

/** @brief 窗口类名：进程内唯一；类不随 DLL 卸载消失，线程退出前必须注销。 */
constexpr wchar_t kWindowClass[] = L"PluginShotLongCaptureHint";

/** @brief 隐藏窗口标题：只排障用，不显示给用户，故不进 i18n。 */
constexpr wchar_t kWindowTitle[] = L"Plugin-shot 长截图提示";

/** @brief 占位图标 id（同一窗口内唯一）。 */
constexpr UINT kIconId = 1;

/** @brief 气泡保留时间（毫秒）：足够读完一句"Enter 保存 / Esc 取消"。 */
constexpr UINT kBalloonTimeoutMs = 4000;

/** @brief 定时器 id：到点后收尾退出。 */
constexpr UINT_PTR kDismissTimerId = 1;

/** @brief 线程消息：退出消息循环。 */
constexpr UINT kMsgQuit = WM_APP + 1;

/** @brief UTF-8 → UTF-16；非法字节由 MultiByteToWideChar 替换，不抛异常。 */
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

/** @brief 中间截断：NIF_INFO 的 szInfo/szInfoTitle 是定长缓冲，超长必须安全截断。 */
std::wstring truncateMiddle(const std::wstring& text, std::size_t maxChars) {
    if (text.size() <= maxChars || maxChars <= 1) {
        return text.size() <= maxChars ? text : std::wstring(1, L'…');
    }
    const std::size_t head = maxChars * 3 / 5;
    const std::size_t tail = maxChars - head - 1;
    return text.substr(0, head) + L"…" + text.substr(text.size() - tail);
}

}  // namespace

/**
 * @brief TrayHint 的实现：窗口、线程、图标与握手状态。
 *
 * 线程模型：show() 起线程并等图标添加完成；线程阻塞在 GetMessage 上，定时器到点后
 * 自己 DestroyWindow → PostQuitMessage → 清理图标/窗口类 → 退出；dismiss() 用
 * PostMessage 提前唤醒并 join。
 */
struct TrayHint::Impl {
    const ShotHostServices* host = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable readyCv;
    bool ready = false;
    bool failed = false;
    bool accepting = false;
    HWND hwnd = nullptr;
    std::string title;
    std::string text;
    bool iconAdded = false;

    void run();
    void messageLoop();
    bool addIcon(HWND wnd);
    void removeIcon();
    bool registerWindowClass(HINSTANCE instance);
    bool ensureWindowClass(HINSTANCE instance);
    void unregisterWindowClass(HINSTANCE instance);
    static LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
};

bool TrayHint::Impl::registerWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Impl::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = nullptr;
    return RegisterClassExW(&windowClass) != 0;
}

bool TrayHint::Impl::ensureWindowClass(HINSTANCE instance) {
    if (registerWindowClass(instance)) {
        return true;
    }
    if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        shot::Logger(host).warn("[long_capture] 注册提示窗口类失败（错误码 " +
                                std::to_string(GetLastError()) + "）");
        return false;
    }
    // 进程里已有同名类：可能是上一次运行留下的悬空 WndProc，先注销再注册自己这一份。
    if (!UnregisterClassW(kWindowClass, instance) || !registerWindowClass(instance)) {
        shot::Logger(host).warn("[long_capture] 清理/重新注册提示窗口类失败（错误码 " +
                                std::to_string(GetLastError()) + "）");
        return false;
    }
    return true;
}

void TrayHint::Impl::unregisterWindowClass(HINSTANCE instance) {
    if (!UnregisterClassW(kWindowClass, instance)) {
        shot::Logger(host).debug("[long_capture] 注销提示窗口类失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
    }
}

bool TrayHint::Impl::addIcon(HWND wnd) {
    // 图标只是气泡的挂载点，用户看不到（NIS_HIDDEN）；优先用宿主应用图标。
    const HINSTANCE hostModule = GetModuleHandleW(nullptr);
    HICON icon = LoadIconW(hostModule, L"IDI_ICON1");
    if (icon == nullptr) {
        icon = LoadIconW(hostModule, MAKEINTRESOURCEW(1));
    }
    if (icon == nullptr) {
        icon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_INFORMATION));
    }
    if (icon == nullptr) {
        icon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    }
    if (icon == nullptr) {
        shot::Logger(host).warn("[long_capture] 加载托盘提示图标失败");
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = wnd;
    data.uID = kIconId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_STATE;
    data.hIcon = icon;
    wcsncpy_s(data.szTip, L"Plugin-shot 长截图", _TRUNCATE);
    data.dwState = NIS_HIDDEN;
    data.dwStateMask = NIS_HIDDEN;
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        shot::Logger(host).warn("[long_capture] 添加隐藏占位托盘图标失败（Shell_NotifyIconW NIM_ADD）");
        return false;
    }
    iconAdded = true;

    // 图标添加成功后提交气泡；标题/正文来自语言表，超长按中间截断。
    data.uFlags = NIF_INFO;
    const std::wstring wideTitle = truncateMiddle(toWide(title), 63);
    const std::wstring wideText = truncateMiddle(toWide(text), 255);
    wcsncpy_s(data.szInfoTitle, wideTitle.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, wideText.c_str(), _TRUNCATE);
    data.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    data.uTimeout = kBalloonTimeoutMs;
    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
        shot::Logger(host).warn("[long_capture] 提交托盘气泡失败（Shell_NotifyIconW NIM_MODIFY）");
    } else {
        shot::Logger(host).info("[long_capture] 托盘气泡已提交");
    }
    return true;
}

void TrayHint::Impl::removeIcon() {
    if (!iconAdded) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    iconAdded = false;
}

void TrayHint::Impl::run() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    bool ok = ensureWindowClass(instance);
    const bool classRegistered = ok;
    HWND wnd = nullptr;
    if (ok) {
        wnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_POPUP, 0, 0, 0, 0, nullptr,
                              nullptr, instance, this);
        if (wnd == nullptr) {
            shot::Logger(host).warn("[long_capture] 创建提示消息窗口失败");
            ok = false;
        }
    }
    if (ok) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            hwnd = wnd;
        }
        ok = addIcon(wnd);
    }

    if (!ok) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;
            ready = false;
            hwnd = nullptr;
        }
        readyCv.notify_all();
        if (wnd != nullptr) {
            DestroyWindow(wnd);
        }
        if (classRegistered) {
            unregisterWindowClass(instance);
        }
        return;
    }

    SetTimer(wnd, kDismissTimerId, kBalloonTimeoutMs, nullptr);
    {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
    }
    readyCv.notify_all();
    messageLoop();

    KillTimer(wnd, kDismissTimerId);
    removeIcon();
    DestroyWindow(wnd);
    {
        std::lock_guard<std::mutex> lock(mutex);
        hwnd = nullptr;
        ready = false;
    }
    if (classRegistered) {
        unregisterWindowClass(instance);
    }
}

void TrayHint::Impl::messageLoop() {
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

LRESULT CALLBACK TrayHint::Impl::windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
            case WM_TIMER:
                if (wparam == kDismissTimerId) {
                    DestroyWindow(wnd);
                    return 0;
                }
                break;
            case kMsgQuit:
                DestroyWindow(wnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
    } catch (...) {
        shot::Logger(self->host).warn("[long_capture] 提示窗口消息处理异常，已忽略该消息");
        return 0;
    }
    return DefWindowProcW(wnd, msg, wparam, lparam);
}

TrayHint::TrayHint() : m_impl(std::make_unique<Impl>()) {}

TrayHint::~TrayHint() { dismiss(); }

bool TrayHint::show(const ShotHostServices* host, const std::string& title,
                    const std::string& text) {
    if (!m_impl) {
        return false;
    }
    dismiss();  // 重复调用：先收掉上一条气泡，再显示新的
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->host = host;
        m_impl->title = title;
        m_impl->text = text;
        m_impl->ready = false;
        m_impl->failed = false;
        m_impl->accepting = true;
        m_impl->iconAdded = false;
    }
    try {
        m_impl->thread = std::thread(&Impl::run, m_impl.get());
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->accepting = false;
        shot::Logger(host).warn("[long_capture] 创建托盘提示线程失败，气泡不可用");
        return false;
    }

    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->readyCv.wait(lock, [this]() { return m_impl->ready || m_impl->failed; });
    return m_impl->ready && !m_impl->failed;
}

void TrayHint::dismiss() {
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
        m_impl->iconAdded = false;
    }
}

bool TrayHint::active() const noexcept {
    if (!m_impl) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->accepting && m_impl->thread.joinable();
}

}  // namespace long_capture
