#pragma once

#include <memory>
#include <string_view>

#include "plugin_api.h"

namespace settings_ui {

/**
 * @brief 设置页：把宿主的配置列表与插件注册表读成本地模型，并渲染 ImGui 表单。
 *
 * 数据来源是宿主服务（不直接调用其它插件）：
 *   · `shot.host.plugin_lists` —— `config_list` 是宿主聚合的 JSON 快照：按提供者
 *     （`owner`，`core` 是宿主自己）分组，每项对应一份配置声明；值一律是字符串；
 *   · `shot.host.plugin_registry` —— 已安装插件清单（含禁用/加载失败的）与异步启停。
 *
 * 编辑模型是"本地副本 + 应用/取消"（document/SETTINGS.md §4）：拖滑块只改本地值，
 * 点「应用」后先发布 `lists.update` 写入、再发布 `lists.commit` 落盘，结果由
 * `lists.updated` / `config.applied` 通知回来；插件启停走注册表，结果由
 * `plugins.changed` 通知回来。点「取消」直接丢弃本地修改。
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
     * @brief 全量重读配置列表与插件注册表，重建本地模型。
     *
     * 打开窗口、`settings.open` 都会走到这里；刷新时按 owner+id 合并回尚未应用的
     * 本地修改（见 mergeDirtyValues），不会把用户编了一半的值冲掉。
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
     * @brief 提交本地修改：发布 lists.update 写入脏配置项、lists.commit 落盘，
     *        再登记插件启停。
     *
     * 提交是事件驱动的（document/LISTS.md）：写值结果由 onListsUpdated() 处理，
     * 落盘结果由 onConfigApplied() 结束，启停由 onPluginsChanged() 结束；每一步都有
     * 超时兜底。被拒绝的配置项留在原地标红并保持脏标记，不中断其它项的提交。
     */
    void apply();

    /** @brief 丢弃全部未应用的编辑，回到最近一次基线（不写宿主、不落盘）。 */
    void cancel();

    /** @brief 把所有配置值改成声明里的默认值；仍然要用户点「应用」才写回。 */
    void restoreDefaults();

    /** @brief 收到 `config.applied`：结束保存等待，并以宿主当前值为新基线刷新配置区。 */
    void onConfigApplied();

    /**
     * @brief 收到 `plugins.changed`：整表重读，并把负载里的拒绝/失败原因显示在对应行。
     *
     * @param[in] payload 宿主发的 JSON（格式见 document/SETTINGS.md §3.2）；可为空。
     */
    void onPluginsChanged(std::string_view payload);

    /**
     * @brief 收到 `lists.changed`：`list` 是 config_list 时重建配置区（保留本地修改）。
     *
     * @param[in] payload JSON `{"list":"config_list"}`；为空时按配置列表处理。
     */
    void onListsChanged(std::string_view payload);

    /**
     * @brief 收到 `lists.updated`：按同 tag 匹配本次提交，逐项更新基线 / 标红被拒绝项，
     *        然后进入 lists.commit 阶段。
     *
     * @param[in] payload JSON `{"list":"config_list","tag":"...","results":[...]}`；
     *                    保存失败时是 `{"tag":"...","error":"..."}`。
     */
    void onListsUpdated(std::string_view payload);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  /**< PIMPL：模型细节在内部头 settings_panel_impl.h 里。 */
};

}  // namespace settings_ui
