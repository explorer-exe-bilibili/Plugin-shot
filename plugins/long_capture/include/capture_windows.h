#pragma once

// ============================================================================
//  include/capture_windows.h —— 插件自带的 Windows 采集层（GDI BitBlt）
//
//  为什么不再走宿主流水线：长截图每帧都要一张目标区域的画面。走
//  capture.request → screen_capture → image_saver 的路径会让每一帧都落一个 PNG、
//  每一帧都发 capture.done，于是 notify/clipboard 等订阅者每帧都弹一次"截图成功"。
//  这里直接 GetDC(nullptr) + BitBlt 把目标区域拷进内存位图，帧数据只进拼接器，
//  全程不落盘、不发事件；只有最终保存时才写一张 PNG 并发布一次 capture.done。
//
//  坐标：capture() 收的是**绝对屏幕坐标、物理像素**（与 GetCursorPos / BitBlt 同一套
//  系统坐标）。region_selector 发布的是虚拟桌面坐标，调用方先用
//  virtualDesktopOrigin() 加上原点再传进来。
//
//  DPI：宿主进程已声明 PER_MONITOR_AWARE_V2，但会话线程不是宿主 main 线程，所以
//  每次抓帧前都对调用线程再兜一次 SetThreadDpiAwarenessContext，返回前恢复原上下文
//  （与 plugins/screen_capture/src/gdi_desktop_capture.cpp 同一套做法）。
//
//  本头文件不包含 <windows.h>；实现见 src/capture_windows.cpp，只有 Windows 构建。
// ============================================================================

#include <string>

#include "capture_frame.h"
#include "screen_rect.h"

namespace long_capture {

/**
 * @brief 目标区域的逐帧抓取器：一次调用 = 一张 BGRA8 → RGBA8 的紧凑帧。
 *
 * 无状态、任意线程可调；每次调用自己建/释放 GDI 对象，不持有句柄、不缓存位图。
 * 抓帧失败（区域非法、GDI 资源耗尽、BitBlt/GetDIBits 失败）收敛成 false + 中文原因，
 * 由会话线程按"连续失败 N 次"结束会话。
 */
class ScreenGrabber {
public:
    /**
     * @brief 抓取屏幕上的一个矩形区域，输出紧凑 RGBA8 帧。
     *
     * @param[in]  screenRect 目标区域（绝对屏幕坐标、物理像素）；宽高必须 > 0。
     * @param[out] out        成功时填好 width/height/rgba（行间无 padding，RGBA 顺序）。
     * @param[out] error      失败时的中文原因；成功时清空。
     * @return true = 已抓到一帧完整画面。
     * @note 不合成鼠标光标：长图里出现一个固定的光标会破坏行指纹匹配，而且用户滚动时
     *       光标位置本身没有意义。
     */
    static bool capture(const ScreenRect& screenRect, CaptureFrame& out, std::string& error);

    /**
     * @brief 物理虚拟桌面的左上角（SM_XVIRTUALSCREEN / SM_YVIRTUALSCREEN）。
     *
     * region_selector 的 region 以虚拟桌面左上角为原点；BitBlt 要的是系统屏幕坐标，
     * 两者相差这个原点（多屏且主屏不在最左/最上时可能是负数）。
     *
     * @param[out] originX 虚拟桌面原点 X。
     * @param[out] originY 虚拟桌面原点 Y。
     */
    static void virtualDesktopOrigin(int& originX, int& originY);
};

}  // namespace long_capture
