// region_window —— RegionWindow 的生命周期、窗口线程、GLFW 事件循环与事件队列；
// 触发负载解析与"隐藏 → 等待 → 发布"的提交流程也在这里。
//
// 线程模型（对应 document/EVENT_BUS.md）：
//   · 宿主只调 init / deinit / post_to_plugin，从不接触窗口句柄；
//   · 事件总线回调只做"拷进队列 + 唤醒 UI 线程"，所有 GLFW / ImGui 调用都在这一个
//     UI 线程上执行（GLFW 和 ImGui 都不是线程安全的）；
//   · 空闲（覆盖层隐藏）时线程阻塞在条件变量上，不占 CPU；框选时按
//     kSelectionPollSeconds 超时轮询鼠标，拖动跟手；
//   · stop() / host.shutdown 让事件循环退出，run() 收尾销毁窗口后线程才 join 成功。
//
// 为什么 GLFW / 窗口要延迟到第一次框选才初始化（实测踩过的坑，不要"优化"回启动时建）：
//   GLFW 在 Windows 上只有一个全局 helper window，glfwPostEmptyEvent() 实际是
//   PostMessage(helperWindow, WM_NULL)：消息只投给**创建 helper window 的线程**，
//   而 helper window 属于进程里第一个调用 glfwInit 的线程。settings_ui 的 stop() 靠
//   "glfwPostEmptyEvent 唤醒自己的窗口线程"退出，如果本插件（目录名字母序在
//   settings_ui 之前）在启动阶段先 glfwInit，helper window 就落在本插件 UI 线程上，
//   settings_ui 退出时永远等不到唤醒 → 宿主卡在它的 deinit，30 秒都退不掉。
//   所以启动只起线程；第一次框选才 ensureGraphics()，此时 settings_ui 已经初始化过
//   GLFW（helper window 归它的线程）。同理，隐藏态等待用条件变量，绝不依赖
//   glfwPostEmptyEvent（它唤醒的是 helper window 的线程，不是我们自己的）。
//
// 触发与自触发：本插件确认后发布的 `{"mode":"region","region":{...}}` 也会回到本插件
// 的订阅里，如果把它也当成触发就会"框选一次 → 又弹一次"无限循环。所以只有
// **mode=region 但没有可用 region 对象** 的负载才是"请开始框选"。
//
// 提交时序（需求硬约束）：先 glfwHideWindow + glfwPollEvents，再 sleep
// kHideSettleDelayMs（100ms），最后才 publish capture.request。DWM 合成是异步的，
// 隐藏函数返回不代表遮罩已经消失；宿主 worker 收到事件就抓屏，中间没有同步点。
//
// TODO: 跨显示器选择：现在只覆盖"鼠标所在显示器"，拖到另一块屏的选区会被夹在
// 当前屏内。要做跨屏需要在虚拟桌面矩形上开一个覆盖所有显示器的窗口，窗口尺寸/定位
// 仍用物理像素；等宿主把混合 DPI 下的跨屏截图验证过再一起做。

#include "region_window.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <GLFW/glfw3.h>  // 跨平台窗口库：窗口生命周期与事件循环

#include <nlohmann/json.hpp>  // 触发负载解析 + 区域结果组包（header-only）

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

constexpr char kWindowTitle[] = "Plugin-shot 区域选择";

// 初始窗口尺寸：只在 start() 里建隐藏窗口时用；真正显示覆盖层前会被搬到目标
// 显示器并铺满整块屏（glfwSetWindowMonitor）。
constexpr int kInitialWindowWidth = 320;
constexpr int kInitialWindowHeight = 180;

// "隐藏"覆盖层用的屏幕外坐标：窗口不 hide/show（那会触发 DWM 过渡动画而闪屏），
// 只把它挪到所有显示器之外；框选时再挪回来。Windows 允许窗口位于屏幕外。
constexpr int kParkedX = -32000;
constexpr int kParkedY = -32000;

