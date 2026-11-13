// region_overlay_x11 —— Linux/X11 覆盖层的定位、输入接管与确认提交。
//
// 与 Windows 分支的区别只在"坐标从哪来"：
//   · Windows 用 glfwGetWindowPos + 虚拟桌面原点（SM_XVIRTUALSCREEN）换算；
//   · X11 只有一块 X screen，root 原点就是 (0,0)，窗口内容坐标 + 窗口在 root 里的
//     位置就是最终 region。X 像素即物理像素，content scale 不参与坐标（只放大字号）。
//   提交之后的流程完全一致：普通框选发布带 region 的 capture.request，由宿主流水线
//   裁剪落盘；region.select.request 发起的选择只回 region.selected（purpose 透传）。
//
// 为什么覆盖层必须 override-redirect（真机需求）：
//   EWMH 的 DOCK 面板（GNOME 的 dash-to-dock / 顶栏、xfce4-panel 等）在 ABOVE 层，
//   WM 管理的普通窗口即使带 `_NET_WM_STATE_ABOVE` 也可能被压在面板下面；面板还会
//   抢焦点、抢悬停/点击输入——现象就是"鼠标移到任务栏，覆盖层收不到事件，悬停全屏
//   失效"。所以窗口在第一次 map 之前就切成 override-redirect，WM 从此完全不管理它，
//   它永远在所有 WM 管理的窗口（含面板）之上。代价是位置/尺寸/焦点/grab 都要自己做：
//     · 显示：XRaiseWindow + XSetInputFocus + XGrabKeyboard/XGrabPointer（owner_events=True，
//       grab_window = 覆盖层）；隐藏：先 XUnmapWindow 再 ungrab；
//     · GLFW 的事件处理走同一个 X 连接和窗口：grab 下 KeyPress / ButtonPress /
//       MotionNotify 仍投递到覆盖层，所以 region_overlay.cpp 的轮询（Esc、拖框、悬停）
//       一行都不用改；
//     · grab 被别的客户端占着（托盘/GTK 菜单收起前会持有 grab）时短重试，仍失败就
//       WARN 并降级回 WM 管理的普通窗口，行为退回到本方案之前的样子。
//
// 提交时序与 Windows 相同，不能省：先隐藏（unmap + ungrab）再 glfwPollEvents，再等
// kHideSettleDelayMs。X11 合成器（尤其开 compositing 的桌面）同样是异步合成的，
// 隐藏函数返回不代表覆盖层已经从画面上消失；宿主 worker 收到事件就抓屏，中间没有
// 同步点，等太短会把遮罩截进图里。
//
// 为什么不做 Windows 的 +1 像素：那是 Windows 把"矩形刚好等于显示器"的窗口判成
// 全屏窗口（fullscreen optimization）的专有坑，X11 没有这条规则；铺满即精确覆盖。

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <X11/Xlib.h>

// 自己包含 Xlib 头；glfw3native.h 会连带声明 RRCrtc/RROutput 并拉 <X11/extensions/Xrandr.h>，
// 所以编译期需要 libxrandr-dev（GLFW 运行时本来也用它枚举显示器）。
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

/** @brief grab 重试次数：托盘/菜单收起前可能还持有键盘或指针 grab（AlreadyGrabbed）。 */
inline constexpr int kX11GrabAttempts = 5;

/** @brief 两次 grab 重试之间的等待（毫秒）：菜单收起通常就在几十毫秒内。 */
inline constexpr int kX11GrabRetryDelayMs = 20;

/** @brief 最近一次 X 错误记录（只在 UI 线程的 X11ErrorScope 作用域内读写）。 */
int g_x11ErrorCount = 0;
int g_x11LastErrorCode = 0;

/**
 * @brief Xlib 错误处理器：只记录、不退出。
 *
 * Xlib 的默认错误处理器会打印错误后 exit(1)。override-redirect / 焦点 / grab 这些
 * 请求可能与 WM 或别的客户端抢时序，异步报 BadMatch/BadWindow 时进程绝不能因此退出
 * （需求：失败 WARN 并降级，不能崩）。这里只统计错误码，由 X11ErrorScope 判读。
 */
