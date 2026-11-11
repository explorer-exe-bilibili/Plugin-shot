#pragma once

#include <cstdint>
#include <memory>

#include "plugin_api.h"

namespace shot {
class I18n;
}  // namespace shot

namespace region_selector {

/**
 * @brief 区域选择覆盖层窗口：自己开线程、跑 GLFW 事件循环、用 ImGui 画遮罩。
 *
 * 对应契约里的 `shot.ui_provider`：宿主只调 init / deinit / post_to_plugin，
 * 从不接触窗口句柄，也不知道本插件有几个线程。生命周期：
 *
 *   - start() 返回 true 时 UI 线程已就绪并已订阅事件；GLFW 运行时、窗口与 ImGui
 *     后端**延迟到第一次框选时**才创建（避免和 settings_ui 抢 GLFW 的全局 helper
 *     window，原因见 README §3.1）；
 *   - 收到 `capture.request`（`mode = "region"` 且没有可用的 `region` 对象）后，
 *     在鼠标所在显示器上显示无边框、置顶、整屏的覆盖层，进入框选；
 *   - 覆盖层显示后：鼠标停在某个顶层窗口上时选框吸附到该窗口（指数平滑）；单击
 *     （位移 ≤4px、按住 ≤400ms）= 直接采集该窗口；拖动（位移 >4px）= 手绘选区，
 *     拖动期间严格跟手、不做吸附与动画；
 *   - 确认时先隐藏窗口并等待遮罩从合成器消失，再发布带 `region` 的
 *     `capture.request`；Esc / 右键 / 拖动选区任一边小于 8 像素 = 取消，不发布事件；
 *   - stop() 退订事件总线、停线程、销毁自己的窗口，返回前线程已 join。
 *
 * GLFW / OpenGL / ImGui 类型全部关在 Impl 里（内部头 region_window_impl.h），
 * 公共头不包含任何图形库或平台头文件。
 *
 * 坐标语义：发布出去的 region 是**物理虚拟桌面像素**（原点 = 虚拟桌面左上角），
 * 与 shot.screen_capture 的 describe 报的 origin/size 同一坐标系。本机混合 DPI
 * 实测与换算推导见 README「坐标语义与 DPI 实测」。
 */
class RegionWindow {
public:
    /** @brief 构造空的窗口对象；此时还没有线程与窗口。 */
    RegionWindow();
    /** @brief 析构：等价于调用 stop()，不留下窗口线程。 */
    ~RegionWindow();

    /** @brief 不可拷贝：持有窗口线程与 GL 上下文。 */
    RegionWindow(const RegionWindow&) = delete;
    /** @brief 不可赋值。 */
    RegionWindow& operator=(const RegionWindow&) = delete;

    /**
     * @brief 启动 UI 线程并订阅事件总线；返回时线程已就绪（或已失败）。
     *
     * GLFW 运行时、窗口与 ImGui 后端在第一次框选时才创建：GLFW 在 Windows 上只有
     * 一个全局 helper window，它的归属线程决定 glfwPostEmptyEvent 能唤醒谁，本插件
     * 启动时不能抢在 settings_ui 之前 glfwInit（原因见 README §3.1）。
     *
     * 幂等：已在运行时直接返回 true。同时订阅事件总线主题 `capture.request`
     * （回调里只做"拷进队列 + 条件变量唤醒"）。
     *
     * @param[in] host 宿主服务表；可为 nullptr（老宿主没有服务表，覆盖层仍可
     *                  由 post_to_plugin 驱动，只是收不到总线事件）。指针必须活到
     *                  stop() 返回。
     * @param[in] i18n 覆盖层文案表；可为 nullptr（此时显示中文兜底文案）。指针必须
     *                  活到 stop() 返回：窗口线程在渲染帧里直接调用它的 tr()。
     * @return true = 线程、窗口与 ImGui 后端已就绪；false = 建线程/建窗口/初始化 GL 失败。
     * @note 建线程可能抛异常（线程资源耗尽），这里就地兜住，异常绝不越过 DLL 边界。
     */
    bool start(const ShotHostServices* host, const shot::I18n* i18n = nullptr);

    /**
     * @brief 退订事件总线、让 UI 线程退出消息循环并 join，最后销毁窗口。
     *
     * 可重复调用；返回时窗口一定已经不在了。宿主不需要、也没办法自己关窗口。
     * 正在进行的框选会被当作取消：不发布任何事件。
     */
    void stop();

    /**
     * @brief 任意线程可调：把事件排进 UI 线程队列并立即唤醒它。
     *
     * @param[in] name 事件名；按契约是编译期静态字符串，不能为空。
     * @param[in] data 不透明事件数据，可为 nullptr（按空数据入队）。
     * @param[in] len  data 的字节数；data 非空时按此长度拷走。
     * @return 0 = 已入队；1 = 窗口还没就绪或正在关闭；2 = 入队/唤醒失败；-1 = 参数非法。
     * @note 事件名与 data_len 字节会在返回前拷进队列，投递方可立即释放自己的缓冲区。
     */
    std::int32_t post(const char* name, const char* data, std::uint32_t len);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  /**< PIMPL：GLFW / OpenGL / ImGui 状态全藏在 Impl 里。 */
};

}  // namespace region_selector
