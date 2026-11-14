// region_overlay_x11 —— Linux/X11 覆盖层的定位、EWMH 全屏、输入接管与确认提交。
//
// 与 Windows 分支的区别只在"坐标从哪来"：
//   · Windows 用 glfwGetWindowPos + 虚拟桌面原点（SM_XVIRTUALSCREEN）换算；
//   · X11 只有一块 X screen，root 原点就是 (0,0)，窗口内容坐标 + 窗口在 root 里的
//     位置就是最终 region。X 像素即物理像素，content scale 不参与坐标（只放大字号）。
//   提交之后的流程完全一致：普通框选发布带 region 的 capture.request，由宿主流水线
//   裁剪落盘；region.select.request 发起的选择只回 region.selected（purpose 透传）。
//
// 为什么覆盖层必须是 "WM 管理的 EWMH 全屏窗口"（真机结论，别再改回 override-redirect）：
//   GNOME（mutter）的 shell 顶栏与 dock 画在**所有客户端窗口之上**，连 override-redirect
//   窗口也压不住，面板还会抢焦点、抢悬停/点击；上一轮"override-redirect +
//   XGrabPointer/XGrabKeyboard"在真机上依然被面板压住（grab 改不了绘制层级）。
//   EWMH 的 `_NET_WM_STATE_FULLSCREEN` 走的是另一条路：mutter 识别到全屏窗口后会
//   **自动隐藏顶栏和 dock**，整块显示器才真正属于覆盖层，鼠标移到最后一像素也不会被
//   面板抢走。代价是窗口重新交给 WM 管理：位置/尺寸/焦点都由 WM 决定，所以：
//     · map 之前用 XChangeProperty 直接写 `_NET_WM_WINDOW_TYPE=NORMAL`、
//       `_NET_WM_STATE=[FULLSCREEN,ABOVE]`（EWMH 的初始状态只在 map 时读取）；
//     · 显示后读回 `_NET_WM_STATE` 校验 WM 是否接受；没接受就 WARN 并退回
//       XMoveResizeWindow 铺满显示器（面板会盖住，但交互仍可用）；
//     · XRaiseWindow + XSetInputFocus 拿焦点；焦点确实不在覆盖层时才 XGrabKeyboard
//       兜底（Esc 可靠），**不再抓指针**——全屏窗口覆盖整块屏，指针本来就在窗口内。
//
// 提交时序与 Windows 相同，不能省：先隐藏（释放 grab + glfwHideWindow）再
// glfwPollEvents，再等 kHideSettleDelayMs。X11 合成器（尤其开 compositing 的桌面）
// 同样是异步合成的，隐藏函数返回不代表覆盖层已经从画面上消失；宿主 worker 收到事件
// 就抓屏，中间没有同步点，等太短会把遮罩截进图里。
//
// 为什么不做 Windows 的 +1 像素：那是 Windows 把"矩形刚好等于显示器"的窗口判成
// 全屏窗口（fullscreen optimization）的专有坑，X11 没有这条规则；全屏窗口由 WM
// 铺满显示器，铺满即精确覆盖。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

// 自己包含 Xlib 头；glfw3native.h 会连带声明 RRCrtc/RROutput 并拉 <X11/extensions/Xrandr.h>，
// 所以编译期需要 libxrandr-dev（GLFW 运行时本来也用它枚举显示器）。
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

/** @brief 等 WM 接受全屏状态的最多轮数：每轮 ≤ kX11FullscreenPollSeconds + 一次事件泵。 */
inline constexpr int kX11FullscreenPollRounds = 8;

/**
 * @brief 每轮等 WM 处理事件的超时（秒）。
 *
 * glfwShowWindow 已经等到 VisibilityNotify（窗口被 map），全屏状态通常在 map 时就被
 * mutter 应用；正常第一轮就能读到 FULLSCREEN。这里留 8 × 20ms = 160ms 上限，是给
 * 慢一点的 WM/compositor 一次往返，而不是把 UI 线程按在这里。
 */
