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
//    region_overlay_x11.cpp    —— EWMH 全屏（让 mutter 隐藏顶栏/dock）/ 状态校验与降级 /
//                                 焦点与键盘 grab / 实际几何日志 / 确认后的发布（Windows 同一时序）
// ============================================================================

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "plugin_api.h"
#include "region_window.h"
#include "window_target.h"

#if defined(__linux__)
#include "region_image.h"    // ImageRect：Wayland 分支的裁剪矩形类型（见 Impl 末尾的 Linux 段）
#include "region_portal.h"   // PortalScreenshotStatus：startSelection 用来分级记录失败原因
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

#if defined(__linux__)
/**
 * @brief Wayland 隐藏覆盖层的等待上限（毫秒）。
 *
 * `glfwHideWindow` 在 Wayland 上只是把 unmap 请求（xdg_toplevel destroy + attach(NULL)
 * + commit）写进 `wl_display` 的发送缓冲，真正发出去靠下一次 `glfw*Events()` 里的
 * `wl_display_flush`。所以隐藏之后不能只 poll 一两次就回去阻塞：要**用事件泵等**，
 * 直到 GLFW 报告隐藏或超时。上限 500ms 是"用户能忍、又能覆盖一次合成器往返"的折中；
 * 超时也继续（记 WARN），绝不把 UI 线程按在这里更久——UI 线程一停就不再 dispatch
 * Wayland 事件，合成器 ping 无人应答，GNOME 会直接报"应用没有响应"。
 */
inline constexpr int kHideWaitTimeoutMs = 500;

/** @brief 隐藏等待每轮的 `glfwWaitEventsTimeout` 秒数（也会 flush，见上）。 */
inline constexpr double kHideWaitRoundSeconds = 0.005;

/**
 * @brief 隐藏等待至少泵几轮才认为"隐藏完成"。
 *
 * 关键事实（GLFW 3.4 `wl_window.c`）：`_glfwHideWindowWayland` 在**客户端**把
 * `window->wl.visible` 置 false，所以 `glfwGetWindowAttrib(GLFW_VISIBLE)` 在
 * `glfwHideWindow` 返回后立刻就是 0，它**不能证明**合成器已经 unmap。因此不能
 * "看到 0 就走"，至少泵几轮，保证 flush 发生过、并给合成器一次处理 unmap 的机会。
 */
inline constexpr int kHideMinPumpRounds = 4;

/** @brief 隐藏等待里逐轮打日志的轮数（之后的轮次只在结束时汇总，避免刷屏）。 */
inline constexpr int kHideLogRounds = 6;

/**
 * @brief 回收上一次 Wayland 提交线程的最长等待（毫秒）。
 *
 * join 一个还在编码的线程会把 UI 线程按在原地几百毫秒到数秒（4K PNG 编码实测可达
 * 数秒）。这里只等这么久，没跑完就不等：线程留在 `commitWorkers` 里，由下一次回收
 * 或 `stop()` 收掉。
 */
inline constexpr int kCommitReapTimeoutMs = 200;
#endif

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

/**
 * @brief 屏幕四角"吸附当前显示器全屏"的方块边长（物理像素）。
 *
 * 用户要求：光标落在**当前显示器**四角的 20x20 像素方块里（距两条相邻边都 < 20px）
 * 时，把"当前显示器全屏"当作悬停目标——那里通常是桌面/任务栏边缘，直接框整屏比
 * "选中被角落盖住的那个窗口"更符合直觉。角落优先于窗口命中，原因见
 * RegionWindow::Impl::resolveHoverHit。
 */
inline constexpr int kCornerSnapSize = 20;

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
 * @brief 悬停吸附命中的类别（决定日志文案与"要不要换成当前显示器全屏"）。
 *
 * 枚举值刻意不叫 `None`：Xlib 的 `X11/Xlib.h` 把 `None` 定义成宏（0L），而本插件
 * 的 X11 平台层先包含 Xlib 再包含本头，用 `None` 会被预处理成数字，编译直接报
 * "expected identifier before numeric constant"（Linux 构建实测）。
 */
