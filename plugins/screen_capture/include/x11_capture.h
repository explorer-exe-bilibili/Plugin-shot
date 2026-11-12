#pragma once

// ============================================================================
//  x11_capture.h —— 内部头：Linux/X11 整屏采集
//
//  **不是插件对外接口**：只有本插件的 Linux 实现包含它。这里会出现 Xlib 类型
//  （Display / XImage），公共头 screen_capture.h 不包含 <X11/Xlib.h>。
//
//  职责：只做 X11 系统调用与像素转换，不判断契约返回码；契约层
//  （screen_capture_linux.cpp）负责入参校验、返回码映射与日志分类。
//
//  线程模型：本类的函数全部是静态、无状态的；每次调用自己 XOpenDisplay，用完
//  XCloseDisplay，绝不让 Display 逃出一次调用。Xlib 的 Display 不是线程安全的，
//  而宿主进程里 settings_ui（GLFW）与 clipboard（X11 selection）也可能各开自己的
//  连接；不共享连接就不需要插件内的锁，也不会在插件卸载时留下后台线程或悬挂连接。
// ============================================================================

#include <cstdint>

#include "plugin_api.h"
#include "shot_plugin_helpers/shot_logger.h"

// Xlib 放在项目头之后：X.h 会定义 Bool / Status / None 等宏，别让它们影响
// C++ 标准库或契约头的解析。
#include <X11/Xlib.h>

namespace screen_capture {

/**
 * @brief Linux/X11 整屏采集：XShmGetImage 优先，XGetImage 兜底，可选 XFixes 光标合成。
 *
 * 采集目标只有一个：默认 X screen 的 root window。多显示器由 XRandR / Xinerama
 * 合并成一个 screen，所以 describe 报整个 screen 的 DisplayWidth / DisplayHeight，
 * origin 恒为 (0,0)；插件不做单显示器或区域采集，取景由宿主裁剪。
 *
 * @note 线程模型见文件头：所有函数都是静态的，Display 只在单次调用内存在。
 * @note X11 没有 Windows 那种 DPI 虚拟化：root window 的像素就是物理像素，
 *       这里不做任何缩放，也不调用 DPI 感知 API。
 */
class X11Capture {
public:
    /**
     * @brief 读默认 X screen 的几何，成功时填 width/height，origin 置 (0,0)。
     *
     * 只填几何四个字段；struct_size / pixel_format 由契约层负责（与 Windows 的
     * GdiDesktopCapture::queryGeometry 同一分工）。多显示器报合并后的整个 screen。
     *
     * @param[out] info 成功时填几何；struct_size 与 pixel_format 由调用方填。
     * @param[in]  log  宿主日志；失败原因写在这里。
     * @return 0 = 成功；1 = 当前没有可采集的屏幕（宽或高 <= 0）；
     *         3 = 无法连接 X server（DISPLAY 未设置或不可达）。
     */
    static std::int32_t queryGeometry(ShotCaptureInfo& info, shot::Logger log);

    /**
     * @brief 把整个 X screen 抓进 dst（BGRA8，尺寸必须与 target 一致）。
     *
     * 抓取顺序：MIT-SHM（XShmGetImage）→ 不可用或失败时 XGetImage。得到的 XImage
     * 按 depth / bits_per_pixel / byte_order / 通道 mask 转成 BGRA8，alpha 置 255；
     * includeCursor 为 true 时再用 XFixes 把当前光标合成到帧上。
     *
     * @param[in,out] dst           目标图；只写像素，不改结构体字段。
     * @param[in]     target        当前几何（queryGeometry 的结果），提供宽高。
     * @param[in]     includeCursor 是否合成鼠标光标；false 时完全不碰 XFixes。
     * @param[in]     log           宿主日志；失败原因写在这里。
     * @return true = 成功；false = X 系统调用失败（调用方返回 3）。
     * @warning 只写每行前 width*4 字节；dst->stride 可负、可大于 width*4。
     */
    static bool captureTo(const ShotImage& dst, const ShotCaptureInfo& target, bool includeCursor,
                          shot::Logger log);

private:
    /**
     * @brief 打开默认 X display（XOpenDisplay(nullptr)）。
     * @param[in] log 宿主日志；失败时写 "DISPLAY 未设置或 X server 不可达"。
     * @return 连接；nullptr = 打不开（调用方按返回码 3 处理）。
     */
    static Display* openDisplay(shot::Logger log);

