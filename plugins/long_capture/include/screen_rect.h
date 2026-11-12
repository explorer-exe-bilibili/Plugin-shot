#pragma once

// ============================================================================
//  include/screen_rect.h —— 物理像素矩形（纯数据，不依赖任何平台头）
//
//  长截图的三个几何概念共用它：目标窗口客户区、HUD 摆放候选、HUD 自身位置。
//  坐标语义见 window_target.h：一律是 Windows 系统屏幕坐标（物理像素），
//  发布 capture.request 时才减去虚拟桌面原点（document/CAPTURE.md §3.1）。
// ============================================================================

namespace long_capture {

/**
 * @brief 一个屏幕矩形：左上角 + 宽高，单位是物理像素。
 *
 * 刻意不用 RECT/HWND：HUD 接口头、会话逻辑与核心算法都不应该被迫包含
 * <windows.h>（STYLE.md §6.3 要求 Win32 类型只出现在平台实现里）。
 */
struct ScreenRect {
    int x = 0;      /**< 左边缘（系统屏幕坐标，物理像素）。 */
    int y = 0;      /**< 上边缘（系统屏幕坐标，物理像素）。 */
    int width = 0;  /**< 宽度，必须 > 0 才有意义。 */
    int height = 0; /**< 高度，必须 > 0 才有意义。 */

    /** @brief 右边缘（不含）。 */
    int right() const noexcept { return x + width; }
    /** @brief 下边缘（不含）。 */
    int bottom() const noexcept { return y + height; }
    /** @brief 是否是一个有面积的矩形。 */
    bool valid() const noexcept { return width > 0 && height > 0; }
};

}  // namespace long_capture