// GLFW 的错误回调没有 user 参数，只能用一个文件内全局记录"当前实例的宿主服务"。
// 单窗口单实例所以够用；由 UI 线程在 glfwInit 之前设置、在窗口销毁之后清空。
// 这两个函数与任何类无关、只在本 TU 用，所以放匿名命名空间（STYLE.md §6.7）。
std::mutex g_glfwErrorMutex;
const ShotHostServices* g_glfwErrorHost = nullptr;

/**
 * @brief 记录 GLFW 错误回调要用宿主服务表（只在 glfwInit 前调）。
 * @param[in] host 宿主服务表，可为 nullptr（= 不记日志）。
 */
void setGlfwErrorHost(const ShotHostServices* host) {
    std::lock_guard<std::mutex> lock(g_glfwErrorMutex);
    g_glfwErrorHost = host;
}

/** @brief 清掉 GLFW 错误回调的日志目标（窗口销毁之后调）。 */
void clearGlfwErrorHost() {
    std::lock_guard<std::mutex> lock(g_glfwErrorMutex);
    g_glfwErrorHost = nullptr;
}

/** @brief capture.request 负载的分类结果。 */
enum class RequestKind {
    Ignore,         /**< 不是区域框选请求（空负载 / 其它 mode / 已经带可用 region）。 */
    BeginSelection, /**< `{"mode":"region"}`：请在鼠标所在显示器上开始框选。 */
};

/**
 * @brief 判断一条 capture.request 负载是不是"请开始框选"。
 *
 * 约定（document/CAPTURE.md）：空负载 = 整屏采集；`{"mode":"region"}` = 请求开始框选；
 * `{"mode":"region","region":{x,y,width,height}}` = 已经选好区域、请求按区域采集。
 *
 * @param[in] payload 事件负载原文（可能为空串）。
 * @return BeginSelection 仅当负载是 JSON 对象、mode == "region" 且**没有**可用的
 *         region 对象（x/y/width/height 四个整数、宽高都 > 0）。
 * @note 不抛异常：非法 JSON / 字段类型不对都按 Ignore 安静处理（事件总线是共享的，
 *       收到不认识或坏掉的负载不是错误）。
 */
RequestKind classifyCaptureRequest(const std::string& payload) {
    if (payload.empty()) {
        return RequestKind::Ignore;
    }
    const nlohmann::json data = nlohmann::json::parse(payload, nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
        return RequestKind::Ignore;
    }
    const auto modeIt = data.find("mode");
    if (modeIt == data.end() || !modeIt->is_string()) {
        return RequestKind::Ignore;
    }
    if (modeIt->get<std::string>() != "region") {
        return RequestKind::Ignore;
    }
    // 带合法 region 的请求是"我们自己刚发布的那条"，以及别处已经选好区域的请求，
    // 都不能再触发一次框选（否则会无限循环）。
    const auto regionIt = data.find("region");
    if (regionIt != data.end() && regionIt->is_object()) {
        const auto readInt = [&regionIt](const char* key, std::int64_t& out) {
            const auto field = regionIt->find(key);
            if (field == regionIt->end() || !field->is_number_integer()) {
                return false;
            }
            out = field->get<std::int64_t>();
            return true;
        };
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t width = 0;
        std::int64_t height = 0;
        if (readInt("x", x) && readInt("y", y) && readInt("width", width) &&
            readInt("height", height) && width > 0 && height > 0) {
            return RequestKind::Ignore;
        }
    }
    return RequestKind::BeginSelection;
}

}  // namespace

// ---------------------------------------------------------------------------
// UI 线程主体
// ---------------------------------------------------------------------------
void RegionWindow::Impl::run() {
    setGlfwErrorHost(host);

    // 线程本身不依赖 GLFW：GLFW/窗口/ImGui 延迟到第一次框选（原因见文件头）。
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
    }
    readyCv.notify_all();
    shot::Logger(host).info(
        "[region_selector] 覆盖层线程就绪（GLFW/窗口在第一次框选时按需创建，等 capture.request）");

    messageLoop();

    // 事件循环退出（stop 或 host.shutdown）：正在框选就按取消处理，不发布任何事件。
    if (overlayVisible) {
        hideOverlay();
    }
    // 顺序有讲究：先关 ImGui，再销毁窗口。
    shutdownGraphics();
    shutdownWindow();
    clearGlfwErrorHost();
}

