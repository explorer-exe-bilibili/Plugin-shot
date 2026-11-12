// 全局按键的 Windows 实现：隐藏消息窗口 + 一条消息线程 + RegisterHotKey / 低级键盘钩子。
//
// 为什么所有注册都在一条专用消息线程上做：
//   · RegisterHotKey(NULL, ...) 把热键绑在"调用线程的消息队列"上，WM_HOTKEY 只会投给
//     那个线程；UnregisterHotKey 也要求由注册它的线程撤销。把注册、撤销、消息循环放在
//     同一个线程，跨线程撤销的失败与竞态就都不存在；
//   · 触发键（配置项 hotkey）常驻，会话期间再额外注册 Enter / Esc；会话结束立刻注销，
//     不让"Enter 被全局吃掉"的影响延续到会话之外；
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
#include <string_view>
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

/** @brief 线程消息：按当前配置重注册触发键。 */
constexpr UINT kMsgApplyTrigger = WM_APP + 1;
/** @brief 线程消息：开始会话（注册 Enter / Esc，失败装低级钩子）。 */
constexpr UINT kMsgBeginSession = WM_APP + 2;
/** @brief 线程消息：结束会话（注销 Enter / Esc，拆钩子）。 */
constexpr UINT kMsgEndSession = WM_APP + 3;
/** @brief 线程消息：退出消息循环。 */
constexpr UINT kMsgQuit = WM_APP + 4;

/** @brief 热键 id：触发键 / 保存 / 取消，同一线程内唯一。 */
constexpr int kTriggerHotkeyId = 1;
constexpr int kSaveHotkeyId = 2;
constexpr int kCancelHotkeyId = 3;

/** @brief Win32 VK_RETURN / VK_ESCAPE：契约头不包含 <windows.h>，这里用数值表达。 */
constexpr std::uint32_t kVkReturn = 0x0Du;
constexpr std::uint32_t kVkEscape = 0x1Bu;

/** @brief F1 的 VK 数值：F1..F12 = 0x70..0x7B。 */
constexpr std::uint32_t kVkF1 = 0x70u;

/** @brief 语法只接受 F1..F12。 */
constexpr int kMaxFunctionKey = 12;

/** @brief 去掉两端空白；返回原字符串的子串视图（不分配内存）。 */
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/** @brief ASCII 小写化一个字符；非字母原样返回。 */
char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/** @brief ASCII 小写化整段文本。 */
std::string asciiLowerCopy(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        result.push_back(asciiLower(c));
    }
    return result;
}

/** @brief 解析主键：单个字母 A-Z、单个数字 0-9 或 F1..F12。 */
bool parseMainKey(std::string_view token, std::uint32_t& virtualKey, std::string& label) {
    if (token.size() == 1) {
        const char c = token.front();
        if (c >= 'a' && c <= 'z') {
            virtualKey = static_cast<std::uint32_t>(c - 'a' + 'A');  // VK 与 ASCII 大写一致
            label.assign(1, static_cast<char>(c - 'a' + 'A'));
            return true;
        }
        if (c >= '0' && c <= '9') {
            virtualKey = static_cast<std::uint32_t>(c);  // VK_0..VK_9 = '0'..'9'
            label.assign(1, c);
            return true;
        }
        return false;
    }
    if (token.size() < 2 || token.size() > 3 || token.front() != 'f') {
        return false;
    }
    int number = 0;
    for (std::size_t index = 1; index < token.size(); ++index) {
        const char c = token[index];
        if (c < '0' || c > '9') {
            return false;
        }
        number = number * 10 + (c - '0');
    }
    if (number < 1 || number > kMaxFunctionKey) {
        return false;
    }
    virtualKey = kVkF1 + static_cast<std::uint32_t>(number - 1);
    label = "F" + std::to_string(number);
    return true;
}

/** @brief 按固定顺序拼出规范化显示名。 */
std::string buildLabel(std::uint32_t modifiers, const std::string& mainLabel) {
    std::string label;
    if ((modifiers & kKeyModCtrl) != 0) {
        label += "Ctrl+";
    }
    if ((modifiers & kKeyModAlt) != 0) {
        label += "Alt+";
    }
    if ((modifiers & kKeyModShift) != 0) {
        label += "Shift+";
    }
    if ((modifiers & kKeyModWin) != 0) {
        label += "Win+";
    }
    label += mainLabel;
    return label;
}

