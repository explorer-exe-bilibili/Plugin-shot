// 热键线程主体：隐藏消息窗口、消息循环、热键注册与 WM_HOTKEY 事件发布。
//
// 为什么需要一个窗口：RegisterHotKey 可以绑定一个窗口，热键按下时系统把
// WM_HOTKEY 投递给这个窗口所在的线程；与其在裸线程消息队列里自己区分消息，
// 不如建一个永远不显示的窗口（没有 WS_VISIBLE）当消息泵：
//   · WM_HOTKEY   → 按 id 找到对应动作，往事件总线发布一条事件；
//   · kMsgReapply → 配置变了：先 UnregisterHotKey 全部，再按新配置逐项注册；
//   · kMsgQuit    → stop() 请求退出：撤掉全部注册后退出消息循环；
//   · WM_DESTROY  → 兜底退出。
//
// 热键注册与撤销都在热键线程上做：UnregisterHotKey 要求"由注册它的线程撤销"，
// 把两者放在同一个线程可以彻底避开跨线程撤销的失败与竞态。
//
// 非法/被占用的配置只 warn 跳过，不影响其它键，也不让 start() 失败——热键插件
// 少一个键仍能工作，宿主不该因为用户写错一个字符串就丢整个插件。

#include "hotkey_window_impl.h"

#include <string>
#include <string_view>

#include "hotkey_config.h"
#include "shot_plugin_helpers.h"

namespace hotkey {
namespace {

// 下面两个辅助函数只服务本文件的热键注册路径、与任何类无关，所以放匿名命名空间
// （STYLE.md §6.7：只在本 TU 用的具名函数）。
constexpr wchar_t kWindowClass[] = L"PluginShotHotkeyWnd"; /**< 进程内唯一；类不随 DLL 卸载消失，必须显式注销。 */
constexpr wchar_t kWindowTitle[] = L"Plugin-shot 热键";     /**< 隐藏窗口的标题，只用于排障。 */

/** @brief 把 hotkey::Modifier 转成 Win32 的 MOD_* 组合；额外加 MOD_NOREPEAT。 */
DWORD toWin32Modifiers(std::uint32_t modifiers) {
    DWORD result = 0;
    if ((modifiers & kModCtrl) != 0) {
        result |= MOD_CONTROL;
    }
    if ((modifiers & kModAlt) != 0) {
        result |= MOD_ALT;
    }
    if ((modifiers & kModShift) != 0) {
        result |= MOD_SHIFT;
    }
    if ((modifiers & kModWin) != 0) {
        result |= MOD_WIN;
    }
    // 按住不放只触发一次：全局热键最常见的用法是"按一下执行一个动作"，
    // 自动重复会在长按时连续截屏/连续打开设置。
    result |= MOD_NOREPEAT;
    return result;
}

/** @brief 注册失败的简短中文提示（错误码数值留给排障用）。 */
std::string registerFailureHint(DWORD error) {
    if (error == ERROR_HOTKEY_ALREADY_REGISTERED) {
        return "，该组合键已被其它程序占用";
    }
    if (error == ERROR_ACCESS_DENIED) {
        return "，可能被更高权限的程序占用";
    }
    return "";
}

}  // namespace

bool HotkeyWindow::Impl::registerWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Impl::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    // IDC_ARROW 在非 UNICODE 编译里展开成窄字符资源 ID；它本质是资源整数而不是
    // 真指针，所以按整数语义转换（与 plugins/tray 的做法一致）。
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = nullptr;  // 隐藏窗口从不绘制，不需要背景刷
    return RegisterClassExW(&windowClass) != 0;
}

