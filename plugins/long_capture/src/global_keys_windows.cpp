// 会话内按键的 Windows 实现：隐藏消息窗口 + 一条消息线程 + RegisterHotKey / 低级键盘钩子。
//
// 为什么所有注册都在一条专用消息线程上做：
//   · RegisterHotKey(NULL, ...) 把热键绑在"调用线程的消息队列"上，WM_HOTKEY 只会投给
//     那个线程；UnregisterHotKey 也要求由注册它的线程撤销。把注册、撤销、消息循环放在
//     同一个线程，跨线程撤销的失败与竞态就都不存在；
//   · 本插件只注册**会话按键**（Enter 保存 / Esc 取消），会话开始才注册、结束立刻注销，
//     不让"Enter 被全局吃掉"的影响延续到会话之外；
//   · 长期触发键（Ctrl+Alt+L）不在这里注册：hotkey 插件读取 long_capture 贡献的
//     hotkey_list 后统一管理，本文件不再有 trigger 相关代码；
//   · RegisterHotKey 失败（被别的程序占用 / 系统策略）时退回 WH_KEYBOARD_LL：低级键盘
//     钩子同样只在本线程上安装和拆除，回调里只置原子标志 + 唤醒会话线程。
//
// 已知代价（README 里写明）：会话期间 VK_RETURN 被全局注册，任何程序里的 Enter 都
// 不会再到达它们自己。这是"Enter 保存"这个交互的必然代价；
// 想避免只能改用带修饰键的组合。
//
// 本文件包含 <windows.h>；公共头 global_keys.h 不出现任何 Win32 类型。

#include "global_keys.h"

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "shot_plugin_helpers.h"

namespace long_capture {
namespace {

// 只服务本文件的辅助函数与常量（STYLE.md §6.7：匿名命名空间内部辅助函数除外）。

/** @brief 窗口类名：进程内唯一；窗口类不随 DLL 卸载消失，线程退出前必须注销。 */
constexpr wchar_t kWindowClass[] = L"PluginShotLongCaptureKeys";

/** @brief 隐藏窗口标题：只排障用，不显示给用户，故不进 i18n。 */
constexpr wchar_t kWindowTitle[] = L"Plugin-shot 长截图按键";

/** @brief 线程消息：开始会话（注册 Enter / Esc，失败装低级钩子）。 */
constexpr UINT kMsgBeginSession = WM_APP + 1;
/** @brief 线程消息：结束会话（注销 Enter / Esc，拆钩子）。 */
constexpr UINT kMsgEndSession = WM_APP + 2;
/** @brief 线程消息：退出消息循环。 */
constexpr UINT kMsgQuit = WM_APP + 3;

/** @brief 热键 id：保存 / 取消，同一线程内唯一。 */
constexpr int kSaveHotkeyId = 1;
constexpr int kCancelHotkeyId = 2;

/** @brief Win32 VK_RETURN / VK_ESCAPE：契约头不包含 <windows.h>，这里用数值表达。 */
constexpr std::uint32_t kVkReturn = 0x0Du;
constexpr std::uint32_t kVkEscape = 0x1Bu;

}  // namespace

/**
 * @brief GlobalKeys 的实现：隐藏窗口、消息线程、会话热键与低级钩子的全部状态。
 *
 * 线程模型：start() 起线程并等窗口就绪；beginSession / endSession 可以在任意线程
 * 调用，只改期望状态 + PostMessage；真正的 RegisterHotKey / UnregisterHotKey /
 * SetWindowsHookEx 全部在消息线程上执行。
 */
struct GlobalKeys::Impl {
    const ShotHostServices* host = nullptr; /**< 只用于日志，可为 nullptr。 */
    std::thread thread;                     /**< 消息线程。 */
    std::mutex mutex;                       /**< 保护下面全部跨线程状态。 */
    std::condition_variable readyCv;        /**< start() 等窗口就绪。 */
    bool ready = false;                     /**< 窗口已创建。 */
    bool failed = false;                    /**< 建窗失败。 */
    bool accepting = false;                 /**< stop() 开始后拒绝新投递。 */
    HWND hwnd = nullptr;                    /**< 隐藏消息窗口。 */

