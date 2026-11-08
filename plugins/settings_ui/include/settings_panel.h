#pragma once

#include <memory>
#include <string_view>

#include "plugin_api.h"

namespace settings_ui {

/**
 * @brief 设置页：把宿主的配置目录与插件注册表读成本地模型，并渲染 ImGui 表单。
 *
 * 数据来源是宿主服务（不直接调用其它插件）：
 *   · `shot.host.plugin_config` —— 索引 0 是宿主设置（id "core"），其余是实现了
 *     shot.configurable 的插件；声明来自 ShotConfigDesc，值一律是字符串；
 *   · `shot.host.plugin_registry` —— 已安装插件清单（含禁用/加载失败的）与异步启停。
 *
 * 编辑模型是"本地副本 + 应用/取消"（document/SETTINGS.md §4）：拖滑块只改本地值，
 * 点「应用」才逐条 set_value / set_enabled 并 save，点「取消」直接丢弃。
 *
 * @note 所有成员函数都只能在 SettingsWindow 的 UI 线程上调用：模型里存着 ImGui
 *       编辑缓冲，不是线程安全的。
 * @note 宿主服务不可用时安静降级成"空模型 + 提示文本"，不抛异常。
 * @note 实现拆在 src/settings_model.cpp（模型）、settings_form.cpp（表单）、
 *       settings_plugin_table.cpp（插件管理表）、settings_apply.cpp（提交与事件），
 *       共享的私有状态在内部头 settings_panel_impl.h。
 */
class SettingsPanel {
public:
    /** @brief 构造空的设置页模型：还没有读过宿主服务。 */
    SettingsPanel();
    /** @brief 析构：释放模型。 */
    ~SettingsPanel();

    /** @brief 不可拷贝：模型里持有编辑缓冲与宿主服务指针。 */
    SettingsPanel(const SettingsPanel&) = delete;
    /** @brief 不可赋值。 */
    SettingsPanel& operator=(const SettingsPanel&) = delete;

    /**
     * @brief 全量重读配置目录与插件注册表，重建本地模型。
     *
     * 打开窗口、`config.applied`、`plugins.changed` 都会走到这里。声明字符串由
     * 提供者插件持有，插件被热卸载后就失效，所以这里一律拷成本地副本。
     *
     * @param[in] host 宿主服务表；可为 nullptr（老宿主），此时模型置空并显示降级提示。
     */
    void refresh(const ShotHostServices* host);

    /**
     * @brief 画一帧设置页：顶部操作条 + 一页滚动的配置节与插件管理表。
     *
     * @warning 调用前必须已经 ImGui::NewFrame()；只允许 UI 线程调用。
     */
    void render();

    /**
     * @brief 提交本地修改：脏配置项逐条 set_value、变化的启停逐条 set_enabled、再 save。
     *
     * 有启停变更时进入"应用中…"，等宿主的 `plugins.changed` 到达后由
     * onPluginsChanged() 结束等待（带超时兜底）。配置项被插件拒绝时留在原地标红。
     */
    void apply();

    /** @brief 丢弃全部未应用的编辑，回到最近一次基线（不写插件、不落盘）。 */
    void cancel();

    /** @brief 把所有配置值改成声明里的默认值；仍然要用户点「应用」才写回。 */
    void restoreDefaults();

    /** @brief 收到 `config.applied`：以插件当前值为新基线重建模型。 */
    void onConfigApplied();

    /**
     * @brief 收到 `plugins.changed`：整表重读，并把负载里的拒绝/失败原因显示在对应行。
     *
     * @param[in] payload 宿主发的 JSON（格式见 document/SETTINGS.md §3.2）；可为空。
     */
    void onPluginsChanged(std::string_view payload);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  /**< PIMPL：模型细节在内部头 settings_panel_impl.h 里。 */
};

}  // namespace settings_ui