bool RegionWindow::Impl::initGlfw() {
    // settings_ui 可能已经 glfwInit 过：GLFW 的 init 是幂等的，重复调用直接返回成功。
    glfwSetErrorCallback(&Impl::onGlfwError);
    if (glfwInit() != GLFW_TRUE) {
        shot::Logger(host).error("[region_selector] GLFW 初始化失败");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        glfwInitialized = true;
    }
    return true;
}

bool RegionWindow::Impl::createWindow() {
    // 先把窗口 hint 复位成默认值：settings_ui 也在这个进程里建窗口，GLFW 的 hint 是
    // 全局状态，不复位就会继承别人的设置（可见性/上下文版本等）。
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);       // 先隐藏，收到触发事件再显示
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);     // 无边框
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);       // 置顶
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);     // 铺满整块屏，不需要用户拉伸
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);  // 显示时拿键盘焦点，Esc 才能生效
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    // 覆盖层是**不透明**窗口：背景靠 region_background.cpp 里"触发瞬间的桌面快照"
    // 纹理画出来，不用透明帧缓冲（透明窗口在 hide→show 之后会丢 DWM 合成，变黑屏）。
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    window = glfwCreateWindow(kInitialWindowWidth, kInitialWindowHeight, kWindowTitle, nullptr,
                              nullptr);
    if (window == nullptr) {
        shot::Logger(host).error("[region_selector] 创建 GLFW 窗口/OpenGL 上下文失败");
        return false;
    }
    crosshairCursor = glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR);
    if (crosshairCursor == nullptr) {
        shot::Logger(host).warn("[region_selector] 十字光标创建失败，用默认光标");
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // 有垂直同步就开；覆盖层每帧都是一次整屏混合，没必要超频绘制
    configureOverlayWindowNative();  // 关 DWM 显隐过渡动画（否则一出现/一关闭会闪一下）
    return true;
}

bool RegionWindow::Impl::ensureGraphics() {
    // 幂等：首次框选时把 GLFW 运行时、隐藏窗口、ImGui 后端一次建好，之后复用。
    if (window != nullptr && imguiInitialized) {
        return true;
    }
    if (!glfwInitialized) {
        if (!initGlfw()) {
            return false;
        }
    }
    if (window == nullptr && !createWindow()) {
        return false;
    }
    if (!imguiInitialized && !initImGui()) {
        // initImGui 失败时已把自己建了一半的 ImGui 资源收干净；窗口也销毁，
        // 下次框选重新来一遍，不留下半个可用的图形状态。
        shutdownWindow();
        return false;
    }
    // 实测几何写进日志：多显示器 / DPI 问题的第一现场，排查时不用再跑探针。
    if (!metricsLogged) {
        logMonitorMetrics();
        metricsLogged = true;
    }
    return true;
}

void RegionWindow::Impl::waitForEvent() {
    // 隐藏态阻塞等事件：用条件变量而不是 glfwWaitEvents。原因见文件头——
    // glfwPostEmptyEvent 唤醒的是 helper window 所属线程，可能不是我们。
    std::unique_lock<std::mutex> lock(mutex);
    eventCv.wait(lock, [this] { return quitRequested || !queue.empty(); });
}

void RegionWindow::Impl::shutdownWindow() {
    // GLFW 的错误回调是全局的：本 DLL 卸载之后它不能再指向我们的函数，所以先摘掉
    // （GLFW 文档允许传 NULL 移除回调）。settings_ui 会在自己的 init 里重设它。
    glfwSetErrorCallback(nullptr);
    if (crosshairCursor != nullptr) {
        glfwDestroyCursor(crosshairCursor);
        crosshairCursor = nullptr;
    }
    if (window != nullptr) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        glfwInitialized = false;
    }
    // 刻意不调用 glfwTerminate()：GLFW 的 init/terminate 没有引用计数，同一个进程里
    // 还有 settings_ui 在用这份全局运行时。在这里 terminate 会把别的插件的窗口一起
    // 销毁；我们的窗口已经干净销毁，剩下的 GLFW 运行时由宿主进程退出时收尾。
}

