// 托盘窗口线程：建隐藏消息窗口、添加托盘图标、消息循环、事件回调。
//
// 为什么需要一个窗口：Shell_NotifyIconW 要求一个 hWnd 接收托盘回调消息；
// 这个窗口永远不显示（没有 WS_VISIBLE），它只是托盘线程的消息泵：
//   · 托盘鼠标回调（kMsgTrayCallback）→ 左键执行默认命令 / 右键弹菜单；
//   · kMsgDrain            → 队列里有宿主事件，醒来处理；
//   · kMsgQuit             → stop() 请求退出消息循环；
//   · "TaskbarCreated"     → explorer.exe 重启后重新添加图标。
//
// 右键菜单的内容由 tray_command_list 数据驱动：渲染在 tray_icon_menu.cpp、
// 数据模型在 tray_menu.cpp；本文件只做窗口/线程/托盘图标。事件出队与气泡通知在
// tray_icon_notify.cpp；生命周期/线程握手在 tray_icon.cpp；ABI 胶水在
// plugin_entry.cpp（STYLE.md §6.8 按职责拆文件）。

#include "tray_icon.h"

#include <cwchar>
#include <string>

#include "shot_plugin_helpers.h"
#include "tray_icon_impl.h"

namespace tray {
namespace {

constexpr wchar_t kWindowClass[] = L"PluginShotTrayWnd";  /**< 进程内唯一；类不随 DLL 卸载消失，必须显式注销。 */
constexpr wchar_t kWindowTitle[] = L"Plugin-shot 托盘";     /**< 隐藏窗口的标题，只用于排障。 */
constexpr wchar_t kTooltip[] = L"Plugin-shot 托盘";         /**< 鼠标悬停提示，最长 127 字符。 */

}  // namespace

bool TrayIcon::Impl::registerWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Impl::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    // IDC_ARROW 在非 UNICODE 编译里展开成窄字符资源 ID；它本质是资源整数而不是
    // 真指针，所以按整数语义转换（与 plugins/settings_ui 的做法一致）。
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = nullptr;  // 隐藏窗口从不绘制，不需要背景刷
    return RegisterClassExW(&windowClass) != 0;
}

bool TrayIcon::Impl::ensureWindowClass(HINSTANCE instance) {
    if (registerWindowClass(instance)) {
        return true;
    }
    const DWORD registerError = GetLastError();
    if (registerError != ERROR_CLASS_ALREADY_EXISTS) {
        shot::Logger(host).error("[tray] 注册托盘窗口类失败（错误码 " +
                                 std::to_string(registerError) + "）");
        return false;
    }
    // 进程里已经有一个同名窗口类：它可能是上一次运行（甚至已经 FreeLibrary 的旧 DLL）
    // 留下的，lpfnWndProc 可能已经指向被卸载的代码。绝不能直接沿用——先注销，再注册
    // 自己这一份；注销失败说明还有窗口在用它，只能失败返回，让宿主按 init 失败处理。
    // 这是"设置页禁用托盘→再启用托盘崩溃 0xC000041D"的根因所在。
    if (!UnregisterClassW(kWindowClass, instance)) {
        const DWORD unregisterError = GetLastError();
        shot::Logger(host).error("[tray] 清理遗留托盘窗口类失败（错误码 " +
                                 std::to_string(unregisterError) + "），拒绝沿用未知窗口过程");
        return false;
    }
    if (!registerWindowClass(instance)) {
        shot::Logger(host).error("[tray] 清理遗留窗口类后重新注册失败（错误码 " +
                                 std::to_string(GetLastError()) + "）");
        return false;
    }
    shot::Logger(host).warn("[tray] 已清掉进程里遗留的同名窗口类并重新注册（禁用→启用）");
    return true;
}

