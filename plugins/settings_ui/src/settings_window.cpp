// settings_window —— SettingsWindow 的生命周期与窗口线程：建/停窗口、GLFW 事件循环、
// 事件队列、显示/隐藏。窗口与 ImGui 的渲染在 settings_graphics.cpp，窗口几何在
// settings_geometry.cpp，ABI 胶水在 plugin_entry.cpp，文件对话框在 settings_dialog.cpp。
//
// 线程模型（对应 document/EVENT_BUS.md）：
//   · 宿主只调 init / deinit / post_to_plugin，从不接触窗口句柄；
//   · 事件总线回调只做"拷进队列 + glfwPostEmptyEvent 唤醒"，所有 GLFW / ImGui 调用
//     都在这一个 UI 线程上执行（GLFW 和 ImGui 都不是线程安全的）；
//   · X / Esc 只隐藏窗口，线程与 GL 上下文保留，下次 settings.open 直接显示；
//     只有 deinit（或宿主 shutdown）才退出事件循环、销毁窗口，因此宿主热卸载时
//     能 join 成功。
//
// TODO: POSIX（尤其 macOS）未验证。macOS 上 GLFW 要求窗口与事件循环在进程主线程，
// 而本插件的 UI 线程是自建线程；这里按"GLFW 可在任意线程 init/建窗"的 Windows/Linux
// 行为实现，等有 macOS 环境再决定是否把 UI 线程改为进程主线程驱动。

#include "settings_window.h"

#include <string>
#include <utility>

#include <GLFW/glfw3.h>  // 跨平台窗口库（不是平台头）：只用于窗口生命周期与事件循环

#include "imgui.h"
#include "settings_dialog.h"
#include "settings_window_impl.h"
#include "shot_plugin_helpers.h"