inline constexpr double kX11FullscreenPollSeconds = 0.02;

/** @brief grab 重试次数：焦点没拿到时用键盘 grab 兜底，别的客户端可能临时占着。 */
inline constexpr int kX11GrabAttempts = 5;

/** @brief 两次 grab 重试之间的等待（毫秒）：托盘/菜单收起通常就在几十毫秒内。 */
inline constexpr int kX11GrabRetryDelayMs = 20;

/** @brief 最近一次 X 错误记录（只在 UI 线程的 X11ErrorScope 作用域内读写）。 */
int g_x11ErrorCount = 0;
int g_x11LastErrorCode = 0;

/**
 * @brief Xlib 错误处理器：只记录、不退出。
 *
 * Xlib 的默认错误处理器会打印错误后 exit(1)。全屏属性/焦点/grab 这些请求可能与 WM
 * 或别的客户端抢时序，异步报 BadMatch/BadWindow 时进程绝不能因此退出（需求：失败
 * WARN 并降级，不能崩）。这里只统计错误码，由 X11ErrorScope 判读。
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

/** @brief XGrabKeyboard 的返回码转成日志可读的名字（失败时一眼看出是被谁占着）。 */
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

/**
 * @brief 读取窗口的 atom 列表属性（`_NET_WM_STATE` 等）。
 *
 * XGetWindowProperty 的 length 单位是 32 位，最多读 capacity 个 atom；属性不存在、
 * 类型不是 XA_ATOM、WM 还没写过时返回 0（调用方按"没有状态"处理）。
 *
 * @param[in]  display   X11 连接。
 * @param[in]  xid       窗口。
 * @param[in]  property  属性 atom。
 * @param[out] out       atom 输出数组。
 * @param[in]  capacity  out 最多能放多少个 atom。
 * @return 实际读到的 atom 个数（0 = 没有属性 / 读失败）。
 */
int readAtomList(Display* display, Window xid, Atom property, Atom* out, int capacity) {
    if (display == nullptr || xid == 0 || property == None || out == nullptr || capacity <= 0) {
        return 0;
    }
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(display, xid, property, 0, capacity, False, XA_ATOM,
                                          &actualType, &actualFormat, &itemCount, &bytesAfter,
                                          &data);
    int count = 0;
    if (status == Success && actualType == XA_ATOM && actualFormat == 32 && data != nullptr) {
        count = static_cast<int>(std::min<unsigned long>(
            itemCount, static_cast<unsigned long>(capacity)));
        const auto* atoms = reinterpret_cast<const Atom*>(data);
        for (int index = 0; index < count; ++index) {
            out[index] = atoms[index];
        }
    }
    if (data != nullptr) {
        XFree(data);
    }
    return count;
}