    Callback saveCallback = nullptr;    /**< 会话内 Enter 回调。 */
    Callback cancelCallback = nullptr;  /**< 会话内 Esc 回调。 */
    void* sessionUser = nullptr;
    bool sessionActive = false;         /**< 会话热键/钩子是否生效。 */
    bool saveRegistered = false;
    bool cancelRegistered = false;
    HHOOK keyboardHook = nullptr;       /**< RegisterHotKey 失败时的兜底。 */
    static Impl* s_active;              /**< 低级钩子回调没有 user 参数，用它找回实例。 */

    void run();
    void messageLoop();
    void beginSessionOnThread();
    void endSessionOnThread();
    void unregisterSessionKeys();
    bool installKeyboardHook();
    void removeKeyboardHook();
    void handleHotkey(int id);

    bool registerWindowClass(HINSTANCE instance);
    bool ensureWindowClass(HINSTANCE instance);
    void unregisterWindowClass(HINSTANCE instance);

    static LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wparam, LPARAM lparam);
};

GlobalKeys::Impl* GlobalKeys::Impl::s_active = nullptr;

bool GlobalKeys::Impl::registerWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Impl::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = nullptr;  // 隐藏窗口从不绘制
    return RegisterClassExW(&windowClass) != 0;
}

bool GlobalKeys::Impl::ensureWindowClass(HINSTANCE instance) {
    if (registerWindowClass(instance)) {
        return true;
    }
    const DWORD registerError = GetLastError();
    if (registerError != ERROR_CLASS_ALREADY_EXISTS) {
        shot::Logger(host).error("[long_capture] 注册按键窗口类失败（错误码 " +
                                 std::to_string(registerError) + "）");
        return false;
    }
    // 进程里已有同名窗口类：可能是上一次运行（甚至已经 FreeLibrary 的旧 DLL）留下的，
    // lpfnWndProc 可能指向已卸载的代码。先注销再注册自己这一份，
    // 绝不沿用未知窗口过程。
    if (!UnregisterClassW(kWindowClass, instance)) {
        shot::Logger(host).error("[long_capture] 清理遗留按键窗口类失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
        return false;
    }
    if (!registerWindowClass(instance)) {
        shot::Logger(host).error("[long_capture] 清理遗留窗口类后重新注册失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
        return false;
    }
    shot::Logger(host).warn("[long_capture] 已清掉进程里遗留的同名按键窗口类并重新注册");
    return true;
}

void GlobalKeys::Impl::unregisterWindowClass(HINSTANCE instance) {
    if (UnregisterClassW(kWindowClass, instance)) {
        return;
    }
    shot::Logger(host).warn("[long_capture] 注销按键窗口类失败（错误码 " +
                            std::to_string(GetLastError()) + "）");
}

void GlobalKeys::Impl::run() {
    Impl::s_active = this;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    bool ok = ensureWindowClass(instance);
    const bool classRegistered = ok;
    HWND wnd = nullptr;
    if (ok) {
        wnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_POPUP, 0, 0, 0, 0, nullptr,
                              nullptr, instance, this);
        if (wnd == nullptr) {
            shot::Logger(host).error("[long_capture] 创建按键消息窗口失败");
            ok = false;
        }
    }
    if (ok) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            hwnd = wnd;
            ready = true;
        }
        readyCv.notify_all();
        shot::Logger(host).info("[long_capture] 会话按键线程就绪（Enter/Esc 只在会话期间注册）");
        messageLoop();
    }
    // 消息循环退出（stop）：注销会话热键/钩子、销毁窗口、注销窗口类，保证 DLL 被
    // FreeLibrary 后进程里不残留指向本模块代码的 WndProc / 钩子。
    unregisterSessionKeys();
    removeKeyboardHook();
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
    Impl::s_active = nullptr;
    shot::Logger(host).info("[long_capture] 会话按键线程已退出，窗口类与热键都已释放");
}