int recordX11Error(Display* /*display*/, XErrorEvent* event) {
    ++g_x11ErrorCount;
    g_x11LastErrorCode = (event != nullptr) ? static_cast<int>(event->error_code) : 0;
    return 0;
}

/**
 * @brief 一段 X 请求的错误作用域：析构时恢复原处理器。
 *
 * 只在 UI 线程、很短的一段请求里临时替换全局 X 错误处理器（Xlib 没有按连接注册的
 * 接口）；flush() 用 XSync 把请求同步到服务器，异步错误就会在作用域内被捕获。
 * 同一作用域里只放本插件自己的请求，不调用 GLFW（避免和 GLFW 的错误处理器作用域叠加）。
 */
class X11ErrorScope {
public:
    /** @brief 安装记录器并清空计数。 */
    explicit X11ErrorScope(Display* display)
        : display_(display), previous_(XSetErrorHandler(&recordX11Error)) {
        g_x11ErrorCount = 0;
        g_x11LastErrorCode = 0;
    }
    /** @brief 恢复原处理器（XSetErrorHandler 是进程级状态）。 */
    ~X11ErrorScope() {
        XSetErrorHandler(previous_);
    }
    X11ErrorScope(const X11ErrorScope&) = delete;
    X11ErrorScope& operator=(const X11ErrorScope&) = delete;

    /** @brief 同步到服务器；返回 true = 这段请求没有产生 X 错误。 */
    bool flush() {
        if (display_ == nullptr) {
            return false;
        }
        XSync(display_, False);
        return g_x11ErrorCount == 0;
    }
    /** @brief 最近一个 X 错误码（flush() 之后读；0 = 没有错误）。 */
    int errorCode() const {
        return g_x11LastErrorCode;
    }

private:
    Display* display_;
    XErrorHandler previous_;
};

