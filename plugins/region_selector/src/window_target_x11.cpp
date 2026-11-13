// window_target_x11 —— Linux/X11 的顶层窗口枚举、EWMH 类型过滤与命中测试。
//
// 为什么不用 XQueryPointer 之类的"逐点命中"（关键，不要"优化"回去）：
//   覆盖层是一个铺满整块显示器、带 _NET_WM_STATE_ABOVE 的置顶窗口，屏幕上的每个
//   像素都属于它自己；按坐标找窗口只会返回覆盖层（或它的 WM frame）。所以必须自己
//   从 root 枚举直接子窗口，按 Z 序做矩形命中测试。
//
// XQueryTree 返回的 children 顺序：X11 协议规定"从最底层到最顶层"，所以本文件反向
// 遍历，得到与 Windows 版一致的"自上而下"候选列表。
//
// reparenting WM（Mutter / KWin / Xfwm 等）会把客户端窗口塞进一个 frame 窗口，root
// 的直接子窗口是 frame 而不是 client：
//   · 覆盖层自己：frame 的 XID 不等于 glfwGetX11Window()，只按 XID 排除会命中 frame
//     自己的全屏矩形（选框永远吸附在覆盖层上）。所以这里再往下找一层 client，
//     命中覆盖层的 client 或 frame 都跳过；
//   · EWMH 属性（_NET_WM_WINDOW_TYPE）挂在 client 上，不在 frame 上；工具提示、
//     面板、菜单的类型过滤同样要下钻一层才能生效。
// 正常窗口只有一个 client 子窗口，所以"frame → 唯一子窗口"就够（不递归整棵树，
// 避免异常窗口把渲染线程卡住）；没有 WM 时窗口自己就是 root 子窗口。
//
// 类型过滤与 Windows 版的对应关系：
//   _NET_WM_WINDOW_TYPE_DOCK             ≈ 任务栏、面板、Dock 栏（Windows 的 Shell_TrayWnd）
//   _NET_WM_WINDOW_TYPE_TOOLBAR          ≈ 工具栏窗口（WS_EX_TOOLWINDOW 类目标）
//   _NET_WM_WINDOW_TYPE_TOOLTIP / MENU / DROPDOWN_MENU / POPUP_MENU / COMBO / DND
//                                        = 弹出层，用户不会想截它们
//   _NET_WM_WINDOW_TYPE_DESKTOP          = 桌面窗口，几乎铺满全屏，命中它会盖住一切
//   _NET_WM_WINDOW_TYPE_NOTIFICATION     = 通知气泡
//   _NET_WM_WINDOW_TYPE_SPLASH / UTILITY = 启动画面 / 工具面板（与 WS_EX_TOOLWINDOW 对齐）
// 未知/没有类型的窗口照常作为候选：宁可多列一个，也不要漏掉用户想截的窗口。
//
// DOCK 为什么**不跳过**（需求：悬停任务栏/面板 = 选中当前显示器全屏）：
//   DOCK 就是面板/任务栏（GNOME 顶栏、dash-to-dock、xfce4-panel、polybar 等）。
//   旧版把它和 MENU/TOOLTIP 一起跳过，结果是"光标停在面板上什么都不选中"——这正是
//   本次要修的行为。现在 DOCK 窗口收进 WindowScan::panels，由交互层映射成"当前
//   显示器全屏"；它本身不能作为截图目标，所以不进 windows 列表。
//
// 坐标：root 窗口坐标即像素（X 没有 DPI 缩放，Xft 的缩放只影响字体）。矩形用
// XTranslateCoordinates(frame, root, 0, 0, ...) 换算，再裁剪到 X screen 范围
// （单 X screen 限制见 README「Linux（X11）」）。
//
// 线程：纯查询，只由覆盖层 UI 线程调用；glfwGetX11Display() 返回 GLFW 的连接，
// 调用前 GLFW 必须已初始化（startSelection 里 ensureGraphics 已经保证）。

#include "window_target.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