void RegionWindow::Impl::messageLoop() {
    // 窗口就绪前入队的事件（例如宿主 init 返回后立刻发了 capture.request）在这里补处理。
    drainEvents();
    while (!shouldQuit()) {
        if (overlayVisible) {
            // 框选时按 120Hz 超时轮询：鼠标拖动要跟手；有窗口消息时
            // glfwWaitEventsTimeout 会立刻返回，实际帧率由输入事件和刷新率决定。
            glfwWaitEventsTimeout(kSelectionPollSeconds);
            drainEvents();
            if (shouldQuit()) {
                break;
            }
            updateSelection();  // region_overlay.cpp：轮询拖拽/取消输入
            if (overlayVisible) {
                renderFrame();
            }
        } else {
            // 隐藏时不渲染、不碰 GLFW：阻塞在条件变量上，空闲期不占 CPU。
            waitForEvent();
            drainEvents();
        }
    }
}

void RegionWindow::Impl::drainEvents() {
    std::queue<QueuedEvent> local;
    {
        std::lock_guard<std::mutex> lock(mutex);
        local.swap(queue);
    }
    while (!local.empty()) {
        const QueuedEvent& event = local.front();
        if (event.name == kEventHostShutdown) {
            // 宿主热卸载/退出：让事件循环退出。stop() 随后 join，窗口在 run() 收尾销毁。
            shot::Logger(host).info("region_selector：收到 host.shutdown，覆盖层线程退出");
            requestQuit();
        } else if (event.name == kTopicCaptureRequest) {
            if (classifyCaptureRequest(event.payload) == RequestKind::BeginSelection) {
                if (overlayVisible) {
                    // 已经在框选中：重复触发只记一句，不重开（避免覆盖用户正在拖的选区）。
                    shot::Logger(host).debug("region_selector：正在框选，忽略重复的触发事件");
                } else if (!startSelection()) {
                    shot::Logger(host).warn("region_selector：启动框选失败（没有可用显示器？）");
                }
            }
            // 其它 capture.request（整屏采集 / 已带 region）安静忽略：它们不是给本插件的。
        }
        // 其它主题同样安静忽略：事件总线是共享的，收到不认识的主题不是错误。
        local.pop();
    }
}

void RegionWindow::Impl::enqueue(const char* name, const char* data, std::uint32_t len) {
    if (name == nullptr) {
        return;
    }
    std::string payload;
    if (data != nullptr && len > 0) {
        payload.assign(data, len);  // 契约要求：返回前把 data_len 字节拷走
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!acceptingEvents) {
            return;
        }
        queue.push(QueuedEvent{name, std::move(payload)});
    }
    // 事件已经在队列里；唤醒只是让 UI 线程立刻处理，丢了也不会丢数据
    // （它下一轮醒来还会 drain）。框选态下 UI 线程本来就在 120Hz 轮询，不依赖这次唤醒。
    eventCv.notify_one();
}

bool RegionWindow::Impl::shouldQuit() {
    std::lock_guard<std::mutex> lock(mutex);
    return quitRequested;
}

void RegionWindow::Impl::requestQuit() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        quitRequested = true;
    }
    eventCv.notify_all();
}