/** @brief XGrab* 的返回码转成日志可读的名字（失败时一眼看出是被谁占着）。 */
const char* grabStatusName(int status) {
    switch (status) {
        case GrabSuccess:
            return "GrabSuccess";
        case AlreadyGrabbed:
            return "AlreadyGrabbed";
        case GrabInvalidTime:
            return "GrabInvalidTime";
        case GrabNotViewable:
            return "GrabNotViewable";
        case GrabFrozen:
            return "GrabFrozen";
        default:
            return "Unknown";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// override-redirect 与输入接管
// ---------------------------------------------------------------------------
bool RegionWindow::Impl::x11PrepareOverrideRedirect() {
    const shot::Logger log(host);
    Display* display = glfwGetX11Display();
    const Window xid = (window != nullptr) ? glfwGetX11Window(window) : 0;
    if (display == nullptr || xid == 0) {
        log.warn("region_selector：X11 Display/窗口不可用，无法设置 override-redirect"
                 "（按 WM 管理的普通窗口继续）");
        x11OverrideRedirect = false;
        return false;
    }
    // 必须在窗口第一次 map 之前设置：GLFW 的隐藏窗口此刻还没 map，这里再 unmap 一次是
    // 防御性操作（无论建窗路径是否 map 过，属性都会在可见之前生效）。OR 窗口的 map
    // 请求 WM 根本不会看到，所以不需要抢在 WM reparent 之前，只需要抢在 map 之前。
    XUnmapWindow(display, xid);
    bool applied = false;
    int errorCode = 0;
    {
        X11ErrorScope scope(display);
        XSetWindowAttributes attributes{};
        attributes.override_redirect = True;
        XChangeWindowAttributes(display, xid, CWOverrideRedirect, &attributes);
        applied = scope.flush();
        errorCode = scope.errorCode();
    }
    x11OverrideRedirect = applied;
    if (!applied) {
        char text[256] = {};
        std::snprintf(text, sizeof(text),
                      "region_selector：X11 override-redirect 设置失败（X error=%d），"
                      "降级为 WM 管理的普通窗口：面板可能盖住覆盖层",
                      errorCode);
        log.warn(text);
    } else {
        log.info("region_selector：X11 覆盖层已设为 override-redirect（不受 WM 管理，显示时置顶）");
    }
    return applied;
}

bool RegionWindow::Impl::x11ShowOverlayInput() {
    const shot::Logger log(host);
    Display* display = glfwGetX11Display();
    const Window xid = (window != nullptr) ? glfwGetX11Window(window) : 0;
    if (display == nullptr || xid == 0) {
        log.warn("region_selector：X11 Display/窗口不可用，无法接管输入（按普通窗口继续）");
        return false;
    }
    if (!x11OverrideRedirect) {
        // 没有 override-redirect 就别 grab：面板在视觉上盖住覆盖层，抢输入只会让用户
        // 点了看得见的面板却打开选区。保持旧行为（WM 管理 + 普通焦点/指针事件）。
        log.info("region_selector：X11 覆盖层未启用 override-redirect，输入仍按 WM 管理的"
                 "普通窗口处理");
        return false;
    }
    // 焦点与 grab 都要求窗口已经可见：否则 XSetInputFocus 会异步报 BadMatch，grab 也会
    // 返回 GrabNotViewable。glfwShowWindow 已经等过 VisibilityNotify，这里再查一次，
    // 把"还没 map"变成可降级的失败（而不是让 Xlib 默认处理器 exit）。
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, xid, &attributes) == 0 ||
        attributes.map_state != IsViewable) {
        log.warn("region_selector：X11 覆盖层还没有 map，跳过输入 grab（降级为普通窗口）");
        x11FallbackToManagedWindow();
        return false;
    }

    // 键盘：owner_events=True，事件先按焦点窗口投递，焦点不在我们这时仍会落到 grab 窗口；
    // 指针：owner_events=True + 三种事件掩码，鼠标移到窗口外（另一块显示器）也能收到
    // MotionNotify。托盘/GTK 菜单收起前可能还抓着键盘/指针，AlreadyGrabbed 时短重试。
    int keyboardStatus = GrabSuccess;
    int pointerStatus = GrabSuccess;
    bool grabbed = false;
    for (int attempt = 0; attempt < kX11GrabAttempts && !grabbed; ++attempt) {
        keyboardStatus =
            XGrabKeyboard(display, xid, True, GrabModeAsync, GrabModeAsync, CurrentTime);
        if (keyboardStatus == GrabSuccess) {
            pointerStatus =
                XGrabPointer(display, xid, True,
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            grabbed = (pointerStatus == GrabSuccess);
            if (!grabbed) {
                XUngrabKeyboard(display, CurrentTime);
            }
        }
        if (!grabbed && attempt + 1 < kX11GrabAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kX11GrabRetryDelayMs));
        }
    }
    x11KeyboardGrabbed = grabbed;
    x11PointerGrabbed = grabbed;
    if (!grabbed) {
        XSync(display, False);
        // 键盘 grab 都没成功时指针 grab 根本没尝试过，别在日志里写成 GrabSuccess。
        const char* pointerText =
            (keyboardStatus == GrabSuccess) ? grabStatusName(pointerStatus) : "NotAttempted";
        char text[320] = {};
        std::snprintf(text, sizeof(text),
                      "region_selector：X11 输入 grab 失败（keyboard=%s pointer=%s），"
                      "降级为 WM 管理的普通窗口：面板可能盖住覆盖层",
                      grabStatusName(keyboardStatus), pointerText);
        log.warn(text);
        x11FallbackToManagedWindow();
        return false;
    }

    // 焦点：grab 生效后把输入焦点也钉在覆盖层上（RevertToPointerRoot 让隐藏后焦点回到
    // 指针，而不是留给一个已经不存在的窗口）。窗口 viewable 已确认，正常不会 BadMatch；
    // 仍然用错误作用域接住，失败只 WARN 不撤销 grab（owner_events=True 下 Esc 仍可用）。
    bool focusApplied = false;
    int focusError = 0;
    {
        X11ErrorScope scope(display);
        XRaiseWindow(display, xid);
        XSetInputFocus(display, xid, RevertToPointerRoot, CurrentTime);
        focusApplied = scope.flush();
        focusError = scope.errorCode();
    }
    if (!focusApplied) {
        char text[224] = {};
        std::snprintf(text, sizeof(text),
                      "region_selector：X11 XSetInputFocus 失败（X error=%d），"
                      "键盘事件改由 grab 投递（Esc 仍可用）",
                      focusError);
        log.warn(text);
    }

    char text[320] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：X11 覆盖层已显示并接管输入 override_redirect=%d "
                  "grab_keyboard=%d grab_pointer=%d focus=%d 显示器=(%d,%d) %dx%d",
                  x11OverrideRedirect ? 1 : 0, x11KeyboardGrabbed ? 1 : 0,
                  x11PointerGrabbed ? 1 : 0, focusApplied ? 1 : 0, monitorRect.x, monitorRect.y,
                  monitorRect.width, monitorRect.height);
    log.info(text);
    return true;
}