/** @brief atom 列表里是否含指定 atom。 */
bool atomListContains(const Atom* atoms, int count, Atom wanted) {
    if (atoms == nullptr || wanted == None) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        if (atoms[index] == wanted) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 把窗口的 `_NET_WM_STATE` 概括成日志文本（只认 FULLSCREEN / ABOVE）。
 *
 * WM 没接受全屏时，它是唯一能说明"WM 到底写了什么"的证据；正常路径也会把
 * accepted 写进日志，出问题不用再挂 X 探针。
 */
std::string describeWmState(Display* display, Window xid, Atom wmState, Atom fullscreenAtom,
                            Atom aboveAtom) {
    Atom atoms[32] = {};
    const int count = readAtomList(display, xid, wmState, atoms, 32);
    if (count <= 0) {
        return "(没有 _NET_WM_STATE)";
    }
    std::string text;
    for (int index = 0; index < count; ++index) {
        const char* name = "other";
        if (atoms[index] == fullscreenAtom) {
            name = "FULLSCREEN";
        } else if (atoms[index] == aboveAtom) {
            name = "ABOVE";
        }
        if (!text.empty()) {
            text += ",";
        }
        text += name;
    }
    return "[" + text + "]";
}

/** @brief 窗口上是否存在某个属性（值不看，只看属性在不在）。 */
bool hasProperty(Display* display, Window xid, Atom property) {
    if (display == nullptr || xid == 0 || property == None) {
        return false;
    }
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(display, xid, property, 0, 0, False, AnyPropertyType,
                                          &actualType, &actualFormat, &itemCount, &bytesAfter,
                                          &data);
    if (data != nullptr) {
        XFree(data);
    }
    return status == Success && actualType != None;
}

/**
 * @brief 是否有一个 EWMH 兼容的窗口管理器在跑（root 上的 `_NET_SUPPORTING_WM_CHECK`）。
 *
 * 只读回 `_NET_WM_STATE` **不能证明** WM 接受了全屏：属性是我们在 map 之前自己写的，
 * 没有 WM 或 WM 不认 EWMH 时它会原样留着。所以"接受"必须同时满足：有 EWMH WM、
 * 窗口已经被 WM 管理（client 上有 `WM_STATE`，或已被 reparent 进 frame）、读回的
 * 状态含 FULLSCREEN。
 */
bool hasEwmhWindowManager(Display* display) {
    if (display == nullptr) {
        return false;
    }
    const Atom check = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", True);
    if (check == None) {
        return false;
    }
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(display, DefaultRootWindow(display), check, 0, 1, False,
                                          XA_WINDOW, &actualType, &actualFormat, &itemCount,
                                          &bytesAfter, &data);
    bool found = false;
    if (status == Success && actualType == XA_WINDOW && actualFormat == 32 && data != nullptr &&
        itemCount >= 1) {
        const Window wmWindow = *reinterpret_cast<const Window*>(data);
        found = (wmWindow != 0);
    }
    if (data != nullptr) {
        XFree(data);
    }
    return found;
}

/**
 * @brief 窗口是否已被 reparenting WM 收进 frame（parent 不是 root）。
 *
 * 作为 `WM_STATE` 的补充判据：mutter 一定会在 client 上写 `WM_STATE`，但读取发生在
 * 显示后的几轮泵事件里，用 reparent 状态兜底可以避免"WM 已经接管、只是属性还没
 * 落到 client 上"这种假阴性。
 */
bool isReparented(Display* display, Window xid) {
    if (display == nullptr || xid == 0) {
        return false;
    }
    Window rootReturn = 0;
    Window parentReturn = 0;
    Window* children = nullptr;
    unsigned int childCount = 0;
    if (XQueryTree(display, xid, &rootReturn, &parentReturn, &children, &childCount) == 0) {
        return false;
    }
    if (children != nullptr) {
        XFree(children);
    }
    return parentReturn != 0 && parentReturn != rootReturn;
}

/**
 * @brief 当前输入焦点是否正好落在覆盖层的 client 窗口上。
 *
 * 只认 client 自己：焦点在 WM 的 frame 上时，键盘事件给的是 frame，GLFW 的窗口收不到
 * （Esc 失效）。这种情况按"没拿到焦点"处理，外层会 XGrabKeyboard 兜底。
 */
bool focusIsOverlay(Display* display, Window xid) {
    if (display == nullptr || xid == 0) {
        return false;
    }
    Window focused = 0;
    int revertTo = 0;
    XGetInputFocus(display, &focused, &revertTo);
    return focused == xid;
}

}  // namespace

