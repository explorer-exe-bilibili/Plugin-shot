#pragma once

// ============================================================================
//  settings_panel_impl.h —— 内部头：SettingsPanel 的 PIMPL 与设置页模型
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。按 STYLE.md §6.8，设置页的模型
//  （ConfigDesc / ConfigItem / ProviderSection / PluginRow）、内部状态（Impl）和跨 .cpp
//  共用的纯值工具集中在这里；公共头 settings_panel.h 只留稳定的公开接口。
//
//  为什么模型类型不进公共头：它们是"声明副本 + 本地编辑缓冲 + 脏标记"的实现细节，
//  换一种渲染方式就会变；公共头只承诺 refresh / render / apply 这些行为。
//
//  数据来源（ABI 2.0 起，旧的 shot.host.plugin_config 已删除）：
//    · 配置项 = 宿主列表服务 shot.host.plugin_lists 的 config_list JSON 快照，
//      条目 schema 见 document/LISTS.md；本头的 ConfigDesc 是它的一份本地副本；
//    · 插件启停 = shot.host.plugin_registry（同步快照 + 异步登记）；
//    · 写值/提交 = 事件总线上的 lists.update / lists.commit（不再有同步写接口），
//      结果由 lists.updated / config.applied 回来。
//
//  定义按职责拆在四个 .cpp 里：
//    settings_model.cpp        —— config_list JSON 解析 / 模型刷新 / 值解析 / 纯值工具
//    settings_form.cpp         —— 操作条 + 配置节 + 7 种 kind 的控件
//    settings_plugin_table.cpp —— 插件管理表
//    settings_apply.cpp        —— 应用状态机 / 事件 / 等待超时
// ============================================================================

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"

#include "plugin_api.h"
#include "settings_panel.h"
#include "shot_plugin_helpers/shot_plugin_lists.h"
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

/**
 * @brief 一项配置的声明副本：来自 config_list 条目，字符串已拷成本地副本。
 *
 * 宿主返回的是一段 JSON 文本（`shot::PluginLists::json` 的副本），解析后把每个字段
 * 拷成 std::string 留在模型里；之后宿主重建列表、插件被热卸载都不会让这里的字段悬空。
 */
struct ConfigDesc {
    std::uint32_t kind = 0;     /**< SHOT_CONFIG_KIND_*；0 / 未知值渲染成"不支持的配置类型"。 */
    std::string key;            /**< 配置 id（config_list 的 "id"）；存盘键是 <owner>/<id>。 */
    std::string label;          /**< 界面显示名；缺失时用 id 兜底。 */
    std::string defaultValue;   /**< 默认值（字符串形式），没有默认值时为空串。 */
    std::string options;        /**< ENUM 候选，形如 "luma|average"；其它类型为空串。 */
    double min = 0.0;           /**< INT / FLOAT 下界；其它类型忽略。 */
    double max = 0.0;           /**< INT / FLOAT 上界；其它类型忽略。 */
    double step = 0.0;          /**< INT / FLOAT 步长；其它类型忽略。 */
    std::string tooltip;        /**< 悬停提示，可为空串。 */
};

/** @brief 一个可配置项：声明副本 + 本地编辑缓冲 + 最近一次基线值。 */
struct ConfigItem {
    ConfigDesc desc;        /**< 声明副本（来自 config_list）。 */
    std::string value;      /**< 本地编辑缓冲（字符串形式），应用时才提交。 */
    std::string baseline;   /**< 最近一次被宿主确认/读到的值；value != baseline 即"脏"。 */
    std::string error;      /**< 写入失败（lists.updated 的 reason）时的原因；非空 = 该项标红。 */
    std::string hint;       /**< 控件级临时提示（如文件对话框不可用）；空 = 无。 */
    std::vector<std::string> enumOptions;  /**< ENUM 的候选列表（读取模型时切好）。 */
    std::string colorHex;   /**< COLOR 的十六进制编辑缓冲，与 value 同步。 */
};

/** @brief 一个配置提供者（宿主 core 或某个实现了 shot.configurable 的插件）。 */
struct ProviderSection {
    std::string id;                 /**< 提供者 id（config_list 的 owner）："core" 或插件 id。 */
    std::string name;               /**< 界面显示名（config_list 的 owner_name）。 */
    std::vector<ConfigItem> items;  /**< 配置项，顺序与宿主返回顺序一致。 */
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
 * @brief 事件驱动提交的阶段：Idle 之外都算"应用中…"，操作条据此置灰并显示阶段文本。
 *
 * 旧实现是同步 set_value + save；ABI 2.0 起写值/提交都是事件，所以要有一个能跨
 * 多个事件回调存活的显式阶段（回调都在 UI 线程 drain，不需要锁）。
 */
enum class ApplyPhase {
    Idle,            /**< 没有进行中的提交。 */
    WaitingUpdate,   /**< 已发 lists.update，等 lists.updated（同 tag）。 */
    WaitingCommit,   /**< 已发 lists.commit，等 config.applied（失败时回落成 lists.updated）。 */
    WaitingPlugins,  /**< 已登记 set_enabled，等 plugins.changed。 */
};

/** @brief 一次 lists.update 提交的配置项快照：结果回来后按"提交值"更新基线。 */
struct PendingConfigChange {
    std::string owner;  /**< 提供者 id（core 或插件 id）。 */
    std::string key;    /**< 配置 id。 */
    std::string value;  /**< 提交的值；这是基线应该变成的值，不要用当前编辑缓冲代替。 */
};

/**
 * @brief SettingsPanel 的实现：本地模型 + 内部状态 + 只在 UI 线程上用的操作。
 *
 * PIMPL，定义在内部头里：声明集中在这个结构里，定义按职责分散到多个 .cpp
 * （STYLE.md §6.7 要求类的每个具名函数都能在头文件里找到）。
 */
struct SettingsPanel::Impl {
    const ShotHostServices* host = nullptr;