void RegionWindow::Impl::x11HideOverlayInput() {
    if (window == nullptr) {
        return;
    }
    if (!overlayVisible && !x11KeyboardGrabbed && !x11PointerGrabbed) {
        return;  // 从未真正显示 / 已经收过：不重复 unmap、不刷日志
    }
    Display* display = glfwGetX11Display();
    const Window xid = glfwGetX11Window(window);
    if (display == nullptr || xid == 0) {
        return;
    }
    const bool hadKeyboard = x11KeyboardGrabbed;
    const bool hadPointer = x11PointerGrabbed;
    // 顺序：先 unmap（覆盖层立刻从画面消失，窗口不可见时 X 也会自动释放 grab），再显式
    // ungrab，保证下一次显示不会带着上一次的 grab/焦点。之后公共 hideOverlay() 还会再走
    // 一次 glfwHideWindow + glfwPollEvents（对已 unmap 的窗口是空操作）。
    XUnmapWindow(display, xid);
    if (hadKeyboard) {
        XUngrabKeyboard(display, CurrentTime);
    }
    if (hadPointer) {
        XUngrabPointer(display, CurrentTime);
    }
    x11KeyboardGrabbed = false;
    x11PointerGrabbed = false;
    XSync(display, False);
    char text[256] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：X11 覆盖层隐藏 override_redirect=%d grab_keyboard=%d "
                  "grab_pointer=%d（已 unmap 并解除输入 grab）",
                  x11OverrideRedirect ? 1 : 0, hadKeyboard ? 1 : 0, hadPointer ? 1 : 0);
    shot::Logger(host).info(text);
}

void RegionWindow::Impl::x11FallbackToManagedWindow() {
    // 先释放可能已经拿到一半的 grab：降级后事件走普通窗口（WM 管理 + 焦点）。
    if (x11KeyboardGrabbed || x11PointerGrabbed) {
        Display* display = glfwGetX11Display();
        if (display != nullptr) {
            if (x11KeyboardGrabbed) {
                XUngrabKeyboard(display, CurrentTime);
            }
            if (x11PointerGrabbed) {
                XUngrabPointer(display, CurrentTime);
            }
            XSync(display, False);
        }
        x11KeyboardGrabbed = false;
        x11PointerGrabbed = false;
    }
    if (!x11OverrideRedirect || window == nullptr) {
        return;
    }
    Display* display = glfwGetX11Display();
    const Window xid = glfwGetX11Window(window);
    if (display == nullptr || xid == 0) {
        return;
    }
    const shot::Logger log(host);
    // 真正退回 WM 管理：先 unmap、清 override_redirect、再 map，WM 才会重新接管这个
    // 窗口（窗口已经 map 时只改属性，WM 不会回头管它）。之后用 glfwFocusWindow 发
    // _NET_ACTIVE_WINDOW 让 WM 按自己的焦点策略处理，而不是我们 XSetInputFocus 偷焦点。
    bool reverted = false;
    int errorCode = 0;
    {
        X11ErrorScope scope(display);
        XUnmapWindow(display, xid);
        XSetWindowAttributes attributes{};
        attributes.override_redirect = False;
        XChangeWindowAttributes(display, xid, CWOverrideRedirect, &attributes);
        XMapRaised(display, xid);
        reverted = scope.flush();
        errorCode = scope.errorCode();
    }
    if (!reverted) {
        // 属性没改成功：窗口已经 unmap，必须重新 map 回来，别把覆盖层留在不可见状态。
        x11OverrideRedirect = true;
        XMapRaised(display, xid);
        XSync(display, False);
        char text[224] = {};
        std::snprintf(text, sizeof(text),
                      "region_selector：X11 降级为普通窗口失败（X error=%d），"
                      "保持 override-redirect",
                      errorCode);
        log.warn(text);
        return;
    }
    x11OverrideRedirect = false;
    log.warn("region_selector：X11 已降级为 WM 管理的普通窗口（override-redirect=0）："
             "面板/任务栏可能盖住覆盖层，交互仍可继续");
    glfwFocusWindow(window);
}