// 自己包含 Xlib 头；glfw3native.h 会声明 RROutput 等类型并拉 <X11/extensions/Xrandr.h>，
// 因此编译期需要 libxrandr-dev（与 GLFW 运行时同一套依赖）。
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace region_selector {
namespace {

/** @brief 向 frame 子树里找 client 的最大层数：一般 1 层，个别 WM 会多包一层。 */
constexpr int kClientSearchDepth = 3;

/** @brief 不能作为框选目标的 EWMH 窗口类型（完整理由见文件头；DOCK 不在此列）。 */
const char* const kSkippedTypeNames[] = {
    "_NET_WM_WINDOW_TYPE_DESKTOP",      "_NET_WM_WINDOW_TYPE_TOOLBAR",
    "_NET_WM_WINDOW_TYPE_MENU",         "_NET_WM_WINDOW_TYPE_UTILITY",
    "_NET_WM_WINDOW_TYPE_SPLASH",       "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
    "_NET_WM_WINDOW_TYPE_POPUP_MENU",   "_NET_WM_WINDOW_TYPE_TOOLTIP",
    "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_COMBO",
    "_NET_WM_WINDOW_TYPE_DND",
};

constexpr std::size_t kSkippedTypeCount = std::size(kSkippedTypeNames);

/** @brief 面板 / 任务栏的 EWMH 类型：命中的窗口收进 WindowScan::panels（理由见文件头）。 */
constexpr char kDockTypeName[] = "_NET_WM_WINDOW_TYPE_DOCK";

/**
 * @brief 窗口上是否存在某个属性（值不看，只看属性在不在）。
 *
 * 用于识别 client 窗口：EWMH/reparenting WM 会给 client 设 `WM_STATE`，frame 没有。
 * XGetWindowProperty 的 offset=0 / length=0 只取属性类型，不搬运数据。
 */
bool hasProperty(Display* display, Window window, Atom property) {
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(display, window, property, 0, 0, False, AnyPropertyType,
                                          &actualType, &actualFormat, &itemCount, &bytesAfter,
                                          &data);
    if (data != nullptr) {
        XFree(data);
    }
    return status == Success && actualType != None;
}

/**
 * @brief 在 window 及其后代里找带 `WM_STATE` 的 client 窗口。
 *
 * @param[in]  display  X11 连接。
 * @param[in]  window   起点（root 的直接子窗口）。
 * @param[in]  wmState  `WM_STATE` atom：reparenting WM 只把它设在 client 上。
 * @param[in]  depth    还能往下找几层。
 * @param[out] out      找到时写 client 窗口。
 * @return true = 找到；false = 没有 WM_STATE（没有 WM / 非 reparenting / 属性缺失）。
 */
bool findClientWindow(Display* display, Window window, Atom wmState, int depth, Window& out) {
    if (hasProperty(display, window, wmState)) {
        out = window;
        return true;
    }
    if (depth <= 0) {
        return false;
    }
    Window rootReturn = 0;
    Window parentReturn = 0;
    Window* children = nullptr;
    unsigned int childCount = 0;
    if (XQueryTree(display, window, &rootReturn, &parentReturn, &children, &childCount) == 0) {
        return false;
    }
    bool found = false;
    for (unsigned int index = 0; index < childCount && !found; ++index) {
        found = findClientWindow(display, children[index], wmState, depth - 1, out);
    }
    if (children != nullptr) {
        XFree(children);
    }
    return found;
}

/**
 * @brief 把 root 的直接子窗口映射到它的 client 窗口（reparenting WM 的 frame 需要下钻）。
 *
 * @param[in] display X11 连接。
 * @param[in] window  root 的直接子窗口（可能本身已经是 client）。
 * @param[in] wmState `WM_STATE` atom：reparenting WM 只把它设在 client 上。
 * @return client 窗口；找不到（没有 WM / 非 reparenting / 属性缺失）时返回 window 自己。
 */
Window clientWindowOf(Display* display, Window window, Atom wmState) {
    Window client = window;
    if (findClientWindow(display, window, wmState, kClientSearchDepth, client)) {
        return client;
    }
    return window;
}

/**
 * @brief 窗口的 `_NET_WM_WINDOW_TYPE` 里是否含有 types 里的任何一个类型。
 *
 * @param[in] display  X11 连接。
 * @param[in] window   通常是 client 窗口（属性挂在它上面）。
 * @param[in] wmType   `_NET_WM_WINDOW_TYPE` atom。
 * @param[in] types    待匹配的类型 atom 列表（DOCK 或"跳过"类型）。
 * @param[in] typeCount 列表长度。
 * @return true = 至少命中一个类型；false = 没有类型属性或类型都不在列表里。
 * @note 一次最多读 32 个 atom（窗口类型列表实际只有 1~2 个），bytesAfter 不追问。
 */
bool hasAnyWindowType(Display* display, Window window, Atom wmType, const Atom* types,
                      std::size_t typeCount) {
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(display, window, wmType, 0, 32, False, XA_ATOM,
                                          &actualType, &actualFormat, &itemCount, &bytesAfter,
                                          &data);
    bool matched = false;
    if (status == Success && actualType == XA_ATOM && actualFormat == 32 && data != nullptr) {
        const auto* atoms = reinterpret_cast<const Atom*>(data);
        for (unsigned long index = 0; index < itemCount && !matched; ++index) {
            for (std::size_t candidate = 0; candidate < typeCount; ++candidate) {
                if (atoms[index] == types[candidate]) {
                    matched = true;
                    break;
                }
            }
        }
    }
    if (data != nullptr) {
        XFree(data);
    }
    return matched;
}

/** @brief 把 root 坐标矩形裁剪到 X screen；裁剪后为空返回 false。 */
bool clipToScreen(Display* display, WindowTarget& target) {
    const int screen = DefaultScreen(display);
    const int screenWidth = DisplayWidth(display, screen);
    const int screenHeight = DisplayHeight(display, screen);
    const int left = std::max(target.x, 0);
    const int top = std::max(target.y, 0);
    const int right = std::min(target.x + target.width, screenWidth);
    const int bottom = std::min(target.y + target.height, screenHeight);
    if (right <= left || bottom <= top) {
        return false;
    }
    target.x = left;
    target.y = top;
    target.width = right - left;
    target.height = bottom - top;
    return true;
}

}  // namespace

WindowScan scanWindowTargetsX11(std::uintptr_t excludeXid) {
    WindowScan scan;
    Display* display = glfwGetX11Display();
    if (display == nullptr) {
        // GLFW 没初始化 / X11 连接失败：没有窗口列表，覆盖层仍可纯拖框。
        return scan;
    }
    const Window root = DefaultRootWindow(display);
    Window rootReturn = 0;
    Window parentReturn = 0;
    Window* children = nullptr;
    unsigned int childCount = 0;
    if (XQueryTree(display, root, &rootReturn, &parentReturn, &children, &childCount) == 0) {
        return scan;
    }

    const auto wmState = XInternAtom(display, "WM_STATE", True);
    const auto wmType = XInternAtom(display, "_NET_WM_WINDOW_TYPE", True);
    Atom skippedTypes[kSkippedTypeCount] = {};
    for (std::size_t index = 0; index < kSkippedTypeCount; ++index) {
        skippedTypes[index] = XInternAtom(display, kSkippedTypeNames[index], True);
    }
    // 面板/任务栏类型单独 intern：命中它可以吸附到"当前显示器全屏"（见文件头）。
    const auto dockType = (wmType != None) ? XInternAtom(display, kDockTypeName, True) : None;

    // children 是底 → 顶，反向遍历得到"最上层优先"，与 Windows 的 Z 序列表一致。
    for (unsigned int index = childCount; index > 0; --index) {
        const Window child = children[index - 1];
        if (child == None) {
            continue;
        }
        // 没有 WM（或 WM 不 reparent）时窗口自己就是 root 子窗口：直接排除。
        if (excludeXid != 0 && static_cast<std::uintptr_t>(child) == excludeXid) {
            continue;
        }
        const Window client =
            (wmState != None) ? clientWindowOf(display, child, wmState) : child;
        if (wmState != None && client != child) {
            // reparenting WM 的 frame 的 XID 与 client 不同：按 client 排除覆盖层自己。
            if (excludeXid != 0 && static_cast<std::uintptr_t>(client) == excludeXid) {
                continue;
            }
        }

        XWindowAttributes attributes{};
        if (XGetWindowAttributes(display, child, &attributes) == 0) {
            continue;  // 枚举与查询之间窗口被销毁
        }
        // InputOnly 窗口没有可见内容；不可映射（最小化/其它虚拟桌面）与零尺寸窗口
        // 要么用户看不到，要么矩形没有意义。
        if (attributes.c_class == InputOnly || attributes.map_state != IsViewable) {
            continue;
        }
        if (attributes.width <= 0 || attributes.height <= 0) {
            continue;
        }
        // EWMH 类型属性挂在 client 上：reparenting WM 的 frame 上没有这个属性，
        // 所以统一检查 client（没有 WM_STATE 时 client 就是窗口自己）。
        bool isDock = false;
        if (dockType != None) {
            isDock = hasAnyWindowType(display, client, wmType, &dockType, 1);
        }
        if (!isDock && wmType != None &&
            hasAnyWindowType(display, client, wmType, skippedTypes, kSkippedTypeCount)) {
            continue;
        }

        // frame 左上角 → root 坐标。用 frame（而不是 client）的矩形：覆盖层背景是
        // 屏幕快照，用户看到的窗口轮廓包含窗口管理器画的标题栏/边框，与快照一致。
        int rootX = 0;
        int rootY = 0;
        Window childReturn = 0;
        if (XTranslateCoordinates(display, child, root, 0, 0, &rootX, &rootY, &childReturn) == 0) {
            continue;
        }
        WindowTarget target;
        target.hwnd = static_cast<std::uintptr_t>(client);
        target.x = rootX;
        target.y = rootY;
        target.width = attributes.width;
        target.height = attributes.height;
        if (!clipToScreen(display, target)) {
            continue;
        }
        if (isDock) {
            // 面板/任务栏：不进候选窗口（它不该被当成截图目标），只参与悬停判定。
            scan.panels.push_back(target);
        } else {
            scan.windows.push_back(target);
        }
    }
    if (children != nullptr) {
        XFree(children);
    }
    return scan;
}

}  // namespace region_selector