bool HotkeyWindow::Impl::ensureWindowClass(HINSTANCE instance) {
    if (registerWindowClass(instance)) {
        return true;
    }
    const DWORD registerError = GetLastError();
    if (registerError != ERROR_CLASS_ALREADY_EXISTS) {
        shot::Logger(host).error("[hotkey] 注册热键窗口类失败（错误码 " +
                                 std::to_string(registerError) + "）");
        return false;
    }
    // 进程里已经有一个同名窗口类：它可能是上一次运行（甚至已经 FreeLibrary 的旧
    // DLL）留下的，lpfnWndProc 可能已经指向被卸载的代码。绝不能直接沿用——先注销，
    // 再注册自己这一份；注销失败说明还有窗口在用它，只能失败返回。
    if (!UnregisterClassW(kWindowClass, instance)) {
        const DWORD unregisterError = GetLastError();
        shot::Logger(host).error("[hotkey] 清理遗留热键窗口类失败（错误码 " +
                                 std::to_string(unregisterError) + "），拒绝沿用未知窗口过程");
        return false;
    }
    if (!registerWindowClass(instance)) {
        shot::Logger(host).error("[hotkey] 清理遗留窗口类后重新注册失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
        return false;
    }
    shot::Logger(host).warn("[hotkey] 已清掉进程里遗留的同名窗口类并重新注册（禁用→启用）");
    return true;
}

void HotkeyWindow::Impl::unregisterWindowClass(HINSTANCE instance) {
    // 窗口类属于进程、不随 DLL 卸载消失：不注销的话，DLL 被 FreeLibrary 之后类里的
    // lpfnWndProc 就是悬空指针，下次（禁用→启用）重新加载再 CreateWindowExW 就会
    // 跳到已卸载的代码（plugins/tray 实测过 0xC000041D）。所以线程退出前必须显式注销。
    if (UnregisterClassW(kWindowClass, instance)) {
        return;
    }
    shot::Logger(host).warn("[hotkey] 注销热键窗口类失败（错误码 " +
                            std::to_string(GetLastError()) + "）");
}

void HotkeyWindow::Impl::run() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND wnd = nullptr;
    bool ok = ensureWindowClass(instance);
    // ensureWindowClass 成功 = 这个类由本实例注册/接管，退出时才有资格注销。
    const bool classRegistered = ok;

    if (ok) {
        // 不带 WS_VISIBLE：这是"只收消息"的隐藏窗口，用户看不到它，也不进 Alt+Tab。
        wnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_POPUP, 0, 0, 0, 0, nullptr,
                              nullptr, instance, this);
        if (wnd == nullptr) {
            shot::Logger(host).error("[hotkey] 创建热键消息窗口失败");
            ok = false;
        }
    }

    if (ok) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            hwnd = wnd;
        }
        // 启动时按当前配置注册一次：默认值，或宿主 apply_stored 之后更新的已存值。
        applyHotkeys();
    }

    if (!ok) {
        // 初始化失败：把半成品收掉再让 start() 看到 failed，之后线程自己退出。
        {
            std::lock_guard<std::mutex> lock(mutex);
            hwnd = nullptr;
        }
        if (wnd != nullptr) {
            DestroyWindow(wnd);
        }
        if (classRegistered) {
            unregisterWindowClass(instance);  // 类已注册但线程要退出：别把悬空 WndProc 留给下次加载
        }
        {
            std::lock_guard<std::mutex> lock(readyMutex);
            failed = true;
        }
        readyCv.notify_all();
        return;
    }

    // 窗口与启动时的热键都就绪了才握手；宿主可能在 init 返回后立刻发 config.applied，
    // 那之前到达的请求只置 pending，消息循环启动后统一处理。
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
    }
    readyCv.notify_all();
    shot::Logger(host).info("[hotkey] 热键线程就绪");

    messageLoop();

    // 消息循环退出（stop() 或宿主退出）：窗口与类的收尾放这里，保证本 DLL 被
    // FreeLibrary 后进程里不残留指向本模块代码的 WndProc。热键已在 kMsgQuit
    // 分支里撤销；这里再兜一次，防止窗口被外部销毁时漏撤。
    unregisterAllHotkeys();
    DestroyWindow(wnd);
    {
        std::lock_guard<std::mutex> lock(mutex);
        hwnd = nullptr;
    }
    unregisterWindowClass(instance);
    shot::Logger(host).info("[hotkey] 热键线程已退出，窗口类与热键都已释放");
}