enum class HoverTargetKind {
    NoTarget, /**< 光标下没有可吸附的目标。 */
    Window,   /**< 普通顶层窗口：吸附到该窗口矩形。 */
    Panel,    /**< 任务栏 / 面板（Windows Shell_TrayWnd、X11 DOCK）：吸附到当前显示器全屏。 */
    Corner,   /**< 屏幕四角方块（见 kCornerSnapSize）：吸附到当前显示器全屏。 */
};

/**
 * @brief 一次悬停判定的结果：命中的类别 + 吸附矩形 + 原生句柄。
 *
 * 矩形是**绝对屏幕坐标**（物理像素），由调用方减去覆盖层窗口位置换成绘制用的
 * 内容坐标。Panel / Corner 的目标是"当前显示器全屏"这个伪窗口，所以没有真实句柄。
 */
struct HoverHit {
    HoverTargetKind kind = HoverTargetKind::NoTarget; /**< 命中类别。 */
    std::uintptr_t hwnd = 0; /**< 仅 Window 有值（原生句柄）；其余类别恒为 0。 */
    ScreenRect rect;         /**< 吸附矩形（绝对屏幕坐标）；kind == NoTarget 时无意义。 */
};

/**
 * @brief 悬停吸附框：锚点目标矩形 + 当前显示矩形 + 上次动画时刻。
 *
 * 坐标是覆盖层内容坐标（物理像素）。当前显示矩形用 float：每帧按指数曲线逼近
 * 目标，若存整数会因反复取整产生 ±1px 抖动。目标矩形保持整数——它是真实窗口
 * 矩形（或当前显示器全屏），单击确认时直接用 target，而不是动画中的 current。
 */
struct HoverBox {
    bool active = false;       /**< 光标当前是否落在某个候选目标上。 */
    bool initialized = false;  /**< 是否已经有过显示位置：显示首帧直接对齐，不从 (0,0) 飞入。 */
    std::uintptr_t hwnd = 0;   /**< 当前目标窗口句柄（只用于变化时打 debug 日志）。 */
    HoverTargetKind kind = HoverTargetKind::NoTarget; /**< 当前目标的类别（日志用）。 */
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
    bool pendingIdleLog = false;  // 隐藏后要补一行"事件循环回到等待"（Wayland 诊断用）
    std::string selectionPurpose;  // 非空 = 本次选择由 region.select.request 发起，确认后回 region.selected
    DragState drag;               // 当前框选状态
    HoverBox hover;               // 悬停窗口吸附框（目标 + 动画中的显示矩形）
    // 最近一次窗口扫描结果（绝对物理像素，Z 序）：候选窗口 + 任务栏/面板。
    WindowScan windowScan;
    std::chrono::steady_clock::time_point lastWindowScan;  // 上次枚举时刻（30ms 节流）
    bool windowScanValid = false;      // windowScan 是否有有效快照
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

