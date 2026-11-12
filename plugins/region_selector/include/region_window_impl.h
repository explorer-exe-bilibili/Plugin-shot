#pragma once

// ============================================================================
//  region_window_impl.h —— 内部头：RegionWindow 的 PIMPL 与覆盖层内部类型
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。GLFW 只以 `struct GLFWwindow;`
//  等前置声明出现在这里，本头不拖 <GLFW/glfw3.h>，也不含任何平台头；公共头
//  region_window.h 不包含任何图形库类型。
//
//  定义按职责拆成多个 .cpp（STYLE.md §6.7/§6.8）：
//    region_window.cpp   —— 生命周期 / 窗口线程 / GLFW 事件循环 / 事件队列 /
//                           触发负载解析 / 隐藏后发布（提交）
//    region_overlay.cpp  —— 框选交互状态机 + 悬停吸附动画
//    region_overlay_draw.cpp —— ImGui 遮罩 / 选区 / 折角 / 标签绘制
//    region_graphics.cpp —— ImGui 后端、字体与 DPI 缩放、每帧渲染
//    region_background.cpp —— Windows：触发瞬间的桌面快照（Win32 GDI）+ 上传成 GL 纹理
//    region_geometry.cpp —— Windows：显示器矩形 / 光标 / 物理虚拟桌面坐标换算
//    window_target.cpp   —— Windows：顶层窗口枚举 / Z 序 / DWM 边界 / 命中测试
//
//  Linux 分支（只在 UNIX AND NOT APPLE 构建）同时编进两套平台实现，运行期按
//  LinuxSessionBackend 二选一（见 README「Linux（X11）」）：
//
//  Wayland（文件以 _wayland 结尾）：
//    region_portal.cpp / region_image.cpp / region_output_linux.cpp
//                        —— portal 截图、PNG 解码裁剪、输出目录与文件名
//    region_geometry_wayland.cpp / region_background_wayland.cpp
//                        —— 显示器几何、portal 截图转纹理（与 Windows 同名成员函数的另一实现）
//    region_overlay_wayland.cpp —— 等比适配映射 + 内存裁剪写盘 + 发布 capture.done
//    window_target_wayland.cpp  —— 空实现（Wayland 不允许全局窗口枚举，悬停吸附关闭）
//
//  X11（文件以 _x11 结尾；与 Wayland 用不同的成员函数名，避免同一符号两份定义）：
//    region_geometry_x11.cpp   —— X screen 几何 / 光标 / 显示器矩形 + 实测日志
//    region_background_x11.cpp —— XGetImage 整屏快照 → GL 纹理
//    window_target_x11.cpp     —— XQueryTree 顶层窗口枚举 / EWMH 类型过滤 / 命中测试
//    region_overlay_x11.cpp    —— 覆盖层定位 / 实际几何日志 / 确认后的发布（与 Windows 同一时序）
// ============================================================================

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "plugin_api.h"
#include "region_window.h"
#include "window_target.h"

#if defined(__linux__)
#include "region_image.h"  // ImageRect：Wayland 分支的裁剪矩形类型（见 Impl 末尾的 Linux 段）
#endif

struct GLFWwindow;   // 前置声明：内部头不拖 GLFW/GL 头文件
struct GLFWmonitor;
struct GLFWcursor;

namespace shot {
class I18n;
}  // namespace shot

