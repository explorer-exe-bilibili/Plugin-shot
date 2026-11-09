#pragma once

#include <cstdint>
#include <memory>

#include "plugin_api.h"

namespace settings_ui {

/**
 * @brief 设置窗口：自己开线程、跑 GLFW 事件循环、用 OpenGL3 画 ImGui。
 *
 * 对应契约里的 `shot.ui_provider`：宿主只调 init / deinit / post_to_plugin，
 * 从不接触窗口句柄，也不知道本插件有几个线程。生命周期：
 *
 *   - start() 返回 true 时窗口（隐藏）与 ImGui 后端都已就绪；
 *   - 收到 `settings.open` 后显示窗口；X / Esc 只隐藏，线程与窗口保留；
 *   - stop() 退订事件总线、停线程、销毁窗口，返回前线程已 join——宿主热卸载
 *     或退出时都靠它把资源收干净。
 *
 * GLFW / OpenGL / ImGui 类型全部关在 Impl 里（定义在内部头 settings_window_impl.h），
 * 公共头不包含任何平台或图形库头文件。
 *
 * @note 实现拆在 src/settings_window.cpp（生命周期/事件循环）、
 *       settings_graphics.cpp（ImGui 后端 + 字体/DPI + 渲染）、
 *       settings_geometry.cpp（窗口几何持久化）、settings_dialog.cpp（原生文件对话框）。
 */
class SettingsWindow {
public:
    /** @brief 构造空的窗口对象；此时还没有线程与窗口。 */
    SettingsWindow();
    /** @brief 析构：等价于调用 stop()，不留下窗口线程。 */
    ~SettingsWindow();

    /** @brief 不可拷贝：持有窗口线程与 GL 上下文。 */
    SettingsWindow(const SettingsWindow&) = delete;
    /** @brief 不可赋值。 */
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    /**
     * @brief 建隐藏窗口并启动 UI 线程；返回时窗口与 ImGui 已就绪（或已失败）。
     *
     * 幂等：已在运行时直接返回 true。同时订阅事件总线主题 `settings.open` /
     * `config.applied` / `plugins.changed` / `lists.changed` / `lists.updated`
     * （回调只入队 + glfwPostEmptyEvent 唤醒）。
     *
     * @param[in] host 宿主服务表；可为 nullptr（老宿主没有服务表，窗口仍可显示，
     *                  只是配置模型为空）。指针必须活到 stop() 返回。
     * @return true = 窗口与 UI 线程已就绪；false = 建线程/建窗口/初始化 GL 失败。
     * @note 建线程可能抛异常（线程资源耗尽），这里就地兜住，异常绝不越过 DLL 边界。
     */
    bool start(const ShotHostServices* host);

    /**
     * @brief 退订事件总线、让 UI 线程退出消息循环并 join，最后销毁窗口。
     *
     * 可重复调用；返回时窗口一定已经不在了。宿主不需要、也没办法自己关窗口。
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

}  // namespace settings_ui
