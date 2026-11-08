#pragma once

// ============================================================================
//  settings_panel_impl.h —— 内部头：SettingsPanel 的 PIMPL 与设置页模型
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。按 STYLE.md §6.8，设置页的模型
//  （ConfigItem / ProviderSection / PluginRow）、内部状态（Impl）和跨 .cpp 共用的
//  纯值工具集中在这里；公共头 settings_panel.h 只留稳定的公开接口。
//
//  为什么模型类型不进公共头：它们是"声明副本 + 本地编辑缓冲 + 脏标记"的实现细节，
//  换一种渲染方式就会变；公共头只承诺 refresh / render / apply 这些行为。
//
//  定义按职责拆在四个 .cpp 里：
//    settings_model.cpp        —— 模型刷新 / 值解析 / 跨文件共用的纯值工具
//    settings_form.cpp         —— 操作条 + 配置节 + 7 种 kind 的控件
//    settings_plugin_table.cpp —— 插件管理表
//    settings_apply.cpp        —— 应用 / 取消 / 恢复默认 / 事件 / 等待超时
// ============================================================================

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"

#include "plugin_api.h"
#include "settings_panel.h"
#include "shot_plugin_helpers/shot_plugin_config.h"
#include "shot_plugin_helpers/shot_plugin_registry.h"

namespace settings_ui {

/**
 * @brief 操作条状态文本的语气：只影响颜色，不参与逻辑。
 */
enum class Tone {
    Normal,   /**< 普通提示。 */
    Success,  /**< 操作成功。 */
    Warning,  /**< 有未应用的修改 / 等待中。 */
    Error,    /**< 失败或被拒绝。 */
};

/** @brief 普通提示的文字颜色。 */
inline constexpr ImVec4 kToneNormal(0.80f, 0.82f, 0.85f, 1.0f);

/** @brief 成功提示的文字颜色。 */
inline constexpr ImVec4 kToneSuccess(0.42f, 0.78f, 0.47f, 1.0f);

/** @brief 警告提示的文字颜色。 */
inline constexpr ImVec4 kToneWarning(0.94f, 0.74f, 0.32f, 1.0f);

/** @brief 错误提示的文字颜色。 */
inline constexpr ImVec4 kToneError(0.95f, 0.40f, 0.34f, 1.0f);

/**
 * @brief 把语气映射成文字颜色。
 * @param[in] tone 语气。
 * @return 对应的 ImGui 颜色。
 */
inline ImVec4 toneColor(Tone tone) {
    switch (tone) {
        case Tone::Success:
            return kToneSuccess;
        case Tone::Warning:
            return kToneWarning;
        case Tone::Error:
            return kToneError;
        case Tone::Normal:
        default:
            return kToneNormal;
    }
}

/** @brief 一个可配置项：声明副本 + 本地编辑缓冲 + 最近一次基线值。 */
struct ConfigItem {
    shot::ConfigDesc desc;          /**< 声明副本；字符串已拷成本地副本（见 shot::ConfigDesc）。 */
    std::string value;              /**< 本地编辑缓冲（字符串形式），应用时才写回插件。 */
    std::string baseline;           /**< 最近一次成功读到/写入的值；value != baseline 即"脏"。 */
    std::string error;              /**< set_value 被拒绝时的原因；非空 = 该项标红。 */
    std::vector<std::string> enumOptions;  /**< ENUM 的候选列表（构造模型时切好）。 */
    std::string colorHex;           /**< COLOR 的十六进制编辑缓冲，与 value 同步。 */
};

/** @brief 一个配置提供者（宿主 core 或某个实现了 shot.configurable 的插件）。 */
struct ProviderSection {
    std::string id;                 /**< 提供者 id："core" 或插件 id。 */
    std::string name;               /**< 界面显示名。 */
    std::string version;            /**< 提供者版本（只显示，不参与逻辑）。 */
    std::vector<ConfigItem> items;  /**< 配置项，顺序与声明顺序一致。 */
    bool removed = false;           /**< 插件已卸载：整节灰掉、控件不可编辑。 */
};

/** @brief 插件管理表的一行：注册表快照 + 本地"想改成什么"。 */
struct PluginRow {
    shot::PluginStatus status;   /**< 注册表快照（字符串已拷成本地副本）。 */
    bool enabledNow = false;     /**< 宿主当前状态，用来判断这一行脏不脏。 */
    bool enabledPending = false; /**< 用户勾选后的目标状态，应用时才 set_enabled。 */
    std::string message;         /**< plugins.changed 带回的拒绝/失败原因；空 = 正常。 */
};

/**
 * @brief SettingsPanel 的实现：本地模型 + 内部状态 + 只在 UI 线程上用的操作。
 *
 * PIMPL，定义在内部头里：声明集中在这个结构里，定义按职责分散到多个 .cpp
 * （STYLE.md §6.7 要求类的每个具名函数都能在头文件里找到）。
 */
struct SettingsPanel::Impl {
    const ShotHostServices* host = nullptr;

    std::vector<ProviderSection> sections;         // 配置提供者，索引 0 固定是 core
    std::vector<ProviderSection> removedSections;  // 上次刷新后消失的提供者：灰掉显示一节
    std::vector<PluginRow> plugins;                // 插件管理表

    bool configUnavailable = false;    // 宿主没提供 shot.host.plugin_config（老宿主）
    bool registryUnavailable = false;  // 宿主没提供 shot.host.plugin_registry（老宿主）

    std::string statusText;  // 操作条右侧的状态文本
    Tone statusTone = Tone::Normal;

    bool waitingForPlugins = false;  // 已提交启停，等 plugins.changed 结束"应用中…"
    std::chrono::steady_clock::time_point waitDeadline;

    // ---- 模型刷新（settings_model.cpp） ----
    void refresh();
    void refreshConfig();
    void refreshPlugins();
    bool hasPendingChanges() const;

    // ---- 渲染（settings_panel.cpp 的顶层框架 + settings_form.cpp / settings_plugin_table.cpp） ----
    void render();
    void renderActionBar();
    void renderConfigSections();
    void renderProviderSection(ProviderSection& section);
    void renderConfigItem(ConfigItem& item);
    bool renderItemControl(ConfigItem& item);
    void renderPluginTable();
    void renderPluginRow(PluginRow& row);

    // ---- 提交与事件（settings_apply.cpp） ----
    void apply();
    void cancel();
    void restoreDefaults();
    void onConfigApplied();
    void onPluginsChanged(std::string_view payload);
    void setStatus(Tone tone, std::string text);
    void checkWaitTimeout();

    // ---- 跨 .cpp 共用的纯值小工具（无状态 → 静态成员；定义在 settings_model.cpp） ----
    static bool parseBoolValue(std::string_view text);
    static int parseIntValue(std::string_view text, int fallback);
    static double parseDoubleValue(std::string_view text, double fallback);
    static std::string formatIntValue(int value);
    static std::string formatDoubleValue(double value);
    static bool parseColorValue(std::string_view text, float out[4]);
    static std::string formatColorHex(const float color[4]);
    static std::vector<std::string> splitOptions(std::string_view options);
    static std::string defaultValueFor(const shot::ConfigDesc& desc);
    static void syncColorHex(ConfigItem& item);
};

}  // namespace settings_ui
