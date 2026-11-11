#pragma once

// ============================================================================
//  region_window_impl.h —— 内部头：RegionWindow 的 PIMPL 与覆盖层内部类型
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。GLFW 只以 `struct GLFWwindow;`
//  等前置声明出现在这里，本头不拖 <GLFW/glfw3.h>，也不含任何平台头；公共头
//  region_window.h 不包含任何图形库类型。
//
//  定义按职责拆成四个 .cpp（STYLE.md §6.7/§6.8）：
//    region_window.cpp   —— 生命周期 / 窗口线程 / GLFW 事件循环 / 事件队列 /
//                           触发负载解析 / 隐藏后发布（提交）
//    region_overlay.cpp  —— 框选交互状态机 + ImGui 遮罩绘制
//    region_graphics.cpp —— ImGui 后端、字体与 DPI 缩放、每帧渲染
//    region_background.cpp —— 触发瞬间的桌面快照（Win32 GDI）+ 上传成 GL 纹理
//    region_geometry.cpp —— 显示器矩形 / 光标 / 物理虚拟桌面坐标换算（Win32 平台层）
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "plugin_api.h"
#include "region_window.h"

struct GLFWwindow;   // 前置声明：内部头不拖 GLFW/GL 头文件
struct GLFWmonitor;
struct GLFWcursor;

namespace shot {
class I18n;
}  // namespace shot

namespace region_selector {

/** @brief 触发主题：负载 `{"mode":"region"}` 启动框选（见 document/CAPTURE.md）。 */
inline constexpr char kTopicCaptureRequest[] = "capture.request";

/** @brief 宿主卸载 UI 插件前投递的事件（走 post_to_plugin，不走总线）。 */
inline constexpr char kEventHostShutdown[] = "host.shutdown";

/** @brief 最小选区边长（物理像素）：任一边小于它当取消，避免误点确认出 1x1 的图。 */
inline constexpr int kMinimumSelectionSize = 8;

/**
 * @brief 隐藏覆盖层到发布 capture.request 之间的等待（毫秒）。
 *
 * DWM 的合成是异步的：glfwHideWindow 返回时覆盖层可能还没从屏幕上消失。
 * 实测 100ms 足够让隐藏生效（宿主 worker 收到事件后先抓屏再裁剪，抓屏时
 * 覆盖层已经不在画面上）。等太短会把遮罩截进图里，等太久用户会觉得卡。
 */
inline constexpr int kHideSettleDelayMs = 100;

/** @brief 框选时的事件轮询间隔（秒）：鼠标拖动要跟手，有事件时 glfwWaitEventsTimeout 立即返回。 */
inline constexpr double kSelectionPollSeconds = 1.0 / 120.0;

/** @brief 队列里的一个事件：主题名 + 负载字节（总线单条负载上限 64 KiB）。 */
struct QueuedEvent {
    std::string name;
    std::string payload;
};

/** @brief 物理像素矩形：绝对屏幕坐标（虚拟桌面坐标系里的一块，原点不一定是 0,0）。 */
struct ScreenRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/**
 * @brief 一次框选的交互状态（只在 UI 线程读写）。
 *
 * 位置都是**覆盖窗口内容坐标**（物理像素，与窗口大小同一套单位；本机实测
 * framebuffer == window size，所以直接等于绘制用的像素坐标）。
 */
struct DragState {
    bool dragging = false;    /**< 已经看到一次左键按下，正在拖。 */
    bool waitRelease = false; /**< 显示覆盖层时左键还按着：先等它松开，别把菜单点击当确认。 */
    int startX = 0;           /**< 拖拽起点 x。 */
    int startY = 0;           /**< 拖拽起点 y。 */
    int cursorX = 0;          /**< 当前光标 x。 */
    int cursorY = 0;          /**< 当前光标 y。 */
};

/**
 * @brief RegionWindow 的实现：GLFW 窗口、事件队列、线程握手与框选状态全在这里。
 *
 * 声明集中在这个结构里，定义按职责分散到多个 .cpp（STYLE.md §6.7）。
 */
struct RegionWindow::Impl {
    // ---- start() / stop() 跨线程共享的状态（mutex 保护） ----
    std::thread thread;
    std::mutex mutex;
    std::queue<QueuedEvent> queue;
    // 隐藏态的事件唤醒：不能用 glfwPostEmptyEvent（见 region_window.cpp 文件头）。
    std::condition_variable eventCv;
    bool acceptingEvents = false;  // start 成功拉起线程后为 true；stop 一开始就置 false
    bool glfwInitialized = false;  // 本插件是否已经调用过 glfwInit（进程级 GLFW 可能更早被别处初始化）
    bool quitRequested = false;    // stop() / host.shutdown 要求事件循环退出

