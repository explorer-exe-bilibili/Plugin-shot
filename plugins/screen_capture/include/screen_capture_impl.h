#pragma once

// ============================================================================
//  screen_capture_impl.h —— 内部头：Windows GDI 平台实现
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。这里会出现 Win32 类型
//  （HDC / HBITMAP / DWORD），公共头 screen_capture.h 不包含 <windows.h>，
//  宿主与其它插件看不到任何 GDI 对象。
//
//  分层：ScreenCapture（screen_capture.cpp）负责契约——校验宿主入参、把平台结果
//  映射成返回码、写日志；GdiDesktopCapture（gdi_desktop_capture.cpp）只做系统调用。
// ============================================================================

// TODO: 非 Windows 平台（X11 的 XGetImage、macOS 的 CGDisplayCreateImage）还没有实现：
// CMake 目前在非 Windows 上直接跳过整个工程，不保留未验证的编译分支。以后补平台层时
// 照着 GdiDesktopCapture 的两个公开函数实现即可，公共头、ABI 胶水与 metadata 都不用动。

#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX  // 防止 <windows.h> 的 min/max 宏把 std::min / std::max 顶掉
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "plugin_api.h"
#include "shot_plugin_helpers/shot_logger.h"

namespace screen_capture {

/**
 * @brief 虚拟桌面的 Windows GDI 抓取实现（仅本插件内部使用）。
 *
 * 不持有任何成员状态：每次抓屏都是一次完整的 GetDC → BitBlt → GetDIBits 生命周期，
 * 句柄在函数返回前全部释放。截图是低频操作，为省一次建 DC 而把句柄挂在实例上只会
 * 引入跨调用状态和插件卸载时的清理问题。
 *
 * DPI 由本层自己保证：两个公开函数进入时都显式把调用线程的 DPI 感知上下文临时提升到
 * PER_MONITOR_AWARE_V2（运行期解析，设置失败只 warn），返回前恢复原上下文。所以宿主
 * 是否在清单里声明 DPI aware 不影响插件拿到的几何与像素。
 */
class GdiDesktopCapture {
public:
    /**
     * @brief 读当前虚拟桌面的几何（物理像素与系统坐标）。
     *
     * 进入时先把调用线程的 DPI 上下文提升到 PER_MONITOR_AWARE_V2（失败只 warn），
     * 所以这里拿到的是物理像素尺寸与虚拟桌面左上角的系统坐标，不依赖宿主进程的
     * DPI 感知声明；返回前恢复调用前的上下文。
     *
     * @param[out] info 成功时只填 width/height/origin_x/origin_y 四个字段；
     *                  struct_size 与 pixel_format 由调用方负责（它们属于契约层）。
     * @param[in]  log  宿主日志；DPI 上下文提升失败（老系统）时写一条 warn。
     * @return true = 成功；false = 没有可采集的屏幕（宽或高 <= 0）。
     */
    static bool queryGeometry(ShotCaptureInfo& info, shot::Logger log);

    /**
     * @brief 把整个虚拟桌面抓进 dst（BGRA8，尺寸必须与 target 一致）。
     * @param[in] dst    目标图像；调用方已校验过 data/width/height/stride/channels。
     * @param[in] target 当前几何，提供 BitBlt 的源坐标与宽高。
     * @param[in] log    宿主日志；失败时就地写明是哪一步失败及 GetLastError()。
     * @return true = 成功；false = 系统调用失败（已写日志）。
     * @warning 只写像素，不修改 dst 的任何结构体字段。
     * @note 内部同样会临时提升线程 DPI 上下文：BitBlt 的源坐标必须和 queryGeometry
     *       读到的几何在同一套物理像素坐标系里。
     */
    static bool captureTo(const ShotImage& dst, const ShotCaptureInfo& target, shot::Logger log);

private:
    /**
     * @brief 用 GetDIBits 把位图读成 BGRA 并写进 dst，最后把 alpha 置 255。
     * @param[in] deviceDc 已创建、用于识别设备的 DC（内存 DC 即可）。
     * @param[in] bitmap   位图，内容已经是要返回的整屏画面。
     * @param[in] dst      目标图像；stride 可以为负或大于 width*4。
     * @param[in] log      宿主日志。
     * @return true = 成功；false = GetDIBits 失败或临时缓冲分配失败（已写日志）。
     */
    static bool copyPixels(HDC deviceDc, HBITMAP bitmap, const ShotImage& dst, shot::Logger log);

    /**
     * @brief 把 BGRA 图像每一行的 alpha 字节置成 255。
     *
     * 屏幕 DC 没有 alpha 通道，GetDIBits 的 32bpp 输出里第 4 字节通常是 0；
     * 不置 255 的话保存成 PNG/WebP 会变成整张透明。
     *
     * @param[in,out] data   图像首行第一个像素；不能为 nullptr。
     * @param[in]     width  像素宽，>= 1。
     * @param[in]     height 像素高，>= 1。
     * @param[in]     stride 行距（字节），可以为负；|stride| >= width*4。
     */
    static void setAlphaOpaque(std::uint8_t* data, std::int32_t width, std::int32_t height,
                               std::int64_t stride);

    /**
     * @brief 记一条失败日志：失败步骤 + GetLastError()。
     * @param[in] log   宿主日志。
     * @param[in] step  失败的系统调用名，如 "BitBlt"；编译期静态字符串。
     * @param[in] error 调用方在失败后立刻取到的 GetLastError()。
     */
    static void logFailure(shot::Logger log, const char* step, DWORD error);
};

}  // namespace screen_capture