void GlobalKeys::Impl::messageLoop() {
    MSG msg{};
    for (;;) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            return;
        }
        if (msg.message == WM_HOTKEY) {
            // RegisterHotKey(NULL, ...) 的 WM_HOTKEY 没有窗口句柄，走线程消息队列；
            // 在 DispatchMessage 之前按 id 分派，避免把 hwnd=NULL 的消息丢掉。
            handleHotkey(static_cast<int>(msg.wParam));
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void GlobalKeys::Impl::handleHotkey(int id) {
    Callback callback = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!sessionActive) {
            return;  // 会话外的 WM_HOTKEY（迟到消息）：不回调
        }
        if (id == kSaveHotkeyId) {
            callback = saveCallback;
            user = sessionUser;
        } else if (id == kCancelHotkeyId) {
            callback = cancelCallback;
            user = sessionUser;
        }
    }
    if (callback != nullptr) {
        callback(user);
    }
}

void GlobalKeys::Impl::beginSessionOnThread() {
    unregisterSessionKeys();
    // 需求约定：会话期间 RegisterHotKey(NULL, id, 0, VK_RETURN / VK_ESCAPE)。
    // 两个键都注册成功才走热键路径；任何一个失败就把另一个也撤掉，
    // 整体退回低级钩子，否则会出现"Enter 走热键、Esc 走钩子"的双路径，容易重复触发。
    const bool saveOk = RegisterHotKey(nullptr, kSaveHotkeyId, 0, kVkReturn) != FALSE;
    const DWORD saveError = saveOk ? ERROR_SUCCESS : GetLastError();
    const bool cancelOk = RegisterHotKey(nullptr, kCancelHotkeyId, 0, kVkEscape) != FALSE;
    const DWORD cancelError = cancelOk ? ERROR_SUCCESS : GetLastError();
    if (saveOk && cancelOk) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            saveRegistered = true;
            cancelRegistered = true;
        }
        shot::Logger(host).info("[long_capture] 会话按键已注册：Enter 保存 / Esc 取消");
        return;
    }
    if (saveOk) {
        UnregisterHotKey(nullptr, kSaveHotkeyId);
    }
    if (cancelOk) {
        UnregisterHotKey(nullptr, kCancelHotkeyId);
    }
    shot::Logger(host).warn("[long_capture] 注册 Enter/Esc 热键失败（Enter 错误码 " +
                            std::to_string(saveError) + "，Esc 错误码 " +
                            std::to_string(cancelError) + "），改用低级键盘钩子");
    if (!installKeyboardHook()) {
        shot::Logger(host).error(
            "[long_capture] 低级键盘钩子也安装失败：本次会话只能用 HUD 按钮保存/取消");
    }
}

void GlobalKeys::Impl::endSessionOnThread() {
    unregisterSessionKeys();
    removeKeyboardHook();
}

void GlobalKeys::Impl::unregisterSessionKeys() {
    bool save = false;
    bool cancel = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        save = saveRegistered;
        cancel = cancelRegistered;
        saveRegistered = false;
        cancelRegistered = false;
    }
    if (save) {
        UnregisterHotKey(nullptr, kSaveHotkeyId);
    }
    if (cancel) {
        UnregisterHotKey(nullptr, kCancelHotkeyId);
    }
}

bool GlobalKeys::Impl::installKeyboardHook() {
    if (keyboardHook != nullptr) {
        return true;
    }
    // WH_KEYBOARD_LL 的钩子过程在本进程的线程上执行，hMod 可以传 NULL（MSDN 允许）。
    keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, &Impl::keyboardProc, nullptr, 0);
    if (keyboardHook == nullptr) {
        shot::Logger(host).error("[long_capture] SetWindowsHookExW(WH_KEYBOARD_LL) 失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
        return false;
    }
    shot::Logger(host).info("[long_capture] 已安装低级键盘钩子作为 Enter/Esc 兜底");
    return true;
}