/** @brief 把插件内的修饰键位掩码转成 Win32 的 MOD_*；extra 用来附加 MOD_NOREPEAT。 */
DWORD toWin32Modifiers(std::uint32_t modifiers, DWORD extra) {
    DWORD result = extra;
    if ((modifiers & kKeyModCtrl) != 0) {
        result |= MOD_CONTROL;
    }
    if ((modifiers & kKeyModAlt) != 0) {
        result |= MOD_ALT;
    }
    if ((modifiers & kKeyModShift) != 0) {
        result |= MOD_SHIFT;
    }
    if ((modifiers & kKeyModWin) != 0) {
        result |= MOD_WIN;
    }
    return result;
}

}  // namespace

bool parse_key_combo(const char* text, KeyCombo& out, std::string& error) {
    out = KeyCombo{};
    error.clear();
    if (text == nullptr) {
        error = "快捷键为空";
        return false;
    }
    const std::string_view input = trim(std::string_view(text));
    if (input.empty()) {
        error = "快捷键为空";
        return false;
    }

    std::uint32_t modifiers = 0;
    std::uint32_t virtualKey = 0;
    std::string mainLabel;
    bool haveMainKey = false;

    // 按 '+' 切分：修饰键可任意顺序、可重复；主键必须恰好一个。
    std::size_t start = 0;
    for (;;) {
        const std::size_t plus = input.find('+', start);
        const std::string_view token =
            trim(input.substr(start, plus == std::string_view::npos ? std::string_view::npos
                                                                    : plus - start));
        if (token.empty()) {
            error = "存在空的键位（多余的 +）";
            return false;
        }
        const std::string lower = asciiLowerCopy(token);
        if (lower == "ctrl" || lower == "control") {
            modifiers |= kKeyModCtrl;
        } else if (lower == "alt") {
            modifiers |= kKeyModAlt;
        } else if (lower == "shift") {
            modifiers |= kKeyModShift;
        } else if (lower == "win" || lower == "windows") {
            modifiers |= kKeyModWin;
        } else {
            std::uint32_t candidateKey = 0;
            std::string candidateLabel;
            if (!parseMainKey(lower, candidateKey, candidateLabel)) {
                error = "不认识的键位 \"" + std::string(token) + "\"";
                return false;
            }
            if (haveMainKey) {
                error = "只能有一个主键";
                return false;
            }
            virtualKey = candidateKey;
            mainLabel = std::move(candidateLabel);
            haveMainKey = true;
        }
        if (plus == std::string_view::npos) {
            break;
        }
        start = plus + 1;
    }

    if (!haveMainKey) {
        error = "缺少主键（A-Z / 0-9 / F1..F12）";
        return false;
    }
    out.modifiers = modifiers;
    out.virtualKey = virtualKey;
    out.label = buildLabel(modifiers, mainLabel);
    return true;
}

/**
 * @brief GlobalKeys 的实现：隐藏窗口、消息线程、热键与低级钩子的全部状态。
 *
 * 线程模型：start() 起线程并等窗口就绪；registerTrigger / beginSession / endSession
 * 可以在任意线程调用，只改期望状态 + PostMessage；真正的 RegisterHotKey /
 * UnregisterHotKey / SetWindowsHookEx 全部在消息线程上执行。
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

    std::string triggerText; /**< 期望的触发键文本（空串 = 不注册）。 */
    bool triggerRegistered = false;
    Callback triggerCallback = nullptr;
    void* triggerUser = nullptr;

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
    void applyTrigger();
    void beginSessionOnThread();
    void endSessionOnThread();
    void unregisterSessionKeys();
    void unregisterTrigger();
    bool installKeyboardHook();
    void removeKeyboardHook();
    void handleHotkey(int id);
    static void invoke(Callback callback, void* user);

    bool registerWindowClass(HINSTANCE instance);
    bool ensureWindowClass(HINSTANCE instance);
    void unregisterWindowClass(HINSTANCE instance);

    static LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wparam, LPARAM lparam);
};

GlobalKeys::Impl* GlobalKeys::Impl::s_active = nullptr;

