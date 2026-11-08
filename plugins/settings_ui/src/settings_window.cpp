// settings_window —— SettingsWindow 的生命周期与窗口线程：建/停窗口、消息循环、
// 事件队列、WndProc、显示/隐藏。WGL/ImGui 在 settings_graphics.cpp，窗口几何在
// settings_geometry.cpp，ABI 胶水在 plugin_entry.cpp。
//
// 线程模型（对应 document/EVENT_BUS.md）：
//   · 宿主只调 init / deinit / post_to_plugin，从不接触 HWND；
//   · 事件总线回调只做"拷进队列 + PostMessage 唤醒"，所有 ImGui 调用都在这一个
//     UI 线程上执行（ImGui 不是线程安全的）；
//   · X / Esc 只隐藏窗口，线程与 GL 上下文保留，下次 settings.open 直接显示；
//     只有 deinit（或宿主 shutdown）才退出消息循环、销毁窗口，因此宿主热卸载时
//     能 join 成功。

#include "settings_window.h"

#include <string>
#include <utility>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "settings_window_impl.h"
#include "shot_plugin_helpers.h"

// imgui_impl_win32.h 故意把 WndProc 处理函数声明注释掉了（避免头文件拖进 <windows.h>），
// 所以应用侧要自己按官方注释把这一行抄过来。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);

namespace settings_ui {
namespace {

constexpr UINT kMsgDrain = WM_APP + 1;  // 队列里来了新事件，唤醒 UI 线程
constexpr UINT kMsgQuit = WM_APP + 2;   // stop()：让消息循环退出（不直接销毁窗口）

constexpr wchar_t kWindowClass[] = L"PluginShotSettingsUi";
constexpr wchar_t kWindowTitle[] = L"Plugin-shot 设置";

constexpr char kTopicSettingsOpen[] = "settings.open";
constexpr char kTopicConfigApplied[] = "config.applied";
constexpr char kTopicPluginsChanged[] = "plugins.changed";
constexpr char kEventHostShutdown[] = "host.shutdown";

}  // namespace

// ---------------------------------------------------------------------------
// UI 线程主体
// ---------------------------------------------------------------------------
void SettingsWindow::Impl::run() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    // CS_OWNDC：OpenGL 要求窗口的 DC 稳定。没有它时 GetDC/ReleaseDC 之间 DC 可能被
    // 系统换掉，像素格式就丢了。
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = &Impl::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    // IDC_ARROW 在非 UNICODE 编译里展开成窄字符资源 ID；它本质是资源整数而不是
    // 真指针，所以按整数语义转换（与 plugins/setting_window 的做法一致）。
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = nullptr;  // OpenGL 每帧覆盖整个客户区，不需要 GDI 背景
    RegisterClassExW(&windowClass);  // 同进程第二次 init 时已注册过会失败，忽略即可

    // 创建时不带 WS_VISIBLE：窗口先隐藏，等宿主的 settings.open 再显示。
    HWND wnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, kDefaultWindowWidth, kDefaultWindowHeight, nullptr, nullptr,
                               instance, this);
    if (wnd == nullptr) {
        shot::Logger(host).error("settings_ui：创建窗口失败");
        {
            std::lock_guard<std::mutex> lock(readyMutex);
            failed = true;
        }
        readyCv.notify_all();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        hwnd = wnd;
    }

    if (!initOpenGl(wnd) || !initImGui(wnd)) {
        // 初始化失败：先把半成品资源收掉，再销毁窗口；start() 会看到 failed 并 join 本线程。
        shutdownGraphics();
        {
            std::lock_guard<std::mutex> lock(mutex);
            hwnd = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(readyMutex);
            failed = true;
        }
        readyCv.notify_all();
        DestroyWindow(wnd);
        return;
    }

    // 窗口已就绪，先只做握手：模型不在这里读。init 发生时宿主的配置目录/注册表服务
    // 可能还没完全装配好（本函数是在宿主加载插件的过程中被调的），第一次真正读模型
    // 放在 settings.open（窗口显示）或收到事件时，那之前也没有人看得到界面。
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
    }
    readyCv.notify_all();
    shot::Logger(host).info("[settings_ui] 设置窗口线程就绪（窗口隐藏，等 settings.open）");

    messageLoop();

    // 消息循环已退出（stop 或 host.shutdown）。宿主退出时窗口可能还开着，顺手把几何存下来，
    // 下次打开才能恢复到同样的位置（正常关窗时 hideWindow 已经存过一次）。
    if (visible) {
        saveGeometry();
        visible = false;
    }
    // 顺序有讲究：先关 ImGui/GL，再销毁窗口。后端先 shutdown 之后，DestroyWindow 触发的
    // WM_DESTROY 就不会再碰 ImGui。
    shutdownGraphics();
    DestroyWindow(wnd);
}

