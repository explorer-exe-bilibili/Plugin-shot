// 通知线程主体：建隐藏消息窗口、加隐藏占位托盘图标、收事件。
//
// 为什么需要一个窗口：Shell_NotifyIconW 要把通知挂在一个窗口的某个图标上；这个
// 窗口永远不显示（没有 WS_VISIBLE），它只是通知线程的消息泵：
//   · kMsgDrain            → 事件队列有新内容（capture.done / capture.failed / app.quit）；
//   · kMsgQuit             → stop() 请求退出消息循环；
//   · kMsgNotifyIconEvent  → 图标的 Shell 回调（用户点了通知 = NIN_BALLOONUSERCLICK）；
//   · "TaskbarCreated"     → explorer.exe 重启后 Shell 丢了旧图标，重新 NIM_ADD。
//
// 为什么需要"隐藏的占位图标"（真机实测，Windows 11 26H2）：
//   · 不做 NIM_ADD 直接 NIM_MODIFY(NIF_INFO)：Shell 返回失败（E_FAIL），系统不显示；
//   · NIM_ADD 带 NIF_STATE + NIS_HIDDEN：图标存在但通知区域不可见，NIF_INFO 气泡
//     （Win10/11 上表现为系统通知）正常显示 —— 本插件采用这一种。
//   证据见 build/verify/notify_probe_*（本地临时探针，不进仓库）。
//
// 本文件只负责窗口 / 消息循环 / 隐藏图标；气泡文本与发送在 notify_toast_balloon.cpp，
// 点击行为在 notify_toast_click.cpp。所有函数都只在通知线程上执行（事件总线回调只调
// enqueue）。文本处理：事件负载与配置值按契约是 UTF-8；Win32 的宽字符 API 要 UTF-16，
// 统一在 toWide() 里转换（定义在 notify_toast_balloon.cpp）。

#include "notify_toast.h"

#include <string>
#include <utility>

#include "notify_toast_impl.h"
#include "shot_plugin_helpers.h"

namespace notify {

/** @brief 进程内唯一的隐藏窗口类名；类不随 DLL 卸载消失，必须显式注销。 */
constexpr wchar_t kWindowClass[] = L"PluginShotNotifyWnd";

// ---------------------------------------------------------------------------
// 窗口类注册与注销
// ---------------------------------------------------------------------------

bool NotifyToast::Impl::registerWindowClass(HINSTANCE instance) {
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

bool NotifyToast::Impl::ensureWindowClass(HINSTANCE instance) {
    if (registerWindowClass(instance)) {
        return true;
    }
    const DWORD registerError = GetLastError();
    if (registerError != ERROR_CLASS_ALREADY_EXISTS) {
        shot::Logger(host).error("[notify] 注册通知窗口类失败（错误码 " +
                                 std::to_string(registerError) + "）");
        return false;
    }
    // 进程里已经有一个同名窗口类：它可能是上一次运行（甚至已经 FreeLibrary 的旧 DLL）
    // 留下的，lpfnWndProc 可能已经指向被卸载的代码。绝不能直接沿用——先注销，再注册
    // 自己这一份（照抄 tray 踩过的 0xC000041D 教训：禁用→启用同一插件时进程里残留
    // 旧窗口类，直接沿用会跳到已卸载的代码）。
    if (!UnregisterClassW(kWindowClass, instance)) {
        const DWORD unregisterError = GetLastError();
        shot::Logger(host).error("[notify] 清理遗留通知窗口类失败（错误码 " +
                                 std::to_string(unregisterError) + "），拒绝沿用未知窗口过程");
        return false;
    }
    if (!registerWindowClass(instance)) {
        shot::Logger(host).error("[notify] 清理遗留窗口类后重新注册失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
        return false;
    }
    shot::Logger(host).warn("[notify] 已清掉进程里遗留的同名窗口类并重新注册（禁用→启用）");
    return true;
}

void NotifyToast::Impl::unregisterWindowClass(HINSTANCE instance) {
    // 窗口类属于进程、不随 DLL 卸载消失：不注销的话，DLL 被 FreeLibrary 之后类里的
    // lpfnWndProc 就是悬空指针，下次重新加载再 CreateWindowExW 会跳到已卸载的代码。
    if (UnregisterClassW(kWindowClass, instance)) {
        return;
    }
    shot::Logger(host).warn("[notify] 注销通知窗口类失败（错误码 " +
                            std::to_string(GetLastError()) + "）");
}

// ---------------------------------------------------------------------------
// 通知线程
// ---------------------------------------------------------------------------

void NotifyToast::Impl::run() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND wnd = nullptr;
    bool ok = ensureWindowClass(instance);
    // ensureWindowClass 成功 = 这个类由本实例注册/接管，退出时才有资格注销。
    const bool classRegistered = ok;

    if (ok) {
        // 不带 WS_VISIBLE：这是"只收消息"的隐藏窗口，用户看不到它，也不进 Alt+Tab。
        // 标题只用于排障，但同样走语言表，避免 DLL 里再留一份硬编码中文。
        const std::wstring title = toWide(localized("toast.window_title", "Plugin-shot 消息提示"));
        wnd = CreateWindowExW(0, kWindowClass, title.c_str(), WS_POPUP, 0, 0, 0, 0, nullptr,
                              nullptr, instance, this);
        if (wnd == nullptr) {
            shot::Logger(host).error("[notify] 创建通知消息窗口失败");
            ok = false;
        }
    }

    if (ok) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            hwnd = wnd;
        }
        // explorer.exe 重启后 Shell 会丢掉旧图标；注册这个广播消息，收到后重新 NIM_ADD。
        taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
        ok = addIcon(wnd);
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

    // 窗口与占位图标已就绪，先只做握手：start() 返回前入队的事件在消息循环里补处理。
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
    }
    readyCv.notify_all();
    shot::Logger(host).info(
        "[notify] 通知线程就绪：隐藏消息窗口 + 隐藏占位托盘图标（通知区域不显示）");