    /**
     * @brief 带超时的隐藏态等待：返回 true = 等到事件/退出，false = 超时。
     *
     * Wayland 下不能无限期阻塞：合成器会给窗口发 ping，需要定期返回事件循环做
     * glfwPollEvents，否则桌面会判定"应用没有响应"。Windows/X11 仍用无超时的 waitForEvent()。
     *
     * @param[in] timeoutMs 超时毫秒数。
     * @return 是否被事件/退出唤醒。
     */
    bool waitForEventFor(int timeoutMs);
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
    /**
     * @brief 判定光标下的悬停吸附目标：窗口 / 任务栏面板 / 屏幕四角。
     *
     * 优先级从高到低：屏幕四角 → 任务栏面板 → 普通窗口。角落方块可能整个压在某个
     * 窗口（尤其最大化窗口）上，用户要的是"这块屏"；任务栏/面板本身在 Z 序最上层，
     * 命中它就不要再去看它下面的窗口。三者统一返回绝对屏幕坐标矩形，调用方再换算成
     * 覆盖层内容坐标——这样"全屏"和其他窗口走同一条绘制/确认路径（不新增事件/ABI）。
     *
     * @param[in] screenX 光标的绝对屏幕横坐标（物理像素）。
     * @param[in] screenY 光标的绝对屏幕纵坐标（物理像素）。
     * @return 命中结果；没有目标时 kind == HoverTargetKind::NoTarget。
     * @note 只读最近一次 windowScan 与 monitorRect（30ms 节流由 updateHover 负责），
     *       不自己枚举窗口，所以可以在同一帧里被重复调用。
     */
    HoverHit resolveHoverHit(int screenX, int screenY) const;
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
    /**
     * @brief 显示覆盖层后确认它真的被合成器 map 了（Wayland 的 map 是异步的）。
     *
     * 每轮先渲染一帧再 poll：Wayland 的 surface 只有在提交过缓冲区之后才会被 map，
     * 光 poll 不渲染会永远等不到 GLFW_VISIBLE。
     *
     * @param[in] maxPolls 最多轮询多少轮（每轮 ~50ms）。
     * @return true = GLFW 已报告窗口可见。
     */
    bool waitOverlayVisible(int maxPolls);
    /**
     * @brief hide 之后用事件泵等"隐藏请求真的送出去"（Wayland 专用）。
     *
     * 每轮 `glfwPollEvents()`（内部 `wl_display_flush`，unmap 请求在这一行才真正发给
     * 合成器）+ `glfwWaitEventsTimeout(kHideWaitRoundSeconds)`，直到
     * `glfwGetWindowAttrib(GLFW_VISIBLE) == 0`（且已泵够 `kHideMinPumpRounds` 轮）或超时。
     * 超时只记 WARN 并返回 false，**绝不阻塞更久**：调用方必须继续往下走，让 UI 线程
     * 回到事件循环。
     *
     * @param[in] timeoutMs 等待上限（毫秒），正常传 `kHideWaitTimeoutMs`。
     * @return true = GLFW 报告已隐藏（并已泵够轮数）；false = 超时。
     */
    bool waitOverlayHidden(int timeoutMs);
    /**
     * @brief 一个"裁剪 + 编码 + 写盘 + 发布 capture.done"的工作线程。
     *
     * 为什么是列表而不是单个 std::thread：join 一个还在编码的线程会把 UI 线程按在
     * 原地几百毫秒到数秒（Wayland 下这段时间不再 dispatch 事件，合成器会认为应用
     * 无响应）。回收因此改成"带超时、只收已完成的"，没跑完的留在这里等下一次或
     * `stop()`；正常情况下列表最多一个元素（同一时刻只有一个任务在写盘）。
     */
    struct CommitWorker {
        std::thread thread;              /**< 工作线程句柄。 */
        std::shared_ptr<bool> finished;  /**< 线程结束标志（读写都由 commitMutex 保护）。 */
    };
    /** @brief 非阻塞回收：最多等 waitMs，只 join 已完成的线程（没跑完的不等）。 */
    void reapCommitWorkers(int waitMs);
    /** @brief stop() 专用：join 所有提交线程（此时可以阻塞，但会记录耗时）。 */
    void joinCommitWorkers();
    /** @brief 确认框选：内存裁剪 → 写 PNG → 发布 capture.done → 收掉覆盖层。 */
    void commitWaylandSelection(int x, int y, int width, int height);

    // portal 截图的整屏像素：既是覆盖层背景，也是确认时的裁剪源（不再第二次采集）。
    std::vector<std::uint8_t> sourcePixels;
    int sourceWidth = 0;
    int sourceHeight = 0;

    // 最近一次 portal 截图的结果：startSelection 据此决定失败日志级别
    // （用户取消保持 debug/静默，其它失败用 error 记录一次）。
    PortalScreenshotStatus lastPortalStatus = PortalScreenshotStatus::Ok;

    /**
     * @brief 提交工作线程的回收列表（见 CommitWorker 的注释）。
     *
     * 只在 UI 线程增删；`stop()` 在 UI 线程 join 之后才碰它。工作线程只通过
     * `commitMutex` 置位自己的 `finished` 标志，不访问列表本身。
     */
    std::vector<CommitWorker> commitWorkers;
    std::mutex commitMutex;            /**< 保护每个 CommitWorker 的 finished 标志。 */
    std::condition_variable commitCv;  /**< 工作线程置位 finished 后唤醒回收方。 */