namespace region_selector {

/** @brief 触发主题：负载 `{"mode":"region"}` 启动框选（见 document/CAPTURE.md）。 */
inline constexpr char kTopicCaptureRequest[] = "capture.request";

/**
 * @brief 选择握手：别的插件请求"先选目标区域"（负载 `{"purpose":"..."}`）。
 *
 * 与 capture.request 的区别只在确认后的产出：普通框选发带 region 的
 * capture.request（交给宿主流水线采集）；带 purpose 的选择只回
 * region.selected {purpose, region}，**不发 capture.request**，由发起方自己采集。
 * 这样长截图这类"自带采集层"的消费者不会触发宿主的整屏采集/落盘。
 */
inline constexpr char kTopicRegionSelectRequest[] = "region.select.request";

/** @brief 选择握手的结果主题：`{purpose, region:{x,y,width,height}}` 或 `{purpose, cancelled:true}`。 */
inline constexpr char kTopicRegionSelected[] = "region.selected";

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

/**
 * @brief 悬停吸附的指数平滑时间常数（秒）。
 *
 * 100ms 是本需求拍板的观感：窗口切换时选框"滑"过去而不是瞬移，但 3~4 帧内就能
 * 走完大部分距离，不会让用户觉得延迟。实现是每帧 alpha = 1 - exp(-dt/tau)。
 */
inline constexpr double kHoverSmoothTimeConstantSeconds = 0.1;

/** @brief 窗口枚举节流间隔（毫秒）：覆盖层铺满屏幕，每帧枚举上百个窗口太浪费。 */
inline constexpr int kWindowScanIntervalMs = 30;

/** @brief 单击判定：按下到松开的最大位移（物理像素）。超过就进入拖动模式。 */
inline constexpr int kClickMaxMovePixels = 4;

/** @brief 单击判定：按下到松开的最长按住时间（毫秒）。 */
inline constexpr int kClickMaxHoldMs = 400;

/** @brief 队列里的一个事件：主题名 + 负载字节（总线单条负载上限 64 KiB）。 */
struct QueuedEvent {
    std::string name;
    std::string payload;
};

/** @brief 物理像素矩形：绝对屏幕坐标（Windows = 虚拟桌面坐标；X11 = root 坐标，原点恒为 (0,0)）。 */
struct ScreenRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/** @brief 一次框选的交互状态（只在 UI 线程读写）。
 *
 * 位置都是**覆盖窗口内容坐标**（物理像素，与窗口大小同一套单位；X11 / Windows 实测
 * framebuffer == window size，所以直接等于绘制用的像素坐标）。
 */
struct DragState {
    bool dragging = false;    /**< 已越过单击阈值：选框跟手，不做吸附/动画。 */
    bool pressed = false;     /**< 左键已按下：可能是单击，也可能是拖拽的第一个阶段。 */
    bool waitRelease = false; /**< 显示覆盖层时左键还按着：先等它松开，别把菜单点击当确认。 */
    int startX = 0;           /**< 按下 / 拖拽起点 x。 */
    int startY = 0;           /**< 按下 / 拖拽起点 y。 */
    int cursorX = 0;          /**< 当前光标 x。 */
    int cursorY = 0;          /**< 当前光标 y。 */
    /** 按下时刻：松开时用它判断"按住 ≤400ms"的单击。 */
    std::chrono::steady_clock::time_point pressTime;
};

/**
 * @brief 悬停吸附框：锚点目标矩形 + 当前显示矩形 + 上次动画时刻。
 *
 * 坐标是覆盖层内容坐标（物理像素）。当前显示矩形用 float：每帧按指数曲线逼近
 * 目标，若存整数会因反复取整产生 ±1px 抖动。目标矩形保持整数——它是真实窗口
 * 矩形，单击确认时直接用 target，而不是动画中的 current。
 */
struct HoverBox {
    bool active = false;       /**< 光标当前是否落在某个候选窗口上。 */
    bool initialized = false;  /**< 是否已经有过显示位置：显示首帧直接对齐，不从 (0,0) 飞入。 */
    std::uintptr_t hwnd = 0;   /**< 当前目标窗口句柄（只用于变化时打 debug 日志）。 */
    ScreenRect target;         /**< 目标矩形（内容坐标）；单击确认时用这个。 */
    float x = 0.0f;            /**< 当前显示矩形左边缘（内容坐标）。 */
    float y = 0.0f;            /**< 当前显示矩形上边缘。 */
    float width = 0.0f;        /**< 当前显示矩形宽度。 */
    float height = 0.0f;       /**< 当前显示矩形高度。 */
    std::chrono::steady_clock::time_point lastTick; /**< 上一帧动画时刻（算 dt 用）。 */
};

/**
 * @brief Linux 会话的显示后端：运行期二选一（见 README「Linux（X11）」）。
 *
 * Linux 的同一份 `.so` 同时编进 Wayland 与 X11 两套平台实现，启动时按环境变量
 * 决定走哪套（显示服务器不会在进程运行期切换，所以判定一次即可）。Wayland 优先：
 * Wayland 会话里 XWayland 也可能导出 DISPLAY，但能真正覆盖所有输出的是门户路径。
 */
enum class LinuxSessionBackend {
    Wayland, /**< `WAYLAND_DISPLAY` / `XDG_SESSION_TYPE=wayland`：门户定格图 + 自有窗口拖选。 */
    X11,     /**< 其它会话：XGetImage 快照 + 全屏覆盖层 + XQueryTree 窗口命中。 */
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
    std::string selectionPurpose;  // 非空 = 本次选择由 region.select.request 发起，确认后回 region.selected
    DragState drag;               // 当前框选状态
    HoverBox hover;               // 悬停窗口吸附框（目标 + 动画中的显示矩形）
    std::vector<WindowTarget> windowTargets;  // 最近一次枚举结果（绝对物理像素，Z 序）
    std::chrono::steady_clock::time_point lastWindowScan;  // 上次枚举时刻（30ms 节流）
    bool windowScanValid = false;      // windowTargets 是否有有效快照
    std::uintptr_t overlayHwnd = 0;    // 覆盖层自己的 HWND：枚举时必须排除
    std::uint32_t themeColorRgb = 0x4C9AFFu;  // 主题色（0xRRGGBB，每次显示读一次配置）
    float uiScale = 1.0f;              // 当前显示器 content scale：只放大字号与装饰
    ScreenRect monitorRect;            // 当前覆盖的显示器矩形（绝对物理像素）
#if defined(__linux__)
    // Linux 运行期后端：start() 里按环境变量判定一次，之后整个会话不变。
    LinuxSessionBackend linuxBackend = LinuxSessionBackend::Wayland;
#endif
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
    bool startSelection(const std::string& purpose);
    void cancelSelection();
    void finishSelection(int localX, int localY, int localWidth, int localHeight, bool windowClick);
    void hideOverlay();
    void publishRegion(const ScreenRect& region) const;
    bool beginPurposeSelection(const std::string& payload);
    void publishPurposeSelection(const std::string& purpose, const ScreenRect& region) const;
    void publishPurposeCancelled(const std::string& purpose) const;
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);
    static void onGlfwError(int errorCode, const char* description);

