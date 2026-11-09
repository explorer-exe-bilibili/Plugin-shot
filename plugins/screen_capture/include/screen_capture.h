#pragma once

#include <cstdint>

#include "plugin_api.h"

namespace screen_capture {

/**
 * @brief 屏幕采集能力（`shot.screen_capture`）：把整个虚拟桌面写进宿主分配的 BGRA8 图。
 *
 * 两段式：describe 只报告几何与像素格式，宿主据此自己分配缓冲；capture 再往里写像素。
 * 所以本类从不分配图像内存、也不持有 GDI 句柄——Win32 细节全在内部头
 * screen_capture_impl.h 的 GdiDesktopCapture 里，这个公共头不包含 `<windows.h>`。
 *
 * v1 只支持"整个虚拟桌面"：多显示器时 width/height 是合成后的总像素尺寸，
 * origin_x/origin_y 是内容左上角在系统坐标里的位置（主屏左上角不一定是 (0,0)，
 * 左侧或上方的副屏会让 origin 为负）。
 *
 * @note 线程模型：宿主只在 main 线程上同步调用这两个函数，同一实例不会被并发调用；
 *       实现不保存任何跨调用的屏幕状态，下一次调用从头读系统状态。
 */
class ScreenCapture {
public:
    /**
     * @brief 绑定宿主服务表（只用于写日志）。
     * @param[in] host 宿主服务表；可为 nullptr（老宿主没有服务表，日志静默丢弃）。
     *                 指针必须活到本对象析构，且由宿主保证。
     */
    explicit ScreenCapture(const ShotHostServices* host) noexcept;

    /** @brief 析构：没有系统资源要收——GDI 句柄都在单次 capture 内释放。 */
    ~ScreenCapture();

    /** @brief 不可拷贝：一个实例对应宿主插件表里的一行。 */
    ScreenCapture(const ScreenCapture&) = delete;
    /** @brief 不可赋值。 */
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    /**
     * @brief 报告虚拟桌面的几何与像素格式，宿主据此分配目标缓冲。
     * @param[out] out 由调用方提供并填好 struct_size 的输出结构；不能为 nullptr。
     * @return 0 = 成功；1 = 当前没有可采集的屏幕（无显示器 / 远程会话断开）；
     *         2 = out 为空或 struct_size 装不下完整结构（宿主与本插件的契约不同步）。
     * @note 尺寸与 origin 直接来自 GetSystemMetrics(SM_*VIRTUALSCREEN)；
     *       进程的 DPI 感知级别会影响到手的是物理像素还是逻辑像素。
     */
    std::int32_t describe(ShotCaptureInfo* out) const;

    /**
     * @brief 把整个虚拟桌面写进宿主提供的 BGRA8 图。
     * @param[in,out] dst 宿主填好 data/width/height/stride/channels/pixel_format 的图；
     *                 插件只能写像素、不得改结构体字段，也不能为 nullptr。
     * @return 0 = 成功；1 = 当前没有可采集的屏幕；2 = dst 参数不合法（空指针、
     *         不是 BGRA8 / 4 通道、stride 装不下一行、尺寸与当前虚拟桌面不一致）；
     *         3 = 系统调用失败（已通过宿主日志写明是哪一步）。
     * @note 目标尺寸必须与 describe 报的一致。describe 与 capture 是两次独立调用，
     *       中间换分辨率 / 拔显示器都会让两者不一致；这时返回 2，绝不按错的尺寸写像素。
     * @warning 按 dst->stride 逐行写（stride 可以为负，表示自底向上）；只写每行前
     *          width*4 字节，行尾填充保持宿主原值。
     */
    std::int32_t capture(ShotImage* dst) const;

private:
    /**
     * @brief 检查宿主给的图像能不能装下一张 BGRA8 截图。
     *
     * 除了空指针与像素布局，还要按 |stride| >= width*4 验证一行装得下：stride 可负、
     * 也可以大于 width*4（行尾填充），只看 width/height 会把行首算错。
     *
     * @param[in] dst 宿主传进来、尚未写入的图像结构。
     * @return true = 布局合法；false = 参数非法（调用方返回 2）。
     */
    static bool isValidTarget(const ShotImage& dst);

    const ShotHostServices* m_host = nullptr;  /**< 宿主服务表，只用于写日志；可为 nullptr。 */
};

}  // namespace screen_capture