void HotkeyWindow::Impl::messageLoop() {
    MSG msg{};
    for (;;) {
        // 热键插件不渲染任何东西：阻塞等消息，空闲期不占 CPU。
        // stop()/requestReapply() 都用 PostMessageW 唤醒这里，不要改成 PeekMessage 空转。
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK HotkeyWindow::Impl::windowProc(HWND wnd, UINT msg, WPARAM wparam,
                                                LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        // 窗口创建期间把 Impl* 挂到窗口上，之后所有消息都靠它找回来。
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(wnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
        // 必须继续交给 DefWindowProc：窗口标题等创建期字段就是在这里设置的
        // （STYLE.md §11 #18 踩过：直接 return TRUE 会让标题一直为空）。
        return DefWindowProcW(wnd, msg, wparam, lparam);
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(wnd, msg, wparam, lparam);
    }

    // 唯一的异常边界：WndProc 是 user32 调起的原生回调帧，C++ 异常不能穿过去
    // （STYLE.md §6.2；真穿过去会以 0xC000041D 结束进程）。handleMessage 里可能
    // 分配内存/调用标准库，所以所有分支统一在这里兜底。
    try {
        return self->handleMessage(wnd, msg, wparam, lparam);
    } catch (...) {
        shot::Logger(self->host).warn("[hotkey] 窗口消息处理异常，已忽略该消息");
        return 0;  // 已消费的消息按"处理过"返回，避免 DefWindowProc 再碰这份半成品状态
    }
}

LRESULT HotkeyWindow::Impl::handleMessage(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_HOTKEY:
            publishHotkey(static_cast<int>(wparam));
            return 0;
        case kMsgReapply: {
            bool pending = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                pending = reapplyPending;
                reapplyPending = false;
            }
            if (pending) {
                shot::Logger(host).info("[hotkey] 配置变化，重新解析并注册全部热键");
                applyHotkeys();
            }
            return 0;
        }
        case kMsgQuit:
            // stop() 请求退出：先撤掉全部注册，再结束消息循环。
            unregisterAllHotkeys();
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            {
                std::lock_guard<std::mutex> lock(mutex);
                hwnd = nullptr;
            }
            PostQuitMessage(0);  // 兜底：万一窗口被外部销毁，消息循环也能退出
            return 0;
        default:
            break;
    }
    return DefWindowProcW(wnd, msg, wparam, lparam);
}

void HotkeyWindow::Impl::onBusEvent(void* user, const char* topic, const void* /*data*/,
                                    std::uint32_t /*len*/) {
    auto* self = static_cast<Impl*>(user);
    if (self == nullptr || topic == nullptr) {
        return;
    }
    // 回调运行在宿主分发线程上：只做"置 pending + PostMessage 唤醒"，
    // 不解析、不 RegisterHotKey（EVENT_BUS.md §3 的回调规则）。
    if (std::string_view(topic) != kTopicConfigApplied) {
        return;  // 只订阅了这一个主题，防御未来改订阅时漏过滤
    }
    try {
        self->requestReapply();
    } catch (...) {
        shot::Logger(self->host).warn("[hotkey] 收到 config.applied 后请求重注册失败（内存不足？）");
    }
}

void HotkeyWindow::Impl::requestReapply() {
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!acceptingEvents) {
            return;  // 还没启动 / 正在关闭：丢弃，启动时会按最新配置注册
        }
        reapplyPending = true;
        wnd = hwnd;
    }
    if (wnd != nullptr && PostMessageW(wnd, kMsgReapply, 0, 0) == 0) {
        shot::Logger(host).warn("[hotkey] 唤醒热键线程失败（错误码 " +
                                std::to_string(GetLastError()) + "）");
    }
}

