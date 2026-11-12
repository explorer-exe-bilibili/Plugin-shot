#pragma once

// ============================================================================
//  include/window_target.h —— 找到"要滚动截图的窗口"并取它的客户区（平台接口头）
//
//  坐标一律是 Windows 系统屏幕坐标、物理像素（宿主进程已声明 PER_MONITOR_AWARE_V2，
//  findForeground 里还会对调用线程再兜一次）。发布 capture.request 时由调用方
//  减去虚拟桌面原点，与 document/CAPTURE.md §3.1 的 region 语义对齐。
//
//  本头文件不包含 <windows.h>：句柄用 std::uintptr_t 传，平台实现见
//  src/window_target_windows.cpp（非 Windows 平台不构建本插件）。
// ============================================================================

#include <cstdint>
#include <string>

#include "screen_rect.h"

namespace long_capture {

/**
 * @brief 一次长截图的目标：窗口句柄 + 客户区矩形。
 *
 * 用客户区而不是整窗：标题栏/边框是系统画的，滚动的是客户区内容；把边框拼进
 * 长图会出现每一段都重复一遍标题栏的荒谬结果。
 */
struct TargetWindow {
    std::uintptr_t handle = 0; /**< 原生窗口句柄（HWND 的整型形式）；0 = 没有目标。 */
    ScreenRect client;         /**< 客户区在屏幕上的矩形（物理像素，绝对屏幕坐标）。 */
};

/**
 * @brief 前台窗口查询：物理像素客户区 + 本进程窗口的跳过规则。
 *
 * 无状态；每次会话开始时调一次。
 */
class WindowTarget {
public:
    /**
     * @brief 取本次要截的窗口并算出它的客户区。
     *
     * 规则（按顺序）：
     *   1. 取 GetForegroundWindow()；
     *   2. 前台窗口属于本进程（托盘隐藏窗口 / 设置页 / 上一次的 HUD）时，按真实 Z 序
     *      （GetTopWindow + GW_HWNDNEXT）找下一个**非本进程**、可见、未最小化、
     *      客户区非空的窗口；
     *   3. 客户区 = GetClientRect() + ClientToScreen()，物理像素。
     *
     * @param[out] out    成功时填好句柄与客户区；失败时保持调用前的值不变。
     * @param[out] error  失败时的中文原因（给 HUD/托盘气泡提示用）；成功时清空。
     * @return true = 找到了可截图的目标窗口。
     * @note 目标窗口最小化 / 不可见 / 客户区为零时按失败处理：这时抓出来的内容
     *       是恢复前的旧画面或者一片空白，硬拍只会得到一张废图。
     */
    static bool findForeground(TargetWindow& out, std::string& error);

    /**
     * @brief 物理虚拟桌面的左上角（SM_XVIRTUALSCREEN / SM_YVIRTUALSCREEN）。
     *
     * screen_capture 的 describe 以虚拟桌面左上角为 (0,0)；region 也要用同一套
     * 坐标，所以发布前要把窗口的屏幕坐标减掉这两个值（多屏且主屏不在最左时
     * 它们可能是负数）。
     *
     * @param[out] originX 虚拟桌面原点 X。
     * @param[out] originY 虚拟桌面原点 Y。
     */
    static void virtualDesktopOrigin(int& originX, int& originY);
};

}  // namespace long_capture