// ---------------------------------------------------------------------------
// EWMH 全屏：显示前写属性，显示后校验，失败降级铺满
// ---------------------------------------------------------------------------
bool RegionWindow::Impl::x11PrepareFullscreen() {
    const shot::Logger log(host);
    Display* display = glfwGetX11Display();
    const Window xid = (window != nullptr) ? glfwGetX11Window(window) : 0;
    if (display == nullptr || xid == 0) {
        log.warn("region_selector：X11 Display/窗口不可用，无法设置 EWMH 全屏"
                 "（按普通窗口继续，面板可能盖住覆盖层）");
        x11FullscreenRequested = false;
        return false;
    }

    // EWMH 的初始状态只在 map 时读一次。正常路径窗口是隐藏的（startSelection 从隐藏态
    // 开始），这里再查一次 map 状态是防御：如果窗口已经在显示，必须先 unmap、等 WM
    // 处理完 unmap（XSync），写属性后再 map，WM 才会把它当成一次新的 map 请求。
    XWindowAttributes attributes{};
    const bool wasViewable = (XGetWindowAttributes(display, xid, &attributes) != 0 &&
                              attributes.map_state == IsViewable);
    if (wasViewable) {
        log.warn("region_selector：X11 覆盖层已处于显示状态，先 unmap 再重新提交全屏请求");
        XUnmapWindow(display, xid);
        XSync(display, False);
    }

    const Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom wmWindowType = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    const Atom fullscreenAtom = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    const Atom aboveAtom = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    const Atom normalType = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    if (wmState == None || wmWindowType == None || fullscreenAtom == None ||
        normalType == None) {
        log.warn("region_selector：X11 无法 intern EWMH atom（_NET_WM_STATE / "
                 "_NET_WM_WINDOW_TYPE），无法请求全屏：按普通窗口继续，面板可能盖住覆盖层");
        x11FullscreenRequested = false;
        if (wasViewable) {
            XMapRaised(display, xid);
            XSync(display, False);
        }
        return false;
    }

    bool applied = false;
    int errorCode = 0;
    {
        X11ErrorScope scope(display);
        // 窗口类型：普通窗口。mutter 对 NORMAL 才会套用全屏/焦点策略；不写也能工作，
        // 但显式写出来避免 GLFW/别的插件把类型留成别的值。
        Atom windowType = normalType;
        XChangeProperty(display, xid, wmWindowType, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&windowType), 1);
        // FULLSCREEN 是让 mutter 隐藏顶栏/dock 的关键；ABOVE 让没接受全屏的 WM 上
        // 也尽量浮在普通窗口之上。
        Atom states[2] = {fullscreenAtom, aboveAtom};
        XChangeProperty(display, xid, wmState, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(states), 2);
        applied = scope.flush();
        errorCode = scope.errorCode();
    }
    x11FullscreenRequested = applied;
    x11FullscreenAccepted = false;
    if (!applied) {
        char text[256] = {};
        std::snprintf(text, sizeof(text),
                      "region_selector：X11 EWMH 全屏属性写入失败（X error=%d），"
                      "按普通窗口继续：面板可能盖住覆盖层",
                      errorCode);
        log.warn(text);
    } else {
        log.info("region_selector：X11 已写入 EWMH 属性 _NET_WM_WINDOW_TYPE=NORMAL "
                 "_NET_WM_STATE=[FULLSCREEN,ABOVE]（等待 WM 接受）");
    }
    if (wasViewable) {
        // 重新 map：WM 只在这时读取初始状态（正常路径不会走到这里）。
        XMapRaised(display, xid);
        XSync(display, False);
    }
    return applied;
}