    std::vector<ProviderSection> sections;         // 配置提供者，渲染时 core 固定在最前
    std::vector<ProviderSection> removedSections;  // 上次刷新后消失的提供者：灰掉显示一节
    std::vector<PluginRow> plugins;                // 插件管理表

    bool configUnavailable = false;    // 读不到 config_list（老宿主 / 列表服务缺失 / JSON 坏了）
    std::string configUnavailableReason;  // 降级原因文本，直接显示在配置区顶部
    bool registryUnavailable = false;  // 宿主没提供 shot.host.plugin_registry（老宿主）

    std::string statusText;  // 操作条右侧的状态文本
    Tone statusTone = Tone::Normal;

    // ---- 事件驱动提交的状态机（settings_apply.cpp） ----
    ApplyPhase applyPhase = ApplyPhase::Idle;
    std::chrono::steady_clock::time_point waitDeadline;
    std::string pendingTag;                       // 本次 lists.update/commit 的 tag（回显匹配）
    std::uint64_t nextTag = 0;                    // 自增序号：同一插件实例内唯一
    std::vector<PendingConfigChange> pendingChanges;  // 等 lists.updated 时用来回填基线
    int applyConfigTotal = 0;                     // 本次提交的配置项数
    int applyConfigApplied = 0;                   // 宿主确认成功的项数
    int applyConfigRejected = 0;                  // 被拒绝的项数
    bool applySaveFailed = false;                 // commit 失败/超时：值可能生效但没落盘
    int applyPluginChanged = 0;                   // 已登记的插件启停数
    int applyPluginRejected = 0;                  // 被注册表拒绝的启停数

    // ---- 模型刷新（settings_model.cpp） ----
    void refresh();
    void refreshConfig();
    void refreshPlugins();
    bool hasPendingChanges() const;
    bool hasPendingConfigChanges() const;
    bool hasPendingPluginChanges() const;

    /**
     * @brief 把 config_list 的 JSON 文本解析成分组模型（纯函数，不做 I/O）。
     * @param[in]  text config_list 的 JSON 数组文本。
     * @param[out] out  解析结果；缺 owner/id 的条目跳过，未知 kind 保留给渲染层显示"不支持"。
     * @return true = 是合法 JSON 数组；false = 空串 / 语法错误 / 不是数组（out 已清空）。
     */
    static bool parseConfigListJson(std::string_view text, std::vector<ProviderSection>& out);

    /**
     * @brief 把上次模型里"用户改过 / 被拒绝过"的本地状态合并回新模型。
     *
     * 刷新时不能丢掉尚未应用的编辑（宿主在任意时刻可能发 lists.changed / config.applied）；
     * 基线用新模型从 config_list 读到的最新值，本地编辑缓冲与错误标记从旧模型搬过来。
     *
     * @param[in]     previous 刷新前的模型（可含 removed 节）。
     * @param[in,out] sections 刚由 config_list 重建的模型。
     */
    static void mergeDirtyValues(const std::vector<ProviderSection>& previous,
                                 std::vector<ProviderSection>& sections);

    /** @brief 按 owner + id 找配置项；找不到返回 nullptr。 */
    ConfigItem* findConfigItem(const std::string& owner, const std::string& key);

    /** @brief 找本次提交里 (owner, id) 的提交值；结果匹配基线时用，找不到返回 nullptr。 */
    const std::string* findPendingChangeValue(const std::string& owner,
                                              const std::string& key) const;

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
    void onListsChanged(std::string_view payload);
    void onListsUpdated(std::string_view payload);
    void setStatus(Tone tone, std::string text);
    void checkWaitTimeout();

    /** @brief 是否有进行中的提交（任一等待阶段）。 */
    bool isBusy() const;

    /** @brief 操作条在"应用中…"时显示的阶段文本。 */
    std::string busyText() const;

    // 状态机的分段（与 apply() 一起构成一次完整提交）。
    void beginApplyConfig();    // 收集脏项 → 发 lists.update → 等 lists.updated
    void beginApplyCommit();    // lists.updated 处理完 → 发 lists.commit → 等 config.applied
    void finishApplyConfig();   // 配置阶段收尾：刷新配置区 → 有启停要登记就继续，否则报告结果
    void beginApplyPlugins();   // 登记 set_enabled → 等 plugins.changed
    void resetApplyState();     // 回到 Idle（不动本次提交的计数器，报告还要用）
    void reportApplyResult();   // 按本次提交的计数器写操作条状态 + 记一条日志

    /**
     * @brief 用 shot::EventBus 发一条事件（老宿主没有总线时返回 false）。
     * @param[in] host    宿主服务表，可为 nullptr。
     * @param[in] topic   事件主题。
     * @param[in] payload JSON 文本负载。
     * @return true = 已入队；false = 总线不可用 / 队列满 / 负载超限。
     */
    static bool publishEvent(const ShotHostServices* host, const char* topic,
                             std::string_view payload);

    // ---- 跨 .cpp 共用的纯值小工具（无状态 → 静态成员；定义在 settings_model.cpp） ----
    static bool parseBoolValue(std::string_view text);
    static int parseIntValue(std::string_view text, int fallback);
    static double parseDoubleValue(std::string_view text, double fallback);
    static std::string formatIntValue(int value);
    static std::string formatDoubleValue(double value);
    static bool parseColorValue(std::string_view text, float out[4]);
    static std::string formatColorHex(const float color[4]);
    static std::vector<std::string> splitOptions(std::string_view options);
    static std::string defaultValueFor(const ConfigDesc& desc);
    static void syncColorHex(ConfigItem& item);
};

}  // namespace settings_ui
