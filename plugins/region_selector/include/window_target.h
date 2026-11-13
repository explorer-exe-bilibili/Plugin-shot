#pragma once

// ============================================================================
//  window_target.h —— 顶层窗口枚举与命中测试（内部头，唯一被本插件 .cpp 包含）
//
//  纯平台查询：不碰 GLFW / ImGui / 事件总线，也没有任何绘制状态。覆盖层渲染线程
//  用它回答"光标下面是不是某个窗口、矩形是多少"，结果再交给 region_overlay.cpp
//  做吸附与动画。
//
//  一次扫描返回**两份**矩形（见 WindowScan）：
//    · windows：普通顶层窗口，悬停中它时吸附到该窗口；
//    · panels ：任务栏 / 面板（Windows `Shell_TrayWnd`、X11 `_NET_WM_WINDOW_TYPE_DOCK`）。
//      它们不是可截的窗口（不能把一条任务栏当图片存下来），悬停中时要吸附到"当前
//      显示器全屏"，所以单独一份，由交互层（region_overlay.cpp）决定映射成哪块矩形。
//
//  三个平台实现（同名接口只保留一份编译单元，Linux 两套用不同函数名）：
//    · Windows：EnumWindows + DWM 边界（window_target.cpp）；
//    · Linux X11：XQueryTree 遍历 root 的子窗口（window_target_x11.cpp）；
//    · Linux Wayland：空实现（协议不允许枚举，window_target_wayland.cpp）。
//
//  为什么需要这个文件（而不是按坐标找窗口）：
//    覆盖层本身是一个铺满整块显示器的置顶窗口，按坐标命中的系统 API 永远返回
//    覆盖层自己，根本看不到下面的真实窗口。所以必须自己枚举顶层窗口、按 Z 序做
//    矩形命中测试。详见各实现文件头。
// ============================================================================

#include <cstdint>
#include <vector>

namespace region_selector {

/**
 * @brief 一个可作为框选目标的顶层窗口：原生句柄 + 屏幕矩形。
 *
 * 矩形是**物理像素、绝对屏幕坐标**（Windows = 虚拟桌面坐标；X11 = X screen / root
 * 坐标，原点恒为 (0,0)），与光标位置、capture.request 的 region 同一套系统坐标，
 * 只差发布时减虚拟桌面原点。不保存标题/类名：框选只需要几何，每次枚举都多拷一份
 * 字符串只会增加分配。
 */
struct WindowTarget {
    std::uintptr_t hwnd = 0; /**< 原生句柄（HWND / X11 Window，转成整数避免内部头拖平台头文件）。 */
    int x = 0;               /**< 左边缘（物理像素，绝对屏幕坐标）。 */
    int y = 0;               /**< 上边缘（物理像素，绝对屏幕坐标）。 */
    int width = 0;           /**< 宽度（> 0，已裁剪到虚拟桌面 / X screen 内）。 */
    int height = 0;          /**< 高度（> 0，已裁剪到虚拟桌面 / X screen 内）。 */
};

/**
 * @brief 一次窗口扫描的结果：候选窗口 + 任务栏/面板，两份都按 Z 序自上而下。
 *
 * 分成两份是因为**悬停后的动作不同**：命中 windows 里的窗口 = 吸附到那个窗口矩形；
 * 命中 panels 里的任务栏/面板 = 吸附到"光标所在显示器的全屏"（需求：悬停任务栏或
 * 屏幕四角都要能选中当前显示器）。任务栏本身永远不该被当成可截窗口发布出去，所以
 * 它不能混进 windows 列表里由绘制层自己判断。
 */
struct WindowScan {
    std::vector<WindowTarget> windows; /**< 候选窗口（可截图的普通顶层窗口），Z 序。 */
    std::vector<WindowTarget> panels;  /**< 任务栏 / 面板矩形，Z 序；不可截图，只用于命中。 */
};

/**
 * @brief 扫描当前能作为框选目标的顶层窗口与任务栏/面板，按 Z 序自上而下返回。
 *
 * 排除规则（顺序无关，任一命中就跳过）：覆盖层自己、不可见、最小化、零尺寸、
 * DWM cloaked、`WS_EX_TOOLWINDOW`。矩形优先用 DWM 的扩展帧边界，失败退回
 * `GetWindowRect`，并裁剪到虚拟桌面矩形。
 *
 * 任务栏（`Shell_TrayWnd` / `Shell_SecondaryTrayWnd`）单独收进 `panels`：它们带
 * `WS_EX_TOOLWINDOW`，如果沿用候选窗口的排除规则就永远找不到；而需求要求"悬停
 * 任务栏 = 选中当前显示器全屏"，所以先按类名认出来、单独放一份（不参与截图）。
 *
 * @param[in] excludeHwnd 必须排除的窗口句柄（覆盖层自己）；0 表示不排除。
 * @return 扫描结果；两份列表都可能为空。
 * @note 纯查询、可在任何线程调用；内部只用 user32 + dwmapi，不初始化任何图形栈。
 *       覆盖层每 30ms 调一次并把结果缓存起来（交互层还要用它做角落/面板判定），
 *       不要逐帧调用。
 */
WindowScan scanWindowTargets(std::uintptr_t excludeHwnd);

#if defined(__linux__)
/**
 * @brief X11 版的窗口扫描：XQueryTree 遍历 root 的直接子窗口（底 → 顶）。
 *
 * 每层 root 子窗口先按 `_NET_WM_WINDOW_TYPE`（在 client 窗口上，reparenting WM 的
 * frame 需要往下找一层）跳过 TOOLTIP / MENU 等 EWMH 类型，再排除覆盖层自己、
 * 不可映射、InputOnly、零尺寸窗口；矩形用 `XTranslateCoordinates` 换算到 root 坐标。
 *
 * `_NET_WM_WINDOW_TYPE_DOCK`（任务栏、面板、Dock 栏）**不跳过**，而是收进
 * `panels`：需求要求悬停面板时选中"当前显示器全屏"，而不是忽略它。
 *
 * @param[in] excludeXid 覆盖层自己的 X11 Window（0 = 不排除）。
 * @return 扫描结果，两份列表都按 Z 序自上而下。
 * @note 只有 X11 分支调用；Wayland 的 `scanWindowTargets()` 恒返回空结果。
 */
WindowScan scanWindowTargetsX11(std::uintptr_t excludeXid);
#endif

}  // namespace region_selector
