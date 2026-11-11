#pragma once

// ============================================================================
//  window_target.h —— 顶层窗口枚举与命中测试（内部头，唯一被本插件 .cpp 包含）
//
//  纯 Win32 查询：不碰 GLFW / ImGui / 事件总线，也没有任何绘制状态。覆盖层渲染线程
//  用它回答"光标下面是不是某个窗口、矩形是多少"，结果再交给 region_overlay.cpp
//  做吸附与动画。
//
//  为什么需要这个文件（而不是按坐标找窗口）：
//    覆盖层本身是一个铺满整块显示器的置顶窗口，按坐标命中的 Win32 API 永远返回
//    覆盖层自己，根本看不到下面的真实窗口。所以必须自己枚举顶层窗口、按 Z 序做
//    矩形命中测试。详见 window_target.cpp 文件头。
// ============================================================================

#include <cstdint>
#include <vector>

namespace region_selector {

/**
 * @brief 一个可作为框选目标的顶层窗口：句柄 + 屏幕矩形。
 *
 * 矩形是**物理像素、绝对屏幕坐标**（与 GetCursorPos、capture.request 的 region
 * 同一套系统坐标，只差发布时减虚拟桌面原点）。不保存标题/类名：框选只需要几何，
 * 每次枚举都多拷一份字符串只会增加分配。
 */
struct WindowTarget {
    std::uintptr_t hwnd = 0; /**< 原生窗口句柄（HWND 的整型形式，避免内部头拖入 windows.h）。 */
    int x = 0;               /**< 左边缘（物理像素，绝对屏幕坐标）。 */
    int y = 0;               /**< 上边缘（物理像素，绝对屏幕坐标）。 */
    int width = 0;           /**< 宽度（> 0，已裁剪到虚拟桌面内）。 */
    int height = 0;          /**< 高度（> 0，已裁剪到虚拟桌面内）。 */
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

}  // namespace region_selector