    // ---- 覆盖层交互与绘制（region_overlay.cpp / region_overlay_draw.cpp） ----
    void updateSelection();
    void updateHover();
    void animateHover();
    void loadThemeColor();
    void drawOverlay() const;
    void applyMonitorScale(float scale);
    /** @brief 由任意两个对角点生成左上角 + 非负宽高；交互与绘制两个 TU 共用。 */
    static void normalizeRect(int ax, int ay, int bx, int by, int& x, int& y, int& width,
                              int& height);

    // ---- 图形层：ImGui 后端 / 字体 / 每帧渲染（region_graphics.cpp） ----
    bool initImGui();
    void loadFonts();
    void shutdownGraphics();
    bool renderFrame();

    // ---- 桌面快照背景（region_background.cpp） ----
    bool captureBackground();
    void releaseBackgroundTexture();

    // ---- 平台几何：显示器与物理坐标（Windows: region_geometry.cpp） ----
    bool monitorUnderCursor(ScreenRect& out, float& contentScale) const;
    void virtualDesktopOrigin(int& x, int& y) const;
    std::uintptr_t nativeWindowHandle() const;
    void logMonitorMetrics() const;
    static ScreenRect monitorRectOf(GLFWmonitor* monitor);

#if defined(__linux__)
    // ---- Linux / Wayland 分支（只由 *_wayland.cpp / region_output_linux.cpp 实现） ----
    /**
     * @brief 截图在覆盖层窗口里的显示区域：等比缩放适配 + 居中（全部是窗口内容坐标）。
     *
     * 绘制背景、坐标反算、尺寸标签共用这一份布局，避免三处各算一套后互相漂移。
     * 每帧/每次确认都按当前窗口尺寸重算——Wayland 的窗口大小由合成器决定，
     * 不能假设"窗口 == 显示器"。
     */
    struct WaylandLayout {
        float scale = 1.0f;   /**< 窗口像素 / 图像像素。 */
        float offsetX = 0.0f; /**< 显示区左边缘（窗口内容坐标）。 */
        float offsetY = 0.0f; /**< 显示区上边缘（窗口内容坐标）。 */
        float width = 0.0f;   /**< 显示区宽度（窗口内容坐标）。 */
        float height = 0.0f;  /**< 显示区高度（窗口内容坐标）。 */
    };
    /** @brief 计算当前的显示布局；没有截图或窗口未就绪时返回零布局（scale 仍为 1）。 */
    WaylandLayout overlayLayout() const;
    /** @brief 覆盖层窗口坐标里的选区 → 截图图像像素矩形（夹进图像范围）。 */
    ImageRect selectionToImagePixels(int x, int y, int width, int height) const;
    /** @brief 显示前把覆盖层尺寸对齐输出（Wayland 不能定位窗口）。 */
    void prepareWaylandOverlay(const ScreenRect& monitor);
    /** @brief 窗口显示后请求全屏（是否生效由合成器决定）。 */
    void requestWaylandFullscreen();
    /** @brief 确认框选：内存裁剪 → 写 PNG → 发布 capture.done → 收掉覆盖层。 */
    void commitWaylandSelection(int x, int y, int width, int height);
    /** @brief 发布 capture.done {path,width,height,processors:0}（与宿主流水线同格式）。 */
    void publishCaptureDone(const std::string& path, int width, int height) const;

