#pragma once

#include <cstdint>
#include <memory>

#include "plugin_api.h"

namespace screen_capture {

#if !defined(_WIN32)
class WaylandCapture;  // 内部实现（wayland_capture.h）：Linux/Wayland 门户 + PipeWire
#endif

/**
 * @brief 屏幕采集能力（`shot.screen_capture`）：把屏幕内容写进宿主分配的 BGRA8 图。
 *
 * 两段式：describe 只报告几何与像素格式，宿主据此自己分配缓冲；capture 再往里写像素。
 * 所以本类从不分配图像内存，也没有跨 DLL 的图像所有权问题。
 *
 * 平台实现由 CMake 按平台选中，互不参与对方编译：
 *  - Windows：GDI 抓整个虚拟桌面（screen_capture.cpp + gdi_desktop_capture.cpp）；
 *  - Linux：同一份 .so 在运行期按会话类型二选一（screen_capture_linux.cpp）——
 *      · Wayland：xdg-desktop-portal 的 ScreenCast 门户 + PipeWire 取一个输出
 *        （wayland_*.cpp）；
 *      · X11：XShmGetImage / XGetImage 抓整个 X screen（x11_*.cpp）。
 *    Wayland 一次只共享一个源，X11 是合并后的 root window，两者的 origin 都恒为
 *    (0,0)；只有 Windows 的虚拟桌面 origin 可能为负。
 *
 * @note 线程模型：宿主在采集流水线的 worker 线程上同步调用这两个函数（见
 *       document/CAPTURE.md §2.1），同一实例不会被并发调用；Windows 与 X11 实现不
 *       保存跨调用状态，Wayland 实现则在这个调用线程上等待门户授权/新帧，它的后台
 *       线程只由插件自己创建与 join。配置项（include_cursor）的状态不放在本类里，
 *       见 CaptureSettings。
 */
class ScreenCapture {
public:
    /**
     * @brief 绑定宿主服务表（只用于写日志）。
     * @param[in] host 宿主服务表；可为 nullptr（老宿主没有服务表，日志静默丢弃）。
     *                 指针必须活到本对象析构，且由宿主保证。
     */
    explicit ScreenCapture(const ShotHostServices* host) noexcept;

    /** @brief 析构：Windows 没有系统资源要收；Linux 上停流、结束门户会话并 join 自己的线程。 */
    ~ScreenCapture();

    /** @brief 不可拷贝：一个实例对应宿主插件表里的一行。 */
    ScreenCapture(const ScreenCapture&) = delete;
    /** @brief 不可赋值。 */
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    /**
     * @brief 报告可采集画面的几何与像素格式，宿主据此分配目标缓冲。
     * @param[out] out 由调用方提供并填好 struct_size 的输出结构；不能为 nullptr。
     * @return 0 = 成功；1 = 当前没有可采集的屏幕（Windows 无显示器 / 远程会话断开 /
     *         X11 screen 尺寸非法）；
     *         2 = out 为空或 struct_size 装不下完整结构（宿主与本插件的契约不同步）；
     *         3 = 采集通道不可用（Wayland 门户/PipeWire 失败、X11 连不上 X server；
     *         已通过日志写明原因）。
     * @note Windows：尺寸与 origin 直接来自 GetSystemMetrics(SM_*VIRTUALSCREEN)，是
     *       **物理像素**与系统坐标；进入时本插件显式把调用线程的 DPI 感知上下文临时
     *       提升到 PER_MONITOR_AWARE_V2（失败只 warn 并按原上下文采集），所以不依赖
     *       宿主进程是否声明 DPI aware。
     * @note Linux/Wayland：没有虚拟桌面坐标系，一次只共享一个输出，origin 恒为
     *       (0,0)；尺寸是 PipeWire 实际协商出的帧尺寸。首次调用会弹系统授权/选择框。
     * @note Linux/X11：尺寸是默认 X screen 的 DisplayWidth/DisplayHeight，多显示器
     *       由 XRandR/Xinerama 合并成一个 root window，origin 恒为 (0,0)。X11 的像素
     *       就是物理像素，不做 DPI 缩放；缺 DISPLAY / X server 不可达时返回 3。
     */
    std::int32_t describe(ShotCaptureInfo* out) const;