// ---------------------------------------------------------------------------
// 框选会话：开始 / 取消 / 确认（提交流程）
// ---------------------------------------------------------------------------
bool RegionWindow::Impl::startSelection() {
    // 第一次框选才初始化 GLFW/窗口/ImGui（原因见文件头），失败就安静放弃本次框选。
    if (!ensureGraphics()) {
        shot::Logger(host).error("[region_selector] 图形栈初始化失败，本次框选放弃");
        return false;
    }
    ScreenRect monitor;
    float contentScale = 1.0f;
    if (!monitorUnderCursor(monitor, contentScale)) {
        // 光标不在任何显示器矩形里（显示器之间的缝隙 / 远程会话刚断开）：退回主显示器，
        // 不然用户点了"区域截屏"却什么也不发生。
        GLFWmonitor* primary = glfwGetPrimaryMonitor();
        if (primary == nullptr) {
            shot::Logger(host).error("[region_selector] 找不到任何显示器，无法框选");
            return false;
        }
        monitor = monitorRectOf(primary);
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        glfwGetMonitorContentScale(primary, &scaleX, &scaleY);
        contentScale = (scaleX > 0.0f) ? scaleX : ((scaleY > 0.0f) ? scaleY : 1.0f);
        shot::Logger(host).warn("region_selector：光标不在任何显示器上，覆盖层退回主显示器");
    }
    if (monitor.width <= 0 || monitor.height <= 0) {
        shot::Logger(host).error("[region_selector] 显示器尺寸非法，无法框选");
        return false;
    }
    monitorRect = monitor;

    // ---- 顺序有讲究：全程不 hide/show，避免 DWM 的显隐过渡动画（用户看到"一闪"）----
    // 1) 先把窗口挪到屏幕外（如果已经 show 过；第一次还隐藏着就什么都不用做）。
    parkOverlay();

    // 2) 调整到目标显示器尺寸：位置/尺寸都是物理像素（PMv2），150% 屏就是 2560x1600。
    //    窗口此刻在屏幕外，不可见，调整不会闪；也绝不出现在下面的快照里。
    glfwSetWindowSize(window, monitor.width, monitor.height);
    // content scale 只用来放大字号：坐标已经是物理像素，不能再乘它。
    applyMonitorScale(contentScale);

    // 3) 截一张当前桌面的快照当覆盖层背景（窗口不透明，靠这张纹理"假装透明"）。
    //    失败只 warn：退化成纯遮罩背景，框选功能不受影响。
    if (!captureBackground()) {
        shot::Logger(host).warn("region_selector：没有桌面快照背景，覆盖层只显示遮罩");
    }

    // 4) 初始光标位置：用 Win32 GetCursorPos（屏幕物理像素）减去显示器原点。
    //    窗口在屏幕外时 glfwGetCursorPos 给的是相对窗口的坐标，不可用，所以走平台层。
    drag = DragState{};
    int cursorScreenX = 0;
    int cursorScreenY = 0;
    if (cursorScreenPosition(cursorScreenX, cursorScreenY)) {
        drag.cursorX = std::clamp(cursorScreenX - monitor.x, 0, std::max(0, monitor.width - 1));
        drag.cursorY = std::clamp(cursorScreenY - monitor.y, 0, std::max(0, monitor.height - 1));
    }
    drag.startX = drag.cursorX;
    drag.startY = drag.cursorY;
    // 显示覆盖层时左键可能还按着（从托盘菜单触发）：先等松开，别把菜单点击当确认。
    drag.waitRelease = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (crosshairCursor != nullptr) {
        glfwSetCursor(window, crosshairCursor);
    }

    // 5) 在屏幕外先把这一帧渲染好（快照 + 遮罩 + 十字线），再挪到目标显示器：
    //    挪过去时内容已经正确，不会出现"先黑/先旧内容再刷新"的一帧。
    for (int attempt = 0; attempt < 2; ++attempt) {
        renderFrame();
        glfwPollEvents();
    }
    glfwSetWindowPos(window, monitor.x, monitor.y);
    glfwSetWindowAttrib(window, GLFW_FLOATING, GLFW_TRUE);
    if (!windowShown) {
        glfwShowWindow(window);  // 只在第一次框选时 show 一次
        windowShown = true;
    }
    glfwFocusWindow(window);
    overlayVisible = true;

    // 把覆盖层几何与换算基准写进日志：落盘区域不对时第一眼看这几行。
    int desktopX = 0;
    int desktopY = 0;
    virtualDesktopOrigin(desktopX, desktopY);
    char text[320] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：开始框选 显示器矩形=(%d,%d) %dx%d content_scale=%.2f "
                  "虚拟桌面origin=(%d,%d)（发布 region 时用 absolute - origin）",
                  monitor.x, monitor.y, monitor.width, monitor.height, contentScale, desktopX,
                  desktopY);
    shot::Logger(host).info(text);
    return true;
}