    /**
     * @brief 关闭 X display；空指针是空操作。
     * @param[in] display 必须是本线程刚打开、尚未关闭的连接。
     */
    static void closeDisplay(Display* display);

    /**
     * @brief 按 debug 级打一行显示器布局（XRandR 1.5+；老 server 静默跳过）。
     *
     * X11 的 screen 尺寸本身不区分显示器，多屏/负坐标只能靠真机日志定位，所以这里
     * 列出每个 monitor 的像素矩形。纯诊断信息，任何失败都只跳过这一行日志。
     *
     * @param[in] display 已打开的连接。
     * @param[in] root    默认 screen 的 root window。
     * @param[in] log     宿主日志（debug 级，正常日志看不到）。
     */
    static void logMonitorLayout(Display* display, Window root, shot::Logger log);

    /**
     * @brief 抓取并转换：先试 XShm，失败回退 XGetImage。
     * @param[in]  display 已打开的连接。
     * @param[in]  root    root window。
     * @param[in]  width   像素宽（> 0）。
     * @param[in]  height  像素高（> 0）。
     * @param[in]  dst     目标图（尺寸已与 width/height 校验一致）。
     * @param[in]  log     宿主日志。
     * @return true = 已完整写入；false = 两条路径都失败（已写日志）。
     */
    static bool grabIntoBgra(Display* display, Window root, std::int32_t width,
                             std::int32_t height, const ShotImage& dst, shot::Logger log);

    /**
     * @brief 用 MIT-SHM 抓一帧并转换。
     *
     * 共享内存在 XShmAttach 成功后立刻标记 IPC_RMID，只在本函数返回前 detach/shmdt；
     * attach 失败时在清理路径标记。正常路径与失败路径都不会留下孤儿段；只有
     * shmget 与 attach 之间进程被杀这一小段窗口除外。
     *
     * @return true = 抓到并转换成功；false = 扩展不可用 / 不是本机 DISPLAY /
     *         任一 SHM 调用失败（已按需写 WARN，调用方回退 XGetImage）。
     */
    static bool grabWithShm(Display* display, Window root, std::int32_t width,
                            std::int32_t height, const ShotImage& dst, shot::Logger log);

    /**
     * @brief 用 XGetImage 抓一帧并转换（XShm 不可用时的兜底路径）。
     * @return true = 成功；false = XGetImage 返回空指针或转换失败（已写日志）。
     */
    static bool grabWithCore(Display* display, Window root, std::int32_t width,
                             std::int32_t height, const ShotImage& dst, shot::Logger log);

    /**
     * @brief 把 XImage 的像素转换成 BGRA8 写进 dst。
     *
     * 只认 ZPixmap；支持 24/32bpp 的 LSBFirst/MSBFirst，以及带 red/green/blue
     * mask 的其它常见布局（16bpp / 30-bit）。每行按 XImage 的 bytes_per_line 走，
     * 不假设紧密排列；目标按 dst->stride 行寻址，alpha 一律置 255。
     *
     * @param[in] image 已抓到的图像，data 必须有效且保持到本函数返回。
     * @param[in] dst   目标图，宽高必须与 image 相同。
     * @param[in] log   宿主日志。
     * @return true = 成功；false = 不支持的布局（已写 error，调用方返回 3）。
     */
    static bool convertToBgra(const XImage& image, const ShotImage& dst, shot::Logger log);

    /**
     * @brief 用 XFixes 取光标位图并合成进已转换好的 BGRA8 帧。
     *
     * 光标左上角 = (cursor->x - xhot, cursor->y - yhot)；XFixes 给的是预乘 alpha
     * 的 ARGB，合成公式是 out = src + dst * (1 - alpha)。越界像素直接跳过。
     * XFixes 不可用或取不到光标只写 WARN，不影响抓屏结果（与 Windows 一致）。
     *
     * @param[in]     display 已打开的连接（本函数调用完就关）。
     * @param[in,out] dst     已经写好桌面像素的帧。
     * @param[in]     log     宿主日志。
     */
    static void compositeCursor(Display* display, const ShotImage& dst, shot::Logger log);
};

}  // namespace screen_capture