void RegionWindow::Impl::x11ActivateOverlay() {
    const shot::Logger log(host);
    Display* display = glfwGetX11Display();
    const Window xid = (window != nullptr) ? glfwGetX11Window(window) : 0;
    if (display == nullptr || xid == 0) {
        log.warn("region_selector：X11 Display/窗口不可用，无法激活覆盖层");
        return;
    }

    // glfwShowWindow 会等 VisibilityNotify（最多 100ms）：走到这里通常已经 viewable。
    // 如果不是，继续尝试焦点/raise 并留下日志，但这次框选基本不会成功（见末尾 WARN）。
    XWindowAttributes attributes{};
    bool viewable = (XGetWindowAttributes(display, xid, &attributes) != 0 &&
                     attributes.map_state == IsViewable);
    if (!viewable) {
        log.warn("region_selector：X11 覆盖层 map 后仍不是 IsViewable（glfwShowWindow 超时？），"
                 "继续尝试 raise/焦点");
    }

    const Atom wmState = XInternAtom(display, "_NET_WM_STATE", True);
    const Atom fullscreenAtom = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", True);
    const Atom aboveAtom = XInternAtom(display, "_NET_WM_STATE_ABOVE", True);
    const Atom icccmWmState = XInternAtom(display, "WM_STATE", True);
    const bool ewmhWm = hasEwmhWindowManager(display);

    // 等 WM 处理 MapRequest。**不能**只看 `_NET_WM_STATE` 里有没有 FULLSCREEN：那是我们
    // 自己在 map 前写的，没有 WM 时会原样留着。接受 = 有 EWMH WM + 窗口已被 WM 管理
    // （client 上有 ICCCM 的 `WM_STATE`，或被 reparent 进 frame）+ 读回状态含
    // FULLSCREEN，三者缺一不可。
    const auto startedAt = std::chrono::steady_clock::now();
    bool accepted = false;
    bool wmManaged = false;
    int rounds = 0;
    for (; rounds < kX11FullscreenPollRounds; ++rounds) {
        bool fullscreenFlag = false;
        if (wmState != None && fullscreenAtom != None) {
            Atom atoms[16] = {};
            const int count = readAtomList(display, xid, wmState, atoms, 16);
            fullscreenFlag = atomListContains(atoms, count, fullscreenAtom);
        }
        wmManaged = hasProperty(display, xid, icccmWmState) || isReparented(display, xid);
        accepted = fullscreenFlag && wmManaged && ewmhWm;
        if (accepted) {
            break;
        }
        glfwWaitEventsTimeout(kX11FullscreenPollSeconds);
    }
    x11FullscreenAccepted = accepted;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - startedAt)
                               .count();
    if (accepted) {
        char text[240] = {};
        std::snprintf(text, sizeof(text),
                      "region_selector：X11 WM 已接受全屏请求（_NET_WM_STATE 含 FULLSCREEN，"
                      "EWMH WM=1 已管理=1，检查 %d 轮 / %lld ms）",
                      rounds, static_cast<long long>(elapsedMs));
        log.info(text);
    } else {
        const std::string stateText = describeWmState(display, xid, wmState, fullscreenAtom,
                                                      aboveAtom);
        char text[512] = {};
        std::snprintf(text, sizeof(text),
                      "region_selector：X11 WM 未接受 _NET_WM_STATE_FULLSCREEN"
                      "（_NET_WM_STATE=%s，EWMH WM=%d，窗口已被 WM 管理=%d，"
                      "检查 %d 轮 / %lld ms），退回 XMoveResizeWindow 铺满显示器："
                      "面板可能盖住覆盖层",
                      stateText.c_str(), ewmhWm ? 1 : 0, wmManaged ? 1 : 0, rounds,
                      static_cast<long long>(elapsedMs));
        log.warn(text);
        // 降级：WM 不受理全屏，就自己把窗口铺满目标显示器（普通窗口仍在面板之下）。
        x11FillMonitorFallback();
    }

    // 渲染一帧：WM 可能在 map 后立刻改窗口尺寸（全屏 configure），先让 ImGui/GL
    // 按新尺寸画一帧，避免回到事件循环前合成到旧尺寸的缓冲。
    renderFrame();

    // 焦点：XRaiseWindow + XSetInputFocus。全屏窗口一般由 WM 主动给焦点，这里显式
    // 再设一次是需求要求的确定性动作；失败只 WARN（grab 会兜底）。
    bool focusRequested = false;
    int focusError = 0;
    {
        X11ErrorScope scope(display);
        XRaiseWindow(display, xid);
        XSetInputFocus(display, xid, RevertToPointerRoot, CurrentTime);
        focusRequested = scope.flush();
        focusError = scope.errorCode();
    }
    if (!focusRequested) {
        char text[224] = {};
        std::snprintf(text, sizeof(text),
                      "region_selector：X11 XSetInputFocus 失败（X error=%d），尝试键盘 grab 兜底",
                      focusError);
        log.warn(text);
    }
    const bool focusOurs = focusIsOverlay(display, xid);

    // 键盘 grab 只在焦点确实不在覆盖层时启用：全屏窗口本来就是焦点窗口，没必要在
    // 正常路径上抢键盘；焦点丢了（WM 拒绝激活 / 别的客户端抢）时它保证 Esc 可用。
    // 指针不 grab：全屏窗口覆盖整块屏，指针不可能落到别处，抓指针只会妨碍其他客户端。
    x11KeyboardGrabbed = false;
    if (!focusOurs) {
        int grabStatus = GrabSuccess;
        bool grabbed = false;
        for (int attempt = 0; attempt < kX11GrabAttempts && !grabbed; ++attempt) {
            grabStatus =
                XGrabKeyboard(display, xid, True, GrabModeAsync, GrabModeAsync, CurrentTime);
            grabbed = (grabStatus == GrabSuccess);
            if (!grabbed && attempt + 1 < kX11GrabAttempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kX11GrabRetryDelayMs));
            }
        }
        x11KeyboardGrabbed = grabbed;
        if (grabbed) {
            log.info("region_selector：X11 键盘 grab 已生效（焦点不在覆盖层，grab 兜底 Esc）");
        } else {
            char text[288] = {};
            std::snprintf(text, sizeof(text),
                          "region_selector：X11 键盘 grab 失败（keyboard=%s），Esc 依赖窗口焦点"
                          "（GLFW_FOCUSED=%d）",
                          grabStatusName(grabStatus), glfwGetWindowAttrib(window, GLFW_FOCUSED));
            log.warn(text);
        }
    }

    // 最后再查一次 map 状态：前面所有请求都可能把窗口从不可见状态救回来。
    viewable = (XGetWindowAttributes(display, xid, &attributes) != 0 &&
                attributes.map_state == IsViewable);
    char text[384] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：X11 覆盖层已显示 viewable=%d fullscreen_requested=%d "
                  "fullscreen_accepted=%d focus=%d grab_keyboard=%d 显示器=(%d,%d) %dx%d",
                  viewable ? 1 : 0, x11FullscreenRequested ? 1 : 0,
                  x11FullscreenAccepted ? 1 : 0, focusOurs ? 1 : 0,
                  x11KeyboardGrabbed ? 1 : 0, monitorRect.x, monitorRect.y, monitorRect.width,
                  monitorRect.height);
    log.info(text);
}