void TrayIcon::Impl::unregisterWindowClass(HINSTANCE instance) {
    // 窗口类属于进程、不随 DLL 卸载消失（也不随窗口销毁消失）：不注销的话，DLL 被
    // FreeLibrary 之后类里的 lpfnWndProc 就是悬空指针，下次（禁用→启用）重新加载再
    // CreateWindowExW 就会跳到已卸载的代码（实测 0xC000041D）。所以线程退出前必须
    // 显式注销；注销失败只 warn，不影响线程退出（类残留可由 ensureWindowClass 兜底清理）。
    if (UnregisterClassW(kWindowClass, instance)) {
        return;
    }
    shot::Logger(host).warn("[tray] 注销托盘窗口类失败（错误码 " +
                            std::to_string(GetLastError()) + "）");
}

void TrayIcon::Impl::run() {
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
            shot::Logger(host).error("[tray] 创建托盘消息窗口失败");
            ok = false;
        }
    }

    if (ok) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            hwnd = wnd;
        }
        // explorer.exe 重启后图标会被 Shell 丢掉；Shell 重启完成时会广播注册消息
        // "TaskbarCreated"，收到后重新 NIM_ADD，否则用户得重启宿主才能再看到图标。
        taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
        trayCallbackMessage = kMsgTrayCallback;
        ok = addIcon(wnd);
        if (ok) {
            // 启动时先构建一次右键菜单：init 返回前菜单已就绪；之后宿主发布
            // lists.changed 时由托盘线程按最新列表重建。
            rebuildMenu();
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

    // 图标已添加，先只做握手：宿主可能在 init 返回后立刻发 capture.done，
    // 那些事件先入队，消息循环启动后统一 drain。
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
    }
    readyCv.notify_all();
    shot::Logger(host).info("[tray] 托盘线程就绪：左键执行默认命令，右键菜单由 tray_command_list 驱动");

    messageLoop();

    // 消息循环退出（stop() 或宿主退出）：顺序有讲究——先 NIM_DELETE 再销毁窗口，
    // 因为 Shell 删除图标时还要用 hWnd 找到它；菜单句柄与窗口类也必须显式释放，
    // 保证本 DLL 被 FreeLibrary 后进程里不残留指向本模块代码的 WndProc。
    if (menu != nullptr) {
        DestroyMenu(menu);
        menu = nullptr;
    }
    removeIcon();
    DestroyWindow(wnd);
    {
        std::lock_guard<std::mutex> lock(mutex);
        hwnd = nullptr;
    }
    unregisterWindowClass(instance);
    shot::Logger(host).info("[tray] 托盘线程已退出，图标与窗口类都已删除");
}

void TrayIcon::Impl::messageLoop() {
    // 线程就绪前入队的事件（例如宿主 init 后立刻发布的 capture.done）在这里补处理。
    drainEvents();
    MSG msg{};
    for (;;) {
        // 托盘不渲染任何东西：阻塞等消息，空闲期不占 CPU。
        // stop()/post() 都用 PostMessageW 唤醒这里，不要改成 PeekMessage 空转。
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool TrayIcon::Impl::addIcon(HWND wnd) {
    // TODO: 这里用系统共享的应用图标兜底（LoadIconW(nullptr, IDI_APPLICATION)），
    // 因为本插件还没有自己的图标资源；等 package 带上 .rc 图标后换成
    // LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(...))。
    HICON loaded = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    if (loaded == nullptr) {
        shot::Logger(host).error("[tray] 加载托盘图标失败");
        return false;
    }
    icon = loaded;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = wnd;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = trayCallbackMessage;
    data.hIcon = icon;
    // szTip 是定长缓冲；_TRUNCATE 保证超长时安全截断，不会写坏结构体。
    wcsncpy_s(data.szTip, kTooltip, _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        shot::Logger(host).error("[tray] Shell_NotifyIconW(NIM_ADD) 失败");
        icon = nullptr;
        return false;
    }
    iconAdded = true;
    shot::Logger(host).info("[tray] 托盘图标已添加（Shell_NotifyIconW NIM_ADD 成功）");

    // 再向 Shell 查一次图标矩形：确认图标真的挂上了，也方便排障时知道它在哪。
    // 系统把图标放进通知区域溢出区时可能查不到，这不是错误，所以只当调试信息。
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = wnd;
    identifier.uID = kTrayIconId;
    RECT rect{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &rect))) {
        shot::Logger(host).debug("[tray] 托盘图标在通知区域的位置 " + std::to_string(rect.left) +
                                 "," + std::to_string(rect.top) + " " +
                                 std::to_string(rect.right - rect.left) + "x" +
                                 std::to_string(rect.bottom - rect.top));
    }
    return true;
}