    // ---- Linux / X11 分支（只由 *_x11.cpp 实现；与上面的 Wayland 成员函数不同名） ----
    /**
     * @brief 是否已经向 WM 写过 `_NET_WM_STATE_FULLSCREEN`（= 请求过全屏）。
     *
     * GNOME（mutter）的 shell 顶栏/dock 画在所有客户端窗口之上，普通窗口甚至
     * override-redirect 窗口都会被压住；但 **WM 管理的全屏窗口会让 mutter 自动隐藏
     * 顶栏和 dock**。所以覆盖层不再用 override-redirect，而是每次显示前把
     * `_NET_WM_STATE` 写成 FULLSCREEN + ABOVE，让 WM 按全屏窗口管理。这个标志只表示
     * 属性写成功（用于日志），WM 是否接受由 x11FullscreenAccepted 记录。
     */
    bool x11FullscreenRequested = false;
    /**
     * @brief 显示后从 `_NET_WM_STATE` 读回 FULLSCREEN：WM 是否真的接受了全屏请求。
     *
     * GNOME 正常会接受；没接受（非 EWMH WM、被别的规则拒绝）时窗口只是一个普通窗口，
     * 会被顶栏/面板盖住——所以 x11ActivateOverlay 会 WARN，并退回
     * `XMoveResizeWindow` 铺满显示器（降级观感，交互仍可用）。
     */
    bool x11FullscreenAccepted = false;
    /** @brief 当前是否持有 XGrabKeyboard（焦点没拿到时的兜底，保证 Esc 可靠）。 */
    bool x11KeyboardGrabbed = false;
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
    /** @brief 把隐藏的覆盖层窗口搬到目标显示器、写 EWMH 全屏属性（X11 客户端可以自己定位）。 */
    void x11PlaceOverlay(const ScreenRect& monitor);
    /** @brief 显示后记录实际窗口几何与 fullscreen 状态：坐标一律按实际位置换算。 */
    void x11LogOverlayGeometry() const;
    /** @brief X11 确认框选：内容坐标 → X screen 坐标，然后按 Windows 同一时序发布。 */
    void x11CommitSelection(int localX, int localY, int localWidth, int localHeight);
    /**
     * @brief 显示前把窗口声明成 EWMH 全屏窗口（必须在 map 之前写好属性）。
     *
     * 用 XInternAtom + XChangeProperty 直接写 32 位 atom 数组（format=32、XA_ATOM）：
     *   · `_NET_WM_WINDOW_TYPE = _NET_WM_WINDOW_TYPE_NORMAL`；
     *   · `_NET_WM_STATE = [_NET_WM_STATE_FULLSCREEN, _NET_WM_STATE_ABOVE]`。
     * 全屏是让 mutter 自动隐藏顶栏/dock 的关键（override-redirect 做不到这一点）。
     * WM 只在窗口 map 时读取初始状态；窗口若已经在显示，先 `XUnmapWindow`、写完再
     * `XMapRaised`，保证 WM 看到的是一次新的 map 请求而不是被忽略的属性变更。
     *
     * @return true = 属性写入成功（**不保证 WM 接受**，接受与否由 x11ActivateOverlay 校验）。
     */
    bool x11PrepareFullscreen();
    /**
     * @brief 显示后接管输入并校验全屏状态。
     *
     * 顺序：泵若干轮 GLFW 事件等 WM 处理 MapRequest → 读 `_NET_WM_STATE` 确认
     * FULLSCREEN（**同时要求有 EWMH WM 且窗口已被 WM 管理**——client 上有 `WM_STATE`
     * 或已被 reparent，否则我们自己写的属性会被误判成"WM 已接受"）；没接受就 WARN +
     * `XMoveResizeWindow` 铺满显示器 →
     * `XRaiseWindow` + `XSetInputFocus` → 焦点确实不在覆盖层时才 `XGrabKeyboard`
     * （owner_events=True，兜底 Esc）；grab 失败只 WARN，全屏窗口本身仍是焦点窗口。
     * 不再抓指针：全屏窗口覆盖整块屏，指针本来就在窗口内。
     */
    void x11ActivateOverlay();
    /**
     * @brief 隐藏前释放键盘 grab（必须在公共的 glfwHideWindow 之前调用）。
     *
     * 全屏状态**不清**：窗口复用，下次显示前 `x11PrepareFullscreen` 会重新写一遍属性。
     * 从未显示过 / 已经释放时直接返回，不刷日志。
     */
    void x11DeactivateOverlay();
    /** @brief WM 没接受全屏时的降级：`XMoveResizeWindow` 直接铺满目标显示器（只 WARN）。 */
    void x11FillMonitorFallback();
#endif
};

}  // namespace region_selector