    /**
     * @brief 把可采集画面写进宿主提供的 BGRA8 图，可选地合成鼠标光标。
     * @param[in,out] dst 宿主填好 data/width/height/stride/channels/pixel_format 的图；
     *                 插件只能写像素、不得改结构体字段，也不能为 nullptr。
     * @param[in] includeCursor Windows/X11：是否把**当前**鼠标光标画进结果（热点与
     *                 屏幕原点都在插件内换算成图像坐标）；这是 shot.configurable 的
     *                 include_cursor，由调用方传入而不是本类保存。Wayland：该值没有
     *                 可执行语义（光标由门户/合成器决定），实现会 WARN 一次。
     * @return 0 = 成功；1 = 当前没有可采集的屏幕；2 = dst 参数不合法（空指针、
     *         不是 BGRA8 / 4 通道、stride 装不下一行、尺寸与当前画面不一致）；
     *         3 = 采集通道不可用（系统调用 / 门户 / PipeWire / X11 失败，已写日志）。
     * @note 目标尺寸必须与 describe 报的一致。describe 与 capture 是两次独立调用，
     *       中间换分辨率 / 拔显示器 / 换输出都会让两者不一致；这时返回 2，绝不按错的
     *       尺寸写像素。
     * @note Windows/X11 的光标合成是"锦上添花"：取不到光标信息只写 warn，抓屏照常成功。
     * @warning 按 dst->stride 逐行写（stride 可以为负，表示自底向上）；只写每行前
     *          width*4 字节，行尾填充保持宿主原值。
     */
    std::int32_t capture(ShotImage* dst, bool includeCursor) const;

#if !defined(_WIN32)
    /**
     * @brief 设置 Wayland 门户要共享的类型（`shot.configurable` 的 source 回灌）。
     *
     * 只影响下一次建立/重建门户会话；与当前会话的类型不一致时，采集层会在下一次
     * describe/capture 前结束旧会话并重新弹框。Windows 与 X11 实现没有这个语义
     * （X11 分支不创建 Wayland 对象，调用是空操作）。
     *
     * @param[in] window true = 共享单个窗口；false = 共享整个输出（monitor）。
     * @note 可能在宿主 main 线程（set_config）调用，采集 worker 线程读；实现用 atomic 保存。
     */
    void setCaptureWindow(bool window);
#endif

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

#if !defined(_WIN32)
    /**
     * @brief Wayland 分支的 describe：调用方已校验 out 非空且 struct_size 够长。
     * @param[out] out 输出结构，填 width/height/origin/pixel_format。
     * @return 契约返回码（0 / 3）。
     * @note 只在 m_waylandSession 为 true 时调用；m_wayland 为空（分配失败）返回 3。
     */
    std::int32_t describeWayland(ShotCaptureInfo* out) const;

    /**
     * @brief X11 分支的 describe：调用方已校验 out 非空且 struct_size 够长。
     * @param[out] out 输出结构，填整个 X screen 的 width/height、origin (0,0)。
     * @return 契约返回码（0 / 1 / 3）。
     * @note X11 每次调用自己开/关 Display，不保存跨调用状态。
     */
    std::int32_t describeX11(ShotCaptureInfo* out) const;

    /**
     * @brief Wayland 分支的 capture：调用方已校验 dst 布局。
     * @param[in,out] dst           目标图。
     * @param[in]     includeCursor 只用于记录 Wayland 的光标限制 WARN。
     * @return 契约返回码（0 / 2 / 3）。
     */
    std::int32_t captureWayland(ShotImage* dst, bool includeCursor) const;

    /**
     * @brief X11 分支的 capture：调用方已校验 dst 布局。
     * @param[in,out] dst           目标图。
     * @param[in]     includeCursor 是否用 XFixes 合成光标。
     * @return 契约返回码（0 / 1 / 2 / 3）。
     * @note 这里再查一次 screen 几何，尺寸与 dst 不一致就返回 2（与 Windows 同一语义）。
     */
    std::int32_t captureX11(ShotImage* dst, bool includeCursor) const;
#endif

    const ShotHostServices* m_host = nullptr;  /**< 宿主服务表，只用于写日志；可为 nullptr。 */
#if !defined(_WIN32)
    std::unique_ptr<WaylandCapture> m_wayland; /**< 仅 Wayland 会话创建；析构时停流、结束会话、join。 */
    bool m_waylandSession = false; /**< 构造时探测一次的会话类型；true = 走 Wayland 后端。 */
#endif
};

}  // namespace screen_capture