namespace settings_ui {
namespace {

constexpr char kWindowTitle[] = "Plugin-shot 设置";

constexpr char kTopicSettingsOpen[] = "settings.open";
constexpr char kTopicConfigApplied[] = "config.applied";
constexpr char kTopicPluginsChanged[] = "plugins.changed";
constexpr char kTopicListsChanged[] = "lists.changed";
constexpr char kTopicListsUpdated[] = "lists.updated";
constexpr char kEventHostShutdown[] = "host.shutdown";

// GLFW 的错误回调没有 user 参数，只能用一个文件内全局记录"当前窗口实例的宿主服务"。
// 单窗口单实例所以够用；由 UI 线程在 glfwInit 之前设置、glfwTerminate 之后清空。
// 这两个函数与任何类无关、只在本 TU 用，所以放匿名命名空间（STYLE.md §6.7）。
std::mutex g_glfwErrorMutex;
const ShotHostServices* g_glfwErrorHost = nullptr;

/**
 * @brief 记录 GLFW 错误回调要用宿主服务表（只在 glfwInit 前/glfwTerminate 后调）。
 * @param[in] host 宿主服务表，可为 nullptr（= 不记日志）。
 */
void setGlfwErrorHost(const ShotHostServices* host) {
    std::lock_guard<std::mutex> lock(g_glfwErrorMutex);
    g_glfwErrorHost = host;
}

/** @brief 清掉 GLFW 错误回调的日志目标（glfwTerminate 之后调）。 */
void clearGlfwErrorHost() {
    std::lock_guard<std::mutex> lock(g_glfwErrorMutex);
    g_glfwErrorHost = nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// UI 线程主体
// ---------------------------------------------------------------------------
void SettingsWindow::Impl::run() {
    setGlfwErrorHost(host);

    // 三步初始化：GLFW 运行时 -> 隐藏窗口 + GL 上下文 -> ImGui 后端。
    // 任一步失败都走"收掉半成品 -> 报告失败"的路径，不留下半个窗口线程。
    if (!initGlfw() || !createWindow() || !initImGui()) {
        shutdownGraphics();
        shutdownGlfw();
        {
            std::lock_guard<std::mutex> lock(readyMutex);
            failed = true;
        }
        readyCv.notify_all();
        return;
    }

    // 窗口已就绪：先只做握手，模型不在这里读。init 发生时宿主的配置目录/注册表服务
    // 可能还没完全装配好（本函数是在宿主加载插件的过程中被调的），第一次真正读模型
    // 放在 settings.open（窗口显示）或收到事件时，那之前也没有人看得到界面。
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
    }
    readyCv.notify_all();
    shot::Logger(host).info("[settings_ui] 设置窗口线程就绪（窗口隐藏，等 settings.open）");

    messageLoop();

    // 事件循环已退出（stop 或 host.shutdown）。宿主退出时窗口可能还开着，顺手把几何存下来，
    // 下次打开才能恢复到同样的位置（正常关窗时 hideWindow 已经存过一次）。
    if (visible) {
        saveGeometry();
        visible = false;
    }
    // 顺序有讲究：先关 ImGui，再销毁窗口/终止 GLFW。
    shutdownGraphics();
    shutdownGlfw();
}

bool SettingsWindow::Impl::initGlfw() {
    glfwSetErrorCallback(&Impl::onGlfwError);
    if (glfwInit() != GLFW_TRUE) {
        shot::Logger(host).error("[settings_ui] GLFW 初始化失败");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        glfwInitialized = true;
    }
    return true;
}

bool SettingsWindow::Impl::createWindow() {
    // 窗口先隐藏：等宿主的 settings.open 再显示（契约允许宿主在 init 之后随时投递事件）。
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    // macOS 只提供 3.2+ core profile，且必须是 forward-compatible 的上下文。
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    window = glfwCreateWindow(kDefaultWindowWidth, kDefaultWindowHeight, kWindowTitle, nullptr,
                              nullptr);
    if (window == nullptr) {
        shot::Logger(host).error("[settings_ui] 创建 GLFW 窗口/OpenGL 上下文失败");
        return false;
    }
    glfwSetWindowSizeLimits(window, kMinimumWindowWidth, kMinimumWindowHeight, GLFW_DONT_CARE,
                            GLFW_DONT_CARE);
    // 键盘/关闭回调：GLFW 的窗口用户指针用来在回调里找回 Impl（static 回调没有 this）。
    glfwSetWindowUserPointer(window, this);
    glfwSetKeyCallback(window, &Impl::onKey);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // 有垂直同步就开；没有也只是多画几帧，不影响功能
    return true;
}

void SettingsWindow::Impl::shutdownGlfw() {
    // 对话框库是懒初始化的：用过才需要收尾（没用过时是空操作）。
    shutdownFileDialogs();
    if (window != nullptr) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        glfwInitialized = false;
    }
    glfwTerminate();  // 未初始化时调用是安全的空操作（GLFW 文档保证）
    clearGlfwErrorHost();
}

void SettingsWindow::Impl::messageLoop() {
    // 窗口就绪前入队的事件（例如宿主在 init 返回后立刻发的 settings.open）在这里补处理。
    drainEvents();
    while (!shouldQuit()) {
        if (visible) {
            // 可见时按约 60Hz 重绘：有事件立即返回（交互跟手），没事件最多等 16ms。
            glfwWaitEventsTimeout(0.016);
        } else {
            // 隐藏时不渲染：阻塞等事件，空闲期不占 CPU。宿主投递用 glfwPostEmptyEvent 唤醒。
            glfwWaitEvents();
        }
        drainEvents();
        if (shouldQuit()) {
            break;
        }
        if (window != nullptr && glfwWindowShouldClose(window) != GLFW_FALSE) {
            // 点 X 只是隐藏：把 GLFW 的"要关闭"标记吃掉，窗口与线程都保留。
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            hideWindow();
        }
        if (escHideRequested) {
            escHideRequested = false;
            hideWindow();
        }
        if (visible) {
            renderFrame();
        }
    }
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
            // 宿主热卸载/退出：让事件循环退出。stop() 随后 join，窗口在 run() 收尾销毁。
            shot::Logger(host).info("settings_ui：收到 host.shutdown，窗口线程退出");
            requestQuit();
        } else if (event.name == kTopicSettingsOpen) {
            showWindow();
        } else if (event.name == kTopicConfigApplied) {
            panel.onConfigApplied();
        } else if (event.name == kTopicPluginsChanged) {
            panel.onPluginsChanged(event.payload);
        } else if (event.name == kTopicListsChanged) {
            panel.onListsChanged(event.payload);
        } else if (event.name == kTopicListsUpdated) {
            panel.onListsUpdated(event.payload);
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
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!acceptingEvents) {
            return;
        }
        queue.push(QueuedEvent{name, std::move(payload)});
        wake = glfwInitialized;
    }
    if (wake) {
        // 唤醒阻塞在 glfwWaitEvents 里的 UI 线程；唤醒失败也不丢数据——事件循环下一轮
        // 会再 drain 一次。
        glfwPostEmptyEvent();
    }
}

bool SettingsWindow::Impl::shouldQuit() {
    std::lock_guard<std::mutex> lock(mutex);
    return quitRequested;
}