void GlobalKeys::Impl::removeKeyboardHook() {
    if (keyboardHook == nullptr) {
        return;
    }
    UnhookWindowsHookEx(keyboardHook);
    keyboardHook = nullptr;
    shot::Logger(host).info("[long_capture] 已拆除低级键盘钩子");
}

LRESULT CALLBACK GlobalKeys::Impl::windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
            case kMsgBeginSession:
                self->beginSessionOnThread();
                return 0;
            case kMsgEndSession:
                self->endSessionOnThread();
                return 0;
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
        shot::Logger(self->host).warn("[long_capture] 按键窗口消息处理异常，已忽略该消息");
        return 0;
    }
    return DefWindowProcW(wnd, msg, wparam, lparam);
}

LRESULT CALLBACK GlobalKeys::Impl::keyboardProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code != HC_ACTION) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    if (wparam != WM_KEYDOWN && wparam != WM_SYSKEYDOWN) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    if (info == nullptr) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    // 钩子过程只认当前实例的状态；实例指针是 Impl 的静态成员（低级钩子回调没有
    // user 参数，同一进程同一时刻只允许一个 GlobalKeys 实例）。
    Impl* self = Impl::s_active;
    if (self == nullptr) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    Callback callback = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        if (!self->sessionActive) {
            return CallNextHookEx(nullptr, code, wparam, lparam);
        }
        if (info->vkCode == kVkReturn) {
            callback = self->saveCallback;
            user = self->sessionUser;
        } else if (info->vkCode == kVkEscape) {
            callback = self->cancelCallback;
            user = self->sessionUser;
        }
    }
    if (callback == nullptr) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    // 回调只允许置原子标志 + 唤醒会话线程（LongCapture 的实现满足这一点）。
    callback(user);
    return 1;  // 吞掉 Enter/Esc，避免同时传给目标窗口
}

GlobalKeys::GlobalKeys() : m_impl(std::make_unique<Impl>()) {}

GlobalKeys::~GlobalKeys() { stop(); }

bool GlobalKeys::start(const ShotHostServices* host) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->thread.joinable()) {
        return true;  // 已经在跑：重复 start 是幂等空操作
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->host = host;
        m_impl->ready = false;
        m_impl->failed = false;
        m_impl->accepting = true;
        m_impl->hwnd = nullptr;
        m_impl->saveRegistered = false;
        m_impl->cancelRegistered = false;
        m_impl->sessionActive = false;
    }
    try {
        m_impl->thread = std::thread(&Impl::run, m_impl.get());
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->accepting = false;
        shot::Logger(host).warn("[long_capture] 创建会话按键线程失败，Enter/Esc 不可用");
        return false;
    }

    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->readyCv.wait(lock, [this]() { return m_impl->ready || m_impl->failed; });
    return m_impl->ready && !m_impl->failed;
}

void GlobalKeys::stop() {
    if (!m_impl) {
        return;
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->accepting = false;
        m_impl->sessionActive = false;
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
        m_impl->saveRegistered = false;
        m_impl->cancelRegistered = false;
        m_impl->keyboardHook = nullptr;
    }
}

void GlobalKeys::beginSession(Callback onSave, Callback onCancel, void* user) {
    if (!m_impl) {
        return;
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->saveCallback = onSave;
        m_impl->cancelCallback = onCancel;
        m_impl->sessionUser = user;
        m_impl->sessionActive = true;
        wnd = m_impl->hwnd;
        if (!m_impl->accepting) {
            return;
        }
    }
    if (wnd != nullptr) {
        PostMessageW(wnd, kMsgBeginSession, 0, 0);
    }
}

void GlobalKeys::endSession() {
    if (!m_impl) {
        return;
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        // 先摘掉回调：endSession 返回后即使还有一条在途 WM_HOTKEY，也不会再回调会话。
        m_impl->sessionActive = false;
        m_impl->saveCallback = nullptr;
        m_impl->cancelCallback = nullptr;
        m_impl->sessionUser = nullptr;
        wnd = m_impl->hwnd;
    }
    if (wnd != nullptr) {
        PostMessageW(wnd, kMsgEndSession, 0, 0);
    }
}

}  // namespace long_capture