void GlobalKeys::Impl::invoke(Callback callback, void* user) {
    if (callback != nullptr) {
        callback(user);
    }
}

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
        }
        applyTrigger();
        {
            std::lock_guard<std::mutex> lock(mutex);
            ready = true;
        }
        readyCv.notify_all();
        shot::Logger(host).info("[long_capture] 全局按键线程就绪");
        messageLoop();
    }

    // 消息循环退出（stop）：注销全部热键/钩子、销毁窗口、注销窗口类，保证 DLL 被
    // FreeLibrary 后进程里不残留指向本模块代码的 WndProc / 钩子。
    unregisterSessionKeys();
    unregisterTrigger();
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
    shot::Logger(host).info("[long_capture] 全局按键线程已退出，窗口类与热键都已释放");
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
            if (id == kTriggerHotkeyId) {
                callback = triggerCallback;
                user = triggerUser;
            }
        } else if (id == kSaveHotkeyId) {
            callback = saveCallback;
            user = sessionUser;
        } else if (id == kCancelHotkeyId) {
            callback = cancelCallback;
            user = sessionUser;
        }
    }
    invoke(callback, user);
}

void GlobalKeys::Impl::applyTrigger() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex);
        text = triggerText;
    }
    unregisterTrigger();
    if (text.empty()) {
        shot::Logger(host).info("[long_capture] 触发快捷键为空，只用托盘菜单启动");
        return;
    }
    KeyCombo combo;
    std::string error;
    if (!parse_key_combo(text.c_str(), combo, error)) {
        shot::Logger(host).warn("[long_capture] 触发快捷键配置非法（" + text + "）：" + error +
                                "，改用托盘菜单启动");
        return;
    }
    const DWORD modifiers = toWin32Modifiers(combo.modifiers, MOD_NOREPEAT);
    if (RegisterHotKey(nullptr, kTriggerHotkeyId, modifiers, combo.virtualKey) == FALSE) {
        shot::Logger(host).warn("[long_capture] 注册触发快捷键 " + combo.label +
                                " 失败（错误码 " + std::to_string(GetLastError()) +
                                "），可能被其它程序占用；改用托盘菜单启动");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        triggerRegistered = true;
    }
    shot::Logger(host).info("[long_capture] 已注册触发快捷键 " + combo.label);
}

void GlobalKeys::Impl::unregisterTrigger() {
    bool registered = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        registered = triggerRegistered;
        triggerRegistered = false;
    }
    if (registered) {
        UnregisterHotKey(nullptr, kTriggerHotkeyId);
    }
}

void GlobalKeys::Impl::beginSessionOnThread() {
    unregisterSessionKeys();
    // 需求约定：会话期间 RegisterHotKey(NULL, id, 0, VK_RETURN / VK_ESCAPE)。
    // 两个键都注册成功才走热键路径；任何一个失败就把另一个也撤掉，
    // 整体退回低级钩子，
    // 否则会出现"Enter 走热键、Esc 走钩子"的双路径，容易重复触发。
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
            case kMsgApplyTrigger:
                self->applyTrigger();
                return 0;
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
        m_impl->triggerRegistered = false;
        m_impl->saveRegistered = false;
        m_impl->cancelRegistered = false;
        m_impl->sessionActive = false;
    }
    try {
        m_impl->thread = std::thread(&Impl::run, m_impl.get());
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->accepting = false;
        shot::Logger(host).warn("[long_capture] 创建全局按键线程失败，快捷键不可用");
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
        m_impl->triggerRegistered = false;
        m_impl->saveRegistered = false;
        m_impl->cancelRegistered = false;
        m_impl->keyboardHook = nullptr;
    }
}

bool GlobalKeys::registerTrigger(const char* comboText) {
    if (!m_impl) {
        return false;
    }
    std::string text = (comboText != nullptr) ? comboText : "";
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->triggerText = text;
        wnd = m_impl->hwnd;
        if (!m_impl->accepting) {
            return false;
        }
    }
    if (wnd != nullptr) {
        PostMessageW(wnd, kMsgApplyTrigger, 0, 0);
    }
    return true;
}

void GlobalKeys::setTriggerCallback(Callback callback, void* user) {
    if (!m_impl) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->triggerCallback = callback;
    m_impl->triggerUser = user;
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
