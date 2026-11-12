#pragma once

// ============================================================================
//  window_target.h —— 顶层窗口枚举与命中测试（内部头，唯一被本插件 .cpp 包含）
//
//  纯平台查询：不碰 GLFW / ImGui / 事件总线，也没有任何绘制状态。覆盖层渲染线程
//  用它回答"光标下面是不是某个窗口、矩形是多少"，结果再交给 region_overlay.cpp
//  做吸附与动画。
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
 * @brief 枚举当前能作为框选目标的顶层窗口，按 Z 序自上而下返回。
 *
 * 排除规则（顺序无关，任一命中就跳过）：覆盖层自己、不可见、最小化、零尺寸、
 * DWM cloaked、`WS_EX_TOOLWINDOW`。矩形优先用 DWM 的扩展帧边界，失败退回
 * `GetWindowRect`，并裁剪到虚拟桌面矩形。
 *
 * @param[in] excludeHwnd 必须排除的窗口句柄（覆盖层自己）；0 表示不排除。
 * @return 候选窗口列表；没有候选时为空。
 * @note 纯查询、可在任何线程调用；内部只用 user32 + dwmapi，不初始化任何图形栈。
 */
std::vector<WindowTarget> listWindowTargets(std::uintptr_t excludeHwnd);

/**
 * @brief 找到包含点 (x, y) 的最上层候选窗口。
 *
 * @param[in] x 物理像素绝对屏幕横坐标。
 * @param[in] y 物理像素绝对屏幕纵坐标。
 * @param[in] excludeHwnd 覆盖层自己的窗口句柄（不排除的话永远命中自己）。
 * @param[out] out 命中窗口的矩形；未命中时保持调用前的值不变。
 * @return true = 命中；false = 该点下没有候选窗口。
 * @note 等价于 listWindowTargets 后取第一个包含点的项；覆盖层每帧只需枚举一次，
 *       所以渲染循环用 listWindowTargets + 自己的缓存，不逐帧调这个函数。
 */
bool findWindowAt(int x, int y, std::uintptr_t excludeHwnd, WindowTarget& out);

#if defined(__linux__)
/**
 * @brief X11 版的候选窗口枚举：XQueryTree 遍历 root 的直接子窗口（底 → 顶）。
 *
 * 每层 root 子窗口先按 `_NET_WM_WINDOW_TYPE`（在 client 窗口上，reparenting WM 的
 * frame 需要往下找一层）跳过 DOCK / TOOLTIP / MENU 等 EWMH 类型，再排除覆盖层自己、
 * 不可映射、InputOnly、零尺寸窗口；矩形用 `XTranslateCoordinates` 换算到 root 坐标。
 *
 * @param[in] excludeXid 覆盖层自己的 X11 Window（0 = 不排除）。
 * @return 候选窗口列表，按 Z 序自上而下；没有候选时为空。
 * @note 只有 X11 分支调用；Wayland 的 `listWindowTargets()` 恒返回空列表。
 */
std::vector<WindowTarget> listWindowTargetsX11(std::uintptr_t excludeXid);

/**
 * @brief 找到包含点 (x, y) 的最上层 X11 候选窗口（root 坐标）。
 *
 * @param[in] x 物理像素 X screen 横坐标。
 * @param[in] y 物理像素 X screen 纵坐标。
 * @param[in] excludeXid 覆盖层自己的 X11 Window（不排除的话永远命中自己或自己的 frame）。
 * @param[out] out 命中窗口的矩形；未命中时保持调用前的值不变。
 * @return true = 命中；false = 该点下没有候选窗口。
 */
bool findWindowAtX11(int x, int y, std::uintptr_t excludeXid, WindowTarget& out);
#endif

}  // namespace region_selector