// ---------------------------------------------------------------------------
// 定位 / 实际几何日志 / 确认提交
// ---------------------------------------------------------------------------
void RegionWindow::Impl::x11PlaceOverlay(const ScreenRect& monitor) {
    if (window == nullptr || monitor.width <= 0 || monitor.height <= 0) {
        return;
    }
    // 位置/尺寸都是 X screen 坐标（= 像素）。窗口是隐藏状态，这里只改属性；真正
    // 显示在 startSelection 的"先渲染两帧再 show"之后。
    glfwSetWindowSize(window, monitor.width, monitor.height);
    glfwSetWindowPos(window, monitor.x, monitor.y);
    if (!x11OverrideRedirect) {
        // 普通窗口：最终摆放由 WM 决定，显示后按实际几何换算（x11LogOverlayGeometry）。
        return;
    }
    Display* display = glfwGetX11Display();
    const Window xid = glfwGetX11Window(window);
    if (display == nullptr || xid == 0) {
        return;
    }
    // override-redirect 窗口不归 WM 管：GLFW 的 setSize/setPos 也是直接发 X 请求，这里
    // 再显式 XMoveResizeWindow 一次，明确"显示器矩形就是最终几何"（X11 不需要 Windows
    // 的 +1px，铺满即精确覆盖）。隐藏状态下设置，map 时一次到位，没有可见的位移。
    XMoveResizeWindow(display, xid, monitor.x, monitor.y,
                      static_cast<unsigned int>(monitor.width),
                      static_cast<unsigned int>(monitor.height));
    XSync(display, False);
}

void RegionWindow::Impl::x11LogOverlayGeometry() const {
    if (window == nullptr) {
        return;
    }
    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    char text[384] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：X11 覆盖层窗口 pos=(%d,%d) size=%dx%d 目标显示器=(%d,%d) "
                  "%dx%d override_redirect=%d（%s）",
                  windowX, windowY, windowWidth, windowHeight, monitorRect.x, monitorRect.y,
                  monitorRect.width, monitorRect.height, x11OverrideRedirect ? 1 : 0,
                  x11OverrideRedirect ? "不受 WM 管理，几何即最终值"
                                      : "WM 可能再次调整位置，坐标一律按实际窗口位置换算");
    shot::Logger(host).info(text);
}

void RegionWindow::Impl::x11CommitSelection(int localX, int localY, int localWidth,
                                            int localHeight) {
    const shot::Logger log(host);
    // hideOverlay() 会清空 selectionPurpose，先拷出来决定发 capture.request 还是
    // region.selected。普通框选（空串）= 交给宿主流水线；非空 = 选择握手。
    const std::string purpose = selectionPurpose;
    int windowX = 0;
    int windowY = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    ScreenRect region;
    region.x = windowX + localX;
    region.y = windowY + localY;
    region.width = localWidth;
    region.height = localHeight;
    char text[256] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：X11 确认选区 region={x:%d,y:%d,width:%d,height:%d}"
                  "（X screen 坐标 = 物理像素）",
                  region.x, region.y, region.width, region.height);
    log.info(text);

    // ---- 关键时序：先隐藏覆盖层（unmap + ungrab），等它真的从画面上消失，再发布 ----
    hideOverlay();
    glfwPollEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(kHideSettleDelayMs));
    // 等待期间宿主可能已经开始卸载本插件：不要在 deinit 之后再发事件。
    if (shouldQuit()) {
        log.info("region_selector：宿主正在退出，丢弃本次已确认的区域");
        return;
    }
    if (!purpose.empty()) {
        // 选择握手（长截图目标选择等）：只回 region.selected，不发 capture.request，
        // 由发起方自己采集。
        publishPurposeSelection(purpose, region);
    } else {
        publishRegion(region);
    }
}

}  // namespace region_selector