void SettingsWindow::Impl::messageLoop() {
    // 窗口就绪前入队的事件（例如宿主在 init 返回后立刻发的 settings.open）在这里补处理。
    drainEvents();
    MSG msg{};
    for (;;) {
        if (visible) {
            // 可见时持续重绘：不阻塞地把消息抽干，然后画一帧。
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    return;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!renderFrame()) {
                Sleep(10);  // 最小化 / 客户区为 0：别空转
            } else if (!vsyncEnabled) {
                Sleep(8);  // 没有垂直同步时自己限一下帧，别把 CPU 占满
            }
        } else {
            // 隐藏时不渲染：阻塞等消息，空闲期不占 CPU。投递事件用 PostMessage 唤醒这里。
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
                return;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void SettingsWindow::Impl::showWindow() {
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        wnd = hwnd;
    }
    if (wnd == nullptr || !imguiInitialized) {
        return;
    }
    if (!visible) {
        restoreGeometry();
        ShowWindow(wnd, SW_SHOW);
        visible = true;
        shot::Logger(host).info("settings_ui：设置窗口已显示");
    }
    // 每次打开都重读一遍：窗口隐藏期间配置可能被别的路径改过（config.applied / 热加载）。
    panel.refresh(host);
}

void SettingsWindow::Impl::hideWindow() {
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        wnd = hwnd;
    }
    if (wnd == nullptr) {
        return;
    }
    // 关闭（X / Esc）只隐藏：线程、窗口、GL 上下文都保留，下次 settings.open 直接显示。
    saveGeometry();
    ShowWindow(wnd, SW_HIDE);
    visible = false;
    shot::Logger(host).info("settings_ui：设置窗口已隐藏（几何已存 host config）");
}

void SettingsWindow::Impl::drainEvents() {
    std::queue<QueuedEvent> local;
    {
        std::lock_guard<std::mutex> lock(mutex);
        local.swap(queue);
    }
    while (!local.empty()) {
        const QueuedEvent& event = local.front();
        if (event.name == kEventHostShutdown) {
            // 宿主热卸载/退出：让消息循环退出。stop() 随后 join，窗口在 run() 收尾销毁。
            shot::Logger(host).info("settings_ui：收到 host.shutdown，窗口线程退出");
            PostQuitMessage(0);
        } else if (event.name == kTopicSettingsOpen) {
            showWindow();
        } else if (event.name == kTopicConfigApplied) {
            panel.onConfigApplied();
        } else if (event.name == kTopicPluginsChanged) {
            panel.onPluginsChanged(event.payload);
        }
        // 其它主题安静忽略：事件总线是共享的，收到不认识的主题不是错误。
        local.pop();
    }
}

void SettingsWindow::Impl::enqueue(const char* name, const char* data, std::uint32_t len) {
    if (name == nullptr) {
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
        queue.push(QueuedEvent{name, std::move(payload)});
        wnd = hwnd;
    }
    if (wnd != nullptr) {
        // 唤醒阻塞在 GetMessage 里的 UI 线程；唤醒失败也不丢数据——消息循环启动/下一轮
        // 会再 drain 一次。
        PostMessageW(wnd, kMsgDrain, 0, 0);
    }
}

void SettingsWindow::Impl::onBusEvent(void* user, const char* topic, const void* data,
                                      std::uint32_t len) {
    auto* self = static_cast<Impl*>(user);
    if (self == nullptr) {
        return;
    }
    self->enqueue(topic, static_cast<const char*>(data), len);
}