    // portal 截图的整屏像素：既是覆盖层背景，也是确认时的裁剪源（不再第二次采集）。
    std::vector<std::uint8_t> sourcePixels;
    int sourceWidth = 0;
    int sourceHeight = 0;

    // ---- Linux / X11 分支（只由 *_x11.cpp 实现；与上面的 Wayland 成员函数不同名） ----
    /** @brief 光标所在显示器：XQueryPointer 取 root 坐标 + GLFW 显示器矩形。 */
    bool x11MonitorUnderCursor(ScreenRect& out, float& contentScale) const;
    /** @brief 左键当前是否按着（XQueryPointer 的 Button1Mask）：覆盖层显示前判 waitRelease。 */
    bool x11LeftButtonHeld() const;
    /** @brief X11 只有一块 X screen，root 原点就是 (0,0)；保留接口与 Windows 对称。 */
    void x11VirtualDesktopOrigin(int& x, int& y) const;
    /** @brief 覆盖层自身的 XID（glfwGetX11Window）；枚举时必须排除。 */
    std::uintptr_t x11NativeWindowHandle() const;
    /** @brief 把 X screen 尺寸与各显示器矩形写进日志（首次框选时一次）。 */
    void x11LogMonitorMetrics() const;
    /** @brief XGetImage 抓整块 X screen 并上传成 GL 纹理；失败时只 WARN、覆盖层退化为纯色底。 */
    bool x11CaptureBackground();
    /** @brief 把隐藏的覆盖层窗口搬到目标显示器并铺满（X11 客户端可以自己定位）。 */
    void x11PlaceOverlay(const ScreenRect& monitor);
    /** @brief 显示后记录实际窗口几何：WM 可能不按请求摆放，坐标一律按实际位置换算。 */
    void x11LogOverlayGeometry() const;
    /** @brief X11 确认框选：内容坐标 → X screen 坐标，然后按 Windows 同一时序发布。 */
    void x11CommitSelection(int localX, int localY, int localWidth, int localHeight);
#endif
};

}  // namespace region_selector