void TrayIcon::Impl::removeIcon() {
    if (!iconAdded) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kTrayIconId;
    if (!Shell_NotifyIconW(NIM_DELETE, &data)) {
        // 删不掉只能用日志留痕：窗口马上要销毁，Shell 最终也会清理这个图标。
        shot::Logger(host).warn("[tray] Shell_NotifyIconW(NIM_DELETE) 失败");
    }
    iconAdded = false;
    // LoadIconW 返回的是系统共享图标，不能 DestroyIcon 释放（会破坏其它进程/窗口的图标）。
    icon = nullptr;
}

LRESULT CALLBACK TrayIcon::Impl::windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
        shot::Logger(self->host).warn("[tray] 窗口消息处理异常，已忽略该消息");
        return 0;  // 已消费的消息按"处理过"返回，避免 DefWindowProc 再碰这份半成品状态
    }
}

LRESULT TrayIcon::Impl::handleMessage(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // explorer.exe 重启时 Shell 会广播注册消息：旧图标已随 Shell 消失，重新加。
    // addIcon 可能失败（Shell 忙），记一条 warn 不影响后续消息循环。
    if (taskbarCreatedMessage != 0 && msg == taskbarCreatedMessage) {
        iconAdded = false;
        if (!addIcon(wnd)) {
            shot::Logger(host).warn("[tray] explorer 重启后重新添加托盘图标失败");
        }
        return 0;
    }

    switch (msg) {
        case kMsgDrain:
            drainEvents();
            return 0;
        case kMsgQuit:
            // stop() 请求退出：只结束消息循环，图标删除与窗口销毁统一在线程收尾做。
            PostQuitMessage(0);
            return 0;
        case kMsgTrayCallback:
            // 托盘回调把原始鼠标消息塞在 lParam 里（wParam 是图标 id + 鼠标消息的高位）。
            if (static_cast<UINT>(lparam) == WM_LBUTTONUP) {
                // 左键 = 列表里标了 default 的命令；没有则保持历史行为 capture.request。
                publishDefaultCommand();
            } else if (static_cast<UINT>(lparam) == WM_RBUTTONUP) {
                showContextMenu();  // 右键单击 = 弹出数据驱动菜单
            }
            // 双击 / 鼠标移动 / 键盘激活等其它托盘消息：安静忽略。
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

void TrayIcon::Impl::onBusEvent(void* user, const char* topic, const void* data,
                                std::uint32_t len) {
    auto* self = static_cast<Impl*>(user);
    if (self == nullptr || topic == nullptr) {
        return;
    }
    // 回调运行在宿主分发线程上：只做深拷贝 + PostMessage 唤醒托盘线程，
    // 不阻塞、不碰任何 Win32 托盘对象（EVENT_BUS.md §3 的回调规则）。
    // 回调是跨 DLL 的 C 边界，异常绝不能穿过去（STYLE.md §6.2）：分配失败就丢掉这条事件。
    try {
        self->enqueue(topic, static_cast<const char*>(data), len);
    } catch (...) {
        shot::Logger(self->host).warn("[tray] 接收事件失败（内存不足？），已丢弃该事件");
    }
}

}  // namespace tray