LRESULT CALLBACK SettingsWindow::Impl::windowProc(HWND wnd, UINT msg, WPARAM wparam,
                                                  LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        // 窗口创建期间把 Impl* 挂到窗口上，之后所有消息都靠它找回来。
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(wnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
        // 必须继续交给 DefWindowProc：窗口标题（CREATESTRUCT 里的 lpszName）就是在这里
        // 被设置的。直接 return TRUE 会让标题栏一直空白（实测两个窗口都踩过这个坑）。
        return DefWindowProcW(wnd, msg, wparam, lparam);
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(wnd, msg, wparam, lparam);
    }
    switch (msg) {
        case kMsgDrain:
            self->drainEvents();
            return 0;
        case kMsgQuit:
            // stop() 请求退出：只结束消息循环，窗口销毁统一放在 run() 收尾做。
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            self->hideWindow();  // X 只隐藏，不销毁线程（SETTINGS.md §6）
            return 0;
        case WM_ERASEBKGND:
            return 1;  // OpenGL 每帧覆盖客户区，让 GDI 擦背景只会闪一下
        case WM_GETMINMAXINFO: {
            auto* minMax = reinterpret_cast<MINMAXINFO*>(lparam);
            minMax->ptMinTrackSize.x = kMinimumWindowWidth;
            minMax->ptMinTrackSize.y = kMinimumWindowHeight;
            return 0;
        }
        case WM_KEYDOWN:
            // Esc 只隐藏窗口。文本框正在输入时把 Esc 让给 ImGui（取消输入而不是关窗）。
            if (wparam == VK_ESCAPE && ImGui::GetCurrentContext() != nullptr &&
                !ImGui::GetIO().WantTextInput) {
                self->hideWindow();
                return 0;
            }
            break;
        case WM_DESTROY: {
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->hwnd = nullptr;
            }
            PostQuitMessage(0);  // 兜底：万一窗口被外部销毁，消息循环也能退出
            return 0;
        }
        default:
            break;
    }
    // 其余消息先给 ImGui 后端（鼠标 / 键盘 / IME / 焦点 / 光标），它没处理再交给系统。
    if (ImGui_ImplWin32_WndProcHandler(wnd, msg, wparam, lparam) != 0) {
        return 0;
    }
    return DefWindowProcW(wnd, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
// 公共接口
// ---------------------------------------------------------------------------

SettingsWindow::SettingsWindow() : m_impl(std::make_unique<Impl>()) {}

SettingsWindow::~SettingsWindow() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool SettingsWindow::start(const ShotHostServices* host) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->thread.joinable()) {
        return true;  // 已经在跑：重复 init 是空操作
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = true;
        std::queue<QueuedEvent>().swap(m_impl->queue);
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->readyMutex);
        m_impl->ready = false;
        m_impl->failed = false;
    }
    m_impl->host = host;

    // 先订阅再起线程：订阅之后到达的 settings.open 不会漏；窗口还没就绪时事件先入队，
    // 消息循环启动后统一 drain。
    shot::EventBus bus(host);
    if (bus.available()) {
        bool subscribedAny = false;
        subscribedAny =
            bus.subscribe(kTopicSettingsOpen, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicConfigApplied, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicPluginsChanged, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        m_impl->busSubscribed = subscribedAny;
        if (!subscribedAny) {
            shot::Logger(host).warn("settings_ui：订阅设置页事件失败，窗口只能靠 post_to_plugin 打开");
        }
    } else {
        // 旧版宿主没有事件总线：安静降级，设置窗口不会自动弹出（只能靠 post_to_plugin 投递 settings.open）。
        shot::Logger(host).warn("settings_ui：宿主未提供事件总线（旧版宿主？），设置窗口不会自动打开");
    }

    try {
        m_impl->thread = std::thread(&Impl::run, m_impl.get());
    } catch (...) {
        // 线程资源耗尽等：异常绝不跨越 DLL 边界。
        if (m_impl->busSubscribed) {
            shot::EventBus unsubscribeBus(host);
            unsubscribeBus.unsubscribe(m_impl.get());
            m_impl->busSubscribed = false;
        }
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(m_impl->readyMutex);
        m_impl->readyCv.wait(lock, [this]() { return m_impl->ready || m_impl->failed; });
        if (!m_impl->failed) {
            return true;
        }
    }
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();  // 已经 failed 返回了，这里不会阻塞
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
    }
    return false;
}

void SettingsWindow::stop() {
    if (!m_impl) {
        return;
    }
    // 1. 先退订事件总线：deinit 返回后宿主不再回调本插件（EVENT_BUS.md §5）。
    if (m_impl->busSubscribed) {
        shot::EventBus bus(m_impl->host);
        bus.unsubscribe(m_impl.get());
        m_impl->busSubscribed = false;
    }
    // 2. 拒绝后续投递（post_to_plugin / 在途的总线回调都会在这里止步）。
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
    }
    // 3. 让 UI 线程退出消息循环，然后 join：返回前窗口一定已经 DestroyWindow。
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
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
        std::queue<QueuedEvent>().swap(m_impl->queue);
    }
}

std::int32_t SettingsWindow::post(const char* name, const char* data, std::uint32_t len) {
    if (!m_impl || name == nullptr) {
        return -1;
    }
    try {
        std::string payload;
        if (data != nullptr && len > 0) {
            payload.assign(data, len);  // 契约要求：返回前把 data_len 字节拷走
        }
        HWND wnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (!m_impl->acceptingEvents) {
                return 1;  // 窗口不在（还没起好 / 正在关闭）：丢弃
            }
            m_impl->queue.push(QueuedEvent{name, std::move(payload)});
            wnd = m_impl->hwnd;
        }
        if (wnd != nullptr && PostMessageW(wnd, kMsgDrain, 0, 0) == 0) {
            return 2;
        }
        return 0;
    } catch (...) {
        return 2;  // 分配失败等：异常绝不跨越 DLL 边界
    }
}

}  // namespace settings_ui