void SettingsWindow::Impl::requestQuit() {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        quitRequested = true;
        wake = glfwInitialized;
    }
    if (wake) {
        glfwPostEmptyEvent();
    }
}

void SettingsWindow::Impl::showWindow() {
    if (window == nullptr || !imguiInitialized) {
        return;
    }
    if (!visible) {
        restoreGeometry();
        glfwShowWindow(window);
        glfwFocusWindow(window);
        visible = true;
        shot::Logger(host).info("settings_ui：设置窗口已显示");
    }
    // 每次打开都重读一遍：窗口隐藏期间配置可能被别的路径改过（config.applied / 热加载）。
    panel.refresh(host);
}

void SettingsWindow::Impl::hideWindow() {
    if (window == nullptr) {
        return;
    }
    // 关闭（X / Esc）只隐藏：线程、窗口、GL 上下文都保留，下次 settings.open 直接显示。
    saveGeometry();
    glfwHideWindow(window);
    visible = false;
    shot::Logger(host).info("settings_ui：设置窗口已隐藏（几何已存 host config）");
}

void SettingsWindow::Impl::onBusEvent(void* user, const char* topic, const void* data,
                                      std::uint32_t len) {
    auto* self = static_cast<Impl*>(user);
    if (self == nullptr) {
        return;
    }
    self->enqueue(topic, static_cast<const char*>(data), len);
}

void SettingsWindow::Impl::onGlfwError(int errorCode, const char* description) {
    const char* text = (description != nullptr) ? description : "(no description)";
    const ShotHostServices* logHost = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_glfwErrorMutex);
        logHost = g_glfwErrorHost;
    }
    shot::Logger(logHost).error("[settings_ui] GLFW 错误 " + std::to_string(errorCode) + "：" + text);
}

void SettingsWindow::Impl::onKey(GLFWwindow* glfwWindow, int key, int /*scancode*/, int action,
                                 int /*mods*/) {
    auto* self = static_cast<Impl*>(glfwGetWindowUserPointer(glfwWindow));
    if (self == nullptr) {
        return;
    }
    // Esc 只隐藏窗口；文本框正在输入时把 Esc 让给 ImGui（取消输入而不是关窗）。
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS &&
        ImGui::GetCurrentContext() != nullptr && !ImGui::GetIO().WantTextInput) {
        self->escHideRequested = true;  // 回调里只置位，隐藏动作放到事件循环里做
    }
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
        m_impl->glfwInitialized = false;
        m_impl->quitRequested = false;
        std::queue<QueuedEvent>().swap(m_impl->queue);
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->readyMutex);
        m_impl->ready = false;
        m_impl->failed = false;
    }
    m_impl->host = host;

    // 先订阅再起线程：订阅之后到达的 settings.open 不会漏；窗口还没就绪时事件先入队，
    // 事件循环启动后统一 drain。
    shot::EventBus bus(host);
    if (bus.available()) {
        bool subscribedAny = false;
        subscribedAny =
            bus.subscribe(kTopicSettingsOpen, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicConfigApplied, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicPluginsChanged, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicListsChanged, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicListsUpdated, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        m_impl->busSubscribed = subscribedAny;
        if (!subscribedAny) {
            shot::Logger(host).warn("settings_ui：订阅设置页事件失败，窗口只能靠 post_to_plugin 打开");
        }
    } else {
        // 旧版宿主没有事件总线：安静降级，设置窗口不会自动弹出。
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
    // 2. 拒绝后续投递，并让事件循环退出（隐藏时它阻塞在 glfwWaitEvents 上）。
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
    }
    m_impl->requestQuit();
    // 3. join：返回前窗口一定已经销毁（销毁在 run() 收尾 / 失败路径里做）。
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->quitRequested = false;
        m_impl->glfwInitialized = false;
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
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (!m_impl->acceptingEvents) {
                return 1;  // 窗口不在（还没起好 / 正在关闭）：丢弃
            }
            m_impl->queue.push(QueuedEvent{name, std::move(payload)});
            wake = m_impl->glfwInitialized;
        }
        if (wake) {
            // glfwPostEmptyEvent 返回 void（"唤醒"本身没有失败返回值可用），事件已在队列里，
            // 即使这次唤醒丢了，事件循环下一轮也会 drain 到。
            glfwPostEmptyEvent();
        }
        return 0;
    } catch (...) {
        return 2;  // 分配失败等：异常绝不跨越 DLL 边界
    }
}

}  // namespace settings_ui
