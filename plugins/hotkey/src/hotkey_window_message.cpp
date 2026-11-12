// 热键线程主体：隐藏消息窗口、消息循环、暂停状态与 WM_HOTKEY 分发。
//
// 为什么需要一个窗口：RegisterHotKey 可以绑定一个窗口，热键按下时系统把
// WM_HOTKEY 投递给这个窗口所在的线程；与其在裸线程消息队列里自己区分消息，
// 不如建一个永远不显示的窗口（没有 WS_VISIBLE）当消息泵：
//   · WM_HOTKEY   → 按 id 找到对应动作，往事件总线发布一条事件；
//   · kMsgReapply → 目录/配置变了：交给 hotkey_window_register.cpp 整批重注册；
//   · kMsgSetSuspend → 设置页录入组合键：撤销全部注册（暂停）或按配置恢复；
//   · kMsgQuit    → stop() 请求退出：撤掉全部注册后退出消息循环；
//   · WM_DESTROY  → 兜底退出。
//
// 热键注册与撤销都在热键线程上做：UnregisterHotKey 要求"由注册它的线程撤销"，
// 把两者放在同一个线程可以彻底避开跨线程撤销的失败与竞态。具体注册逻辑在
// hotkey_window_register.cpp（按 hotkey_list 快照顺序，先到者生效）。

#include "hotkey_window_impl.h"

#include <string>
#include <string_view>

#include "hotkey_config.h"
#include "shot_plugin_helpers.h"

namespace hotkey {
namespace {

constexpr wchar_t kWindowClass[] = L"PluginShotHotkeyWnd"; /**< 进程内唯一；类不随 DLL 卸载消失，必须显式注销。 */
constexpr wchar_t kWindowTitle[] = L"Plugin-shot 热键";     /**< 隐藏窗口标题，只排障用、不显示给用户，故不进 i18n。 */

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
        // 启动时按当前目录快照注册一次：默认值，或宿主 apply_stored 之后更新的已存值。
        // 注册会分配字符串（动态条目数量/负载不是编译期常量），线程入口不能抛异常。
        try {
            applyHotkeys();
        } catch (...) {
            shot::Logger(host).warn("[hotkey] 启动时注册热键发生异常，本轮按没有热键继续");
        }
        // 订阅先于线程就绪：启动期间到达的 hotkey.suspend 只置了 pending（那时还没有窗口
        // 可以投递消息）。注册完补一次，避免"设置页正在录入、热键却先注册上"。
        bool suspendPendingNow = false;
        bool suspendNow = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            suspendPendingNow = suspendPending;
            suspendNow = suspendValue;
            suspendPending = false;
        }
        if (suspendPendingNow) {
            try {
                setSuspended(suspendNow);
            } catch (...) {
                shot::Logger(host).warn("[hotkey] 应用暂停/恢复状态失败，已保持启动时的注册状态");
            }
        }
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
    try {
        unregisterAllHotkeys();
    } catch (...) {
        shot::Logger(host).warn("[hotkey] 线程退出时撤销热键发生异常，继续释放窗口");
    }
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
        case kMsgSetSuspend: {
            // 一次唤醒可能合并了多条暂停/恢复通知：以共享状态里的"最后一次期望"为准。
            // 这样 suspend→resume 紧挨着到达时，消息循环处理到第二条时 pending 已是
            // resume，最终一定回到"按配置注册"（不会停在暂停）。
            bool pending = false;
            bool want = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                pending = suspendPending;
                want = suspendValue;
                suspendPending = false;
            }
            if (pending) {
                setSuspended(want);
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
    const std::string_view topicView(topic);
    try {
        if (topicView == kTopicConfigApplied) {
            self->requestReapply();
        } else if (topicView == kTopicHotkeySuspend) {
            self->requestSuspend(true);
        } else if (topicView == kTopicHotkeyResume) {
            self->requestSuspend(false);
        }
        // 其它主题安静忽略：事件总线是共享的，不认识的主题不是错误。
    } catch (...) {
        shot::Logger(self->host).warn("[hotkey] 处理事件总线通知失败（内存不足？），已忽略这一条");
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

void HotkeyWindow::Impl::requestSuspend(bool suspend) {
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!acceptingEvents) {
            // 还没启动 / 正在关闭：启动时会按最新配置注册，这里不需要"暂停"。
            return;
        }
        suspendPending = true;
        suspendValue = suspend;
        wnd = hwnd;
    }
    if (wnd != nullptr && PostMessageW(wnd, kMsgSetSuspend, suspend ? 1 : 0, 0) == 0) {
        shot::Logger(host).warn("[hotkey] 唤醒热键线程失败（错误码 " +
                                std::to_string(GetLastError()) + "）");
    }
}

void HotkeyWindow::Impl::setSuspended(bool suspend) {
    if (suspended == suspend) {
        return;  // 幂等：重复的 suspend/resume 不重复撤/注册
    }
    suspended = suspend;
    if (suspend) {
        // 立即撤销：设置页已经进入"等待按键"，此刻用户按下的旧组合键不能再触发动作。
        unregisterAllHotkeys();
        shot::Logger(host).info("[hotkey] 收到 hotkey.suspend：已撤销全部热键（设置页正在录入组合键）");
        return;
    }
    // 恢复：重新解析当前配置再注册（录入期间配置可能被别处改过，不能复用旧注册表）。
    shot::Logger(host).info("[hotkey] 收到 hotkey.resume：按当前配置重新注册热键");
    applyHotkeys();
}

}  // namespace hotkey