void RegionWindow::Impl::cancelSelection() {
    // v1 不定义 capture.cancelled：取消就是"什么都不发"，把覆盖层收掉即可。
    shot::Logger(host).info("region_selector：框选已取消（不发布事件）");
    hideOverlay();
}

void RegionWindow::Impl::hideOverlay() {
    parkOverlay();
    overlayVisible = false;
    drag = DragState{};
}

void RegionWindow::Impl::parkOverlay() {
    if (window == nullptr) {
        return;
    }
    // 取消置顶 + 挪到屏幕外：等价于"隐藏"，但不触发 DWM 的 show/hide 过渡。
    glfwSetWindowAttrib(window, GLFW_FLOATING, GLFW_FALSE);
    glfwSetWindowPos(window, kParkedX, kParkedY);
}

void RegionWindow::Impl::finishSelection(int localX, int localY, int localWidth, int localHeight) {
    drag = DragState{};
    // 任一边小于最小尺寸 = 没选中什么（误点/手抖）：按取消处理，不发布事件。
    if (localWidth < kMinimumSelectionSize || localHeight < kMinimumSelectionSize) {
        char text[128] = {};
        std::snprintf(text, sizeof(text),
                      "region_selector：选区过小（%dx%d < %dpx），按取消处理", localWidth,
                      localHeight, kMinimumSelectionSize);
        shot::Logger(host).info(text);
        hideOverlay();
        return;
    }

    // 窗口内容坐标 → 绝对屏幕坐标 → 虚拟桌面坐标。
    //   · glfwGetWindowPos 是窗口内容区在屏幕上的位置（物理像素，实测 = 显示器位置）；
    //   · 本地坐标是物理像素（PMv2 下 framebuffer == 窗口大小）；
    //   · capture.request 的 region 原点 = 虚拟桌面左上角（screen_capture 的 describe
    //     用 SM_XVIRTUALSCREEN/SM_YVIRTUALSCREEN），所以还要减去虚拟桌面原点。
    int windowX = 0;
    int windowY = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    int desktopX = 0;
    int desktopY = 0;
    virtualDesktopOrigin(desktopX, desktopY);
    ScreenRect region;
    region.x = windowX + localX - desktopX;
    region.y = windowY + localY - desktopY;
    region.width = localWidth;
    region.height = localHeight;

    // ---- 关键时序：先隐藏覆盖层，等遮罩真的从屏幕上消失，再发布 ----
    // DWM 合成是异步的：glfwHideWindow 返回时画面可能还没更新；宿主 worker 收到
    // capture.request 就抓屏，中间没有同步点。所以这里主动等 100ms，宁可慢一点也不能
    // 把半透明遮罩截进图里。glfwPollEvents 先把窗口管理器的隐藏消息处理掉。
    hideOverlay();
    glfwPollEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(kHideSettleDelayMs));
    // 等待期间宿主可能已经开始卸载本插件：那样就不要在 deinit 之后再发事件。
    if (shouldQuit()) {
        shot::Logger(host).info("region_selector：宿主正在退出，丢弃本次已确认的区域");
        return;
    }
    publishRegion(region);
}

void RegionWindow::Impl::publishRegion(const ScreenRect& region) const {
    shot::EventBus bus(host);
    if (!bus.available()) {
        shot::Logger(host).warn("region_selector：宿主没有事件总线，区域结果无法交给宿主");
        return;
    }
    // 负载格式与 document/CAPTURE.md 约定一致：坐标是物理虚拟桌面像素（int）。
    const nlohmann::json payload = {
        {"mode", "region"},
        {"region",
         {{"x", region.x}, {"y", region.y}, {"width", region.width}, {"height", region.height}}},
    };
    const std::string text = payload.dump();
    if (!bus.publish(kTopicCaptureRequest, text)) {
        shot::Logger(host).warn("region_selector：发布 capture.request 失败（队列满或参数非法）");
        return;
    }
    char message[256] = {};
    std::snprintf(message, sizeof(message),
                  "region_selector：已发布区域请求 region={x:%d,y:%d,width:%d,height:%d}（物理虚拟桌面像素）",
                  region.x, region.y, region.width, region.height);
    shot::Logger(host).info(message);
}