    messageLoop();

    // 消息循环退出（stop() 或 app.quit）：按"先删图标、再销毁窗口"的顺序收尾，
    // 因为 Shell 删除图标时还要用 hWnd 找到它。
    removeIcon(wnd);
    DestroyWindow(wnd);
    {
        std::lock_guard<std::mutex> lock(mutex);
        hwnd = nullptr;
    }
    unregisterWindowClass(instance);
    shot::Logger(host).info("[notify] 通知线程已退出，隐藏图标与窗口类都已删除");
}

void NotifyToast::Impl::messageLoop() {
    // 线程就绪前入队的事件（例如宿主 init 后立刻发布的 capture.done）在这里补处理。
    drainEvents();
    MSG msg{};
    for (;;) {
        // 通知线程不渲染任何东西：阻塞等消息，空闲期不占 CPU。
        // stop()/事件入队都用 PostMessageW 唤醒这里，不要改成 PeekMessage 空转。
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK NotifyToast::Impl::windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
    // （STYLE.md §6.2；真穿过去会以 0xC000041D 结束进程）。
    try {
        return self->handleMessage(wnd, msg, wparam, lparam);
    } catch (...) {
        shot::Logger(self->host).warn("[notify] 窗口消息处理异常，已忽略该消息");
        return 0;
    }
}

LRESULT NotifyToast::Impl::handleMessage(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // explorer.exe 重启：旧图标已随 Shell 消失，重新加（失败只 WARN，不影响消息循环）。
    if (taskbarCreatedMessage != 0 && msg == taskbarCreatedMessage) {
        iconAdded = false;
        if (!addIcon(wnd)) {
            shot::Logger(host).warn("[notify] explorer 重启后重新添加隐藏占位图标失败");
        }
        return 0;
    }
    switch (msg) {
        case kMsgDrain:
            drainEvents();
            return 0;
        case kMsgQuit:
            PostQuitMessage(0);
            return 0;
        case kMsgNotifyIconEvent:
            // 隐藏图标的 Shell 回调：用户点了通知时打开图片/文件夹（notify_toast_click.cpp）。
            onNotifyIconEvent(lparam);
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

// ---------------------------------------------------------------------------
// 事件接收与出队
// ---------------------------------------------------------------------------

void NotifyToast::Impl::onBusEvent(void* user, const char* topic, const void* data,
                                   std::uint32_t len) {
    auto* self = static_cast<Impl*>(user);
    if (self == nullptr || topic == nullptr) {
        return;
    }
    // 回调运行在宿主分发线程上：只做深拷贝 + PostMessage 唤醒通知线程，
    // 不阻塞、不碰任何 Win32/Shell 对象（EVENT_BUS.md §3 的回调规则）。
    // 回调是跨 DLL 的 C 边界，异常绝不能穿过去（STYLE.md §6.2）：分配失败就丢掉这条事件。
    try {
        self->enqueue(topic, static_cast<const char*>(data), len);
    } catch (...) {
        shot::Logger(self->host).warn("[notify] 接收事件失败（内存不足？），已丢弃该事件");
    }
}

void NotifyToast::Impl::enqueue(const char* topic, const char* data, std::uint32_t len) {
    if (topic == nullptr) {
        return;
    }
    std::string payload;
    if (data != nullptr && len > 0) {
        payload.assign(data, len);  // 契约要求：返回前把 data_len 字节拷走
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!acceptingEvents) {
            return;
        }
        queue.push(QueuedEvent{topic, std::move(payload)});
        wnd = hwnd;
    }
    if (wnd != nullptr) {
        // 唤醒阻塞在 GetMessage 里的通知线程；唤醒失败也不丢数据——消息循环启动/
        // 下一轮会再 drain 一次。
        PostMessageW(wnd, kMsgDrain, 0, 0);
    }
}

void NotifyToast::Impl::drainEvents() {
    std::queue<QueuedEvent> local;
    {
        std::lock_guard<std::mutex> lock(mutex);
        local.swap(queue);
    }
    while (!local.empty()) {
        const QueuedEvent& event = local.front();
        if (event.name == kTopicAppQuit) {
            // 宿主即将退出（托盘"退出"）：提前收线程，不等 destroy 路径的 stop()。
            shot::Logger(host).info("[notify] 收到 app.quit，通知线程退出");
            PostQuitMessage(0);
        } else if (event.name == kTopicCaptureDone) {
            onCaptureDone(event.payload);
        } else if (event.name == kTopicCaptureFailed) {
            onCaptureFailed(event.payload);
        }
        // 其它主题安静忽略：事件总线是共享的，收到不认识的主题不是错误。
        local.pop();
    }
}

// ---------------------------------------------------------------------------
// 隐藏占位托盘图标
// ---------------------------------------------------------------------------

bool NotifyToast::Impl::addIcon(HWND wnd) {
    // 图标只是气泡的挂载点，用户看不到（NIS_HIDDEN）；优先用宿主应用图标，
    // 通知内容里的应用身份会更自然，拿不到就退回系统默认图标。
    const HINSTANCE hostModule = GetModuleHandleW(nullptr);  // 宿主 exe 模块，不是本 DLL
    HICON icon = LoadIconW(hostModule, L"IDI_ICON1");
    if (icon == nullptr) {
        icon = LoadIconW(hostModule, MAKEINTRESOURCEW(1));
    }
    if (icon == nullptr) {
        icon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    }
    if (icon == nullptr) {
        shot::Logger(host).error("[notify] 加载通知占位图标失败，消息提示不可用");
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = wnd;
    data.uID = kNotifyIconId;
    // NIF_STATE + NIS_HIDDEN：图标在 Shell 里存在（可以挂气泡），但通知区域不显示。
    // NIF_MESSAGE + uCallbackMessage：Shell 把 NIN_BALLOONUSERCLICK 投到隐藏窗口，
    // 没有它"点击通知打开图片"永远收不到事件（旧版本就没有这个回调）。
    data.uFlags = NIF_ICON | NIF_TIP | NIF_STATE | NIF_MESSAGE;
    data.uCallbackMessage = kMsgNotifyIconEvent;
    data.hIcon = icon;
    const std::wstring tooltip =
        toWide(localized("toast.icon_tooltip", "Plugin-shot 消息提示（隐藏占位）"));
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
    data.dwState = NIS_HIDDEN;
    data.dwStateMask = NIS_HIDDEN;
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        shot::Logger(host).error("[notify] 添加隐藏占位托盘图标失败（Shell_NotifyIconW NIM_ADD）");
        return false;
    }
    iconAdded = true;
    shot::Logger(host).info("[notify] 隐藏占位托盘图标已添加（气泡挂载点，通知区域不显示）");
    return true;
}

void NotifyToast::Impl::removeIcon(HWND wnd) {
    if (!iconAdded || wnd == nullptr) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = wnd;
    data.uID = kNotifyIconId;
    if (!Shell_NotifyIconW(NIM_DELETE, &data)) {
        shot::Logger(host).warn("[notify] 删除隐藏占位托盘图标失败（Shell_NotifyIconW NIM_DELETE）");
    }
    iconAdded = false;
}

}  // namespace notify