void HotkeyWindow::Impl::applyHotkeys() {
    unregisterAllHotkeys();
    if (config == nullptr) {
        shot::Logger(host).warn("[hotkey] 没有配置对象，跳过热键注册");
        return;
    }
    if (!config->enabled()) {
        shot::Logger(host).info("[hotkey] 全局热键已禁用（enabled=0），不注册任何热键");
        return;
    }
    // 先在宿主线程外把三个值拷成副本，再逐项注册：读配置加锁、注册不持锁。
    const std::string capture = config->captureHotkey();
    const std::string region = config->regionHotkey();
    const std::string settings = config->settingsHotkey();
    registerOne(kHotkeyCapture, capture.c_str(), "截屏", kTopicCaptureRequest, nullptr);
    registerOne(kHotkeyRegion, region.c_str(), "区域截屏", kTopicCaptureRequest,
                kRegionCapturePayload);
    registerOne(kHotkeySettings, settings.c_str(), "打开设置", kTopicSettingsOpen, nullptr);
}

void HotkeyWindow::Impl::unregisterAllHotkeys() {
    for (int index = 0; index < kHotkeyCount; ++index) {
        RegisteredHotkey& slot = hotkeys[index];
        if (!slot.registered) {
            continue;
        }
        const int id = index + 1;
        if (!UnregisterHotKey(hwnd, id)) {
            shot::Logger(host).warn("[hotkey] 撤销热键失败（id=" + std::to_string(id) +
                                    "，错误码 " + std::to_string(GetLastError()) + "）");
        }
        slot = RegisteredHotkey{};
    }
}

bool HotkeyWindow::Impl::registerOne(int id, const char* text, const char* action,
                                     const char* topic, const char* payload) {
    if (id < 1 || id > kHotkeyCount) {
        return false;
    }
    RegisteredHotkey& slot = hotkeys[id - 1];
    slot = RegisteredHotkey{};

    Binding binding;
    std::string error;
    if (!parse_binding(text, binding, error)) {
        shot::Logger(host).warn(std::string("[hotkey] ") + action + "热键配置 \"" +
                                (text != nullptr ? text : "") + "\" 不合法（" + error +
                                "），已跳过这一项");
        return false;
    }
    const DWORD modifiers = toWin32Modifiers(binding.modifiers);
    if (!RegisterHotKey(hwnd, id, modifiers, binding.virtualKey)) {
        const DWORD registerError = GetLastError();
        shot::Logger(host).warn(std::string("[hotkey] ") + action + "热键 " + binding.label +
                                " 注册失败（错误码 " + std::to_string(registerError) +
                                registerFailureHint(registerError) + "），已跳过这一项");
        return false;
    }

    slot.registered = true;
    slot.modifiers = modifiers;
    slot.virtualKey = binding.virtualKey;
    slot.label = binding.label;
    slot.action = action;
    slot.topic = topic;
    slot.payload = payload;

    std::string message = std::string("[hotkey] 已注册") + action + "热键 " + binding.label +
                          " → " + topic;
    if (payload != nullptr) {
        message += "（负载 ";
        message += payload;
        message += "）";
    }
    shot::Logger(host).info(message);
    return true;
}

void HotkeyWindow::Impl::publishHotkey(int id) {
    if (id < 1 || id > kHotkeyCount) {
        return;  // 不是本插件注册的 id：安静忽略
    }
    const RegisteredHotkey& slot = hotkeys[id - 1];
    if (!slot.registered || slot.topic == nullptr) {
        // 注册被跳过/撤销之后才到达的 WM_HOTKEY：忽略，不当作错误。
        return;
    }
    shot::EventBus bus(host);
    const std::string_view payload = (slot.payload != nullptr) ? slot.payload : std::string_view();
    if (!bus.publish(slot.topic, payload)) {
        shot::Logger(host).warn(std::string("[hotkey] 发布 ") + slot.topic +
                                " 失败（宿主没有事件总线或队列已满）");
        return;
    }
    // 这条日志是验收锚点："发布 capture.request" 出现即说明热键通路打通。
    std::string message = std::string("[hotkey] 发布 ") + slot.topic + "（" + slot.action +
                          "热键 " + slot.label + "）";
    if (!payload.empty()) {
        message += "，负载 ";
        message += payload;
    }
    shot::Logger(host).info(message);
}

}  // namespace hotkey