// ---------------------------------------------------------------------------
// 事件总线 / GLFW 错误回调
// ---------------------------------------------------------------------------
void RegionWindow::Impl::onBusEvent(void* user, const char* topic, const void* data,
                                    std::uint32_t len) {
    auto* self = static_cast<Impl*>(user);
    if (self == nullptr) {
        return;
    }
    // 回调跑在宿主分发线程上：只入队 + 唤醒，解析/开窗口留给 UI 线程。
    self->enqueue(topic, static_cast<const char*>(data), len);
}

void RegionWindow::Impl::onGlfwError(int errorCode, const char* description) {
    const char* text = (description != nullptr) ? description : "(no description)";
    const ShotHostServices* logHost = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_glfwErrorMutex);
        logHost = g_glfwErrorHost;
    }
    shot::Logger(logHost).error("[region_selector] GLFW 错误 " + std::to_string(errorCode) + "：" +
                                text);
}

// ---------------------------------------------------------------------------
// 公共接口
// ---------------------------------------------------------------------------
RegionWindow::RegionWindow() : m_impl(std::make_unique<Impl>()) {}

RegionWindow::~RegionWindow() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool RegionWindow::start(const ShotHostServices* host) {
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

    // 先订阅再起线程：订阅之后到达的触发事件不会漏；窗口还没就绪时事件先入队，
    // 事件循环启动后统一 drain。
    shot::EventBus bus(host);
    if (bus.available()) {
        m_impl->busSubscribed =
            bus.subscribe(kTopicCaptureRequest, &Impl::onBusEvent, m_impl.get());
        if (!m_impl->busSubscribed) {
            shot::Logger(host).warn(
                "region_selector：订阅 capture.request 失败，覆盖层只能靠 post_to_plugin 触发");
        }
    } else {
        // 旧版宿主没有事件总线：安静降级，覆盖层不会自动弹出。
        shot::Logger(host).warn(
            "region_selector：宿主未提供事件总线（旧版宿主？），覆盖层只能靠 post_to_plugin 触发");
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

void RegionWindow::stop() {
    if (!m_impl) {
        return;
    }
    // 1. 先退订事件总线：deinit 返回后宿主不再回调本插件（EVENT_BUS.md §5）。
    if (m_impl->busSubscribed) {
        shot::EventBus bus(m_impl->host);
        bus.unsubscribe(m_impl.get());
        m_impl->busSubscribed = false;
    }
    // 2. 拒绝后续投递，并让事件循环退出（隐藏时它阻塞在 eventCv 上）。
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
    // UI 线程已经结束，这两个只属于 UI 线程的状态可以安全复位。
    m_impl->overlayVisible = false;
    m_impl->drag = DragState{};
}

std::int32_t RegionWindow::post(const char* name, const char* data, std::uint32_t len) {
    if (!m_impl || name == nullptr) {
        return -1;
    }
    try {
        std::string payload;
        if (data != nullptr && len > 0) {
            payload.assign(data, len);  // 契约要求：返回前把 data_len 字节拷走
        }
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (!m_impl->acceptingEvents) {
                return 1;  // 窗口不在（还没起好 / 正在关闭）：丢弃
            }
            m_impl->queue.push(QueuedEvent{name, std::move(payload)});
        }
        // 与 enqueue 同一条规则：事件已经在队列里，唤醒丢了也只是晚一点处理。
        m_impl->eventCv.notify_one();
        return 0;
    } catch (...) {
        return 2;  // 分配失败等：异常绝不跨越 DLL 边界
    }
}

}  // namespace region_selector