    // ---- start() 与 UI 线程之间的"窗口 + ImGui 就绪了没有"握手 ----
    std::mutex readyMutex;
    std::condition_variable readyCv;
    bool ready = false;
    bool failed = false;

    const ShotHostServices* host = nullptr;
    // 多语言表：由插件状态持有，这里只借指针（不拥有）。start() 时设置、stop()
    // join 之后清空；UI 线程渲染帧里直接调用 tr()，所以生命周期必须覆盖整个运行期。
    const shot::I18n* i18n = nullptr;
    bool busSubscribed = false;

    // ---- 以下只在 UI 线程（GLFW 的主线程）上访问 ----
    GLFWwindow* window = nullptr;
    GLFWcursor* crosshairCursor = nullptr;  // 覆盖层显示时用的十字光标
    bool imguiInitialized = false;
    bool metricsLogged = false;   // 显示器几何实测日志只打一次
    bool overlayVisible = false;  // 覆盖层是否正在显示
    DragState drag;               // 当前框选状态
    ScreenRect monitorRect;       // 当前覆盖的显示器矩形（绝对物理像素）
    // 触发瞬间的桌面快照纹理：覆盖层窗口是不透明的，靠它当"背景画面"，
    // 这样就不依赖透明窗口（透明帧缓冲在 hide/show 之后会丢 DWM 合成，导致黑屏）。
    unsigned int backgroundTexture = 0;
    int backgroundWidth = 0;
    int backgroundHeight = 0;

    // ---- 生命周期与事件循环（region_window.cpp） ----
    void run();
    void messageLoop();
    void drainEvents();
    void enqueue(const char* name, const char* data, std::uint32_t len);
    bool shouldQuit();
    void requestQuit();
    bool initGlfw();
    bool createWindow();
    bool ensureGraphics();
    void waitForEvent();
    void shutdownWindow();
    bool startSelection();
    void cancelSelection();
    void finishSelection(int localX, int localY, int localWidth, int localHeight);
    void hideOverlay();
    void publishRegion(const ScreenRect& region) const;
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);
    static void onGlfwError(int errorCode, const char* description);

    // ---- 覆盖层交互与绘制（region_overlay.cpp） ----
    void updateSelection();
    void drawOverlay() const;
    void applyMonitorScale(float scale);

    // ---- 图形层：ImGui 后端 / 字体 / 每帧渲染（region_graphics.cpp） ----
    bool initImGui();
    void loadFonts();
    void shutdownGraphics();
    bool renderFrame();

    // ---- 桌面快照背景（region_background.cpp） ----
    bool captureBackground();
    void releaseBackgroundTexture();

    // ---- 平台几何：显示器与物理坐标（region_geometry.cpp） ----
    bool monitorUnderCursor(ScreenRect& out, float& contentScale) const;
    void virtualDesktopOrigin(int& x, int& y) const;
    void logMonitorMetrics() const;
    static ScreenRect monitorRectOf(GLFWmonitor* monitor);
};

}  // namespace region_selector