void RegionWindow::Impl::x11FillMonitorFallback() {
    if (window == nullptr || monitorRect.width <= 0 || monitorRect.height <= 0) {
        return;
    }
    Display* display = glfwGetX11Display();
    const Window xid = glfwGetX11Window(window);
    if (display == nullptr || xid == 0) {
        return;
    }
    bool applied = false;
    int errorCode = 0;
    {
        X11ErrorScope scope(display);
        XMoveResizeWindow(display, xid, monitorRect.x, monitorRect.y,
                          static_cast<unsigned int>(monitorRect.width),
                          static_cast<unsigned int>(monitorRect.height));
        XRaiseWindow(display, xid);
        applied = scope.flush();
        errorCode = scope.errorCode();
    }
    if (applied) {
        shot::Logger(host).warn(
            "region_selector：X11 已用 XMoveResizeWindow 铺满显示器作为全屏降级"
            "（顶栏/面板可能仍在覆盖层之上）");
    } else {
        char text[224] = {};
        std::snprintf(text, sizeof(text),
                      "region_selector：X11 全屏降级也失败（XMoveResizeWindow，X error=%d），"
                      "覆盖层可能只显示在旧位置/旧尺寸",
                      errorCode);
        shot::Logger(host).warn(text);
    }
}

void RegionWindow::Impl::x11DeactivateOverlay() {
    if (window == nullptr) {
        return;
    }
    if (!overlayVisible && !x11KeyboardGrabbed) {
        return;  // 从未真正显示 / 已经收过：不重复 ungrab、不刷日志
    }
    Display* display = glfwGetX11Display();
    const Window xid = glfwGetX11Window(window);
    if (display == nullptr || xid == 0) {
        return;
    }
    const bool hadKeyboard = x11KeyboardGrabbed;
    const bool fullscreenAccepted = x11FullscreenAccepted;
    // 先解除键盘 grab：窗口马上要被 glfwHideWindow unmap，grab 必须归还给其它客户端。
    // 不在这里 unmap：WM 管理的窗口交给 glfwHideWindow 发 UnmapWindow，mutter 收到后
    // 自动重新显示顶栏/dock。
    if (hadKeyboard) {
        XUngrabKeyboard(display, CurrentTime);
        XSync(display, False);
    }
    x11KeyboardGrabbed = false;
    x11FullscreenRequested = false;
    x11FullscreenAccepted = false;
    char text[256] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：X11 覆盖层隐藏 fullscreen_accepted=%d grab_keyboard=%d"
                  "（全屏属性留在窗口上，下次显示前重新提交；已解除键盘 grab）",
                  fullscreenAccepted ? 1 : 0, hadKeyboard ? 1 : 0);
    shot::Logger(host).info(text);
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

    Display* display = glfwGetX11Display();
    const Window xid = glfwGetX11Window(window);
    if (display == nullptr || xid == 0) {
        x11PrepareFullscreen();  // 函数内部会 WARN
        return;
    }
    // GLFW 对隐藏窗口的 setPos 会把 WM_NORMAL_HINTS 的 PPosition 故意写成 (0,0)
    // （见 GLFW x11_window.c 的 HACK，用来让某些 WM 在 map 时不再自行摆放）。但
    // reparenting WM 会按这个 hint 摆放窗口：多显示器时全屏会落到主屏。这里把
    // PPosition 改回目标显示器坐标，保证全屏落在鼠标所在的那块屏。
    {
        X11ErrorScope scope(display);
        XSizeHints* hints = XAllocSizeHints();
        if (hints != nullptr) {
            long supplied = 0;
            if (XGetWMNormalHints(display, xid, hints, &supplied) != 0) {
                hints->flags |= PPosition;
                hints->x = monitor.x;
                hints->y = monitor.y;
                XSetWMNormalHints(display, xid, hints);
            }
            XFree(hints);
        }
        scope.flush();
    }
    // 显示前写 EWMH 属性：WM 只在 map 时读取初始 _NET_WM_STATE。
    x11PrepareFullscreen();
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
    Display* display = glfwGetX11Display();
    const Window xid = glfwGetX11Window(window);
    std::string stateText = "(没有 _NET_WM_STATE)";
    if (display != nullptr && xid != 0) {
        const Atom wmState = XInternAtom(display, "_NET_WM_STATE", True);
        const Atom fullscreenAtom = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", True);
        const Atom aboveAtom = XInternAtom(display, "_NET_WM_STATE_ABOVE", True);
        stateText = describeWmState(display, xid, wmState, fullscreenAtom, aboveAtom);
    }
    // 全屏生效时实际几何应等于显示器矩形；不等也照常工作（坐标按实际位置换算），
    // 但在日志里标出来，排查"面板为什么还盖着"时一眼能看到。
    const bool coversMonitor = (windowX == monitorRect.x && windowY == monitorRect.y &&
                                windowWidth == monitorRect.width &&
                                windowHeight == monitorRect.height);
    char text[512] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：X11 覆盖层窗口 pos=(%d,%d) size=%dx%d 目标显示器=(%d,%d) "
                  "%dx%d 覆盖显示器=%d fullscreen_requested=%d fullscreen_accepted=%d "
                  "_NET_WM_STATE=%s（坐标一律按实际窗口位置换算）",
                  windowX, windowY, windowWidth, windowHeight, monitorRect.x, monitorRect.y,
                  monitorRect.width, monitorRect.height, coversMonitor ? 1 : 0,
                  x11FullscreenRequested ? 1 : 0, x11FullscreenAccepted ? 1 : 0,
                  stateText.c_str());
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

    // ---- 关键时序：先隐藏覆盖层（释放 grab + glfwHideWindow），等它真的从画面上消失，
    //      再发布 ----
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
