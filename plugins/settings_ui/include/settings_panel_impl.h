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
//    · 插件清单 = 同一个列表服务的 plugin_list（name/description/version/state/
//      provides/requires），状态与启停仍以 shot.host.plugin_registry 为准；
//    · 写值/提交 = 事件总线上的 lists.update / lists.commit（不再有同步写接口），
//      结果由 lists.updated / config.applied 回来。
//
//  定义按职责拆在六个 .cpp 里：
//    settings_model.cpp        —— config_list JSON 解析 / 模型刷新 / 值解析 / 纯值工具
//    settings_form.cpp         —— 操作条 + 配置节 + 7 种 kind 的控件
//    settings_hotkey.cpp       —— STRING + options="hotkey" 的"按组合键录入"控件
//    settings_color.cpp        —— STRING + options="#..." 的"预设色块 + 自定义取色"控件
//    settings_plugin_table.cpp —— 插件管理页签（表格 + 描述）
//    settings_apply.cpp        —— 应用状态机 / 事件 / 等待超时
// ============================================================================

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"

#include "plugin_api.h"
#include "settings_panel.h"
#include "shot_plugin_helpers/shot_plugin_lists.h"
#include "shot_plugin_helpers/shot_plugin_registry.h"

namespace shot {

class I18n;  // 前置声明：设置页只借只读解析器，完整定义由各 .cpp 自己 include

}  // namespace shot

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
 * @brief 判断一个字符是不是 printf 的 flag。
 *
 * 只认 C 标准与 MSVC 都支持的 `-` `+` 空格 `#` `0`；认不出来的字符会落到
 * "conversion" 位置按字面比较，所以不会漏判（只是比较得更严）。
 */
inline bool isPrintfFlag(char c) {
    return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0';
}

/**
 * @brief 判断一个字符是不是 printf 的长度修饰符（`hh`/`ll` 由调用方先行处理）。
 */
inline bool isPrintfLength(char c) {
    return c == 'h' || c == 'l' || c == 'j' || c == 'z' || c == 't' || c == 'L';
}

/**
 * @brief 扫出格式串里的 printf 转换说明符序列（每个说明符不含 `%`）。
 *
 * 只做字符扫描，**绝不调用 printf 家族函数去"试一下"**：对用户可编辑的译文试跑本身
 * 就是 UB（`%n` 会往内存写、参数个数/类型不匹配会读错栈），必须先静态判断。
 *
 * 语法：`%[flags][width][.precision][length]conversion`
 *   flags      `-` `+` 空格 `#` `0`
 *   width      十进制数字或 `*`
 *   precision  `.` 后跟十进制数字或 `*`
 *   length     `hh` `h` `ll` `l` `j` `z` `t` `L`
 *   conversion 其余任意字符（与 fallback 逐字比较，认不出来的也照抄）
 * `%%` 是字面百分号，不消耗参数，不写进序列。
 *
 * @param[in]  format 格式串；nullptr 视为空串。
 * @param[out] specs  按出现顺序写入的转换说明符（如 `s`、`08d`、`*ld`）；先清空。
 * @return true = 扫到结尾；false = 无法安全比较的写法（结尾悬空 `%`、只有修饰符没有
 *         转换字符），调用方必须按"不可信"处理并退回 fallback。
 */
inline bool scanPrintfSpecs(const char* format, std::vector<std::string>& specs) {
    specs.clear();
    if (format == nullptr) {
        return true;
    }
    const std::string_view text(format);
    std::size_t index = 0;
    while (index < text.size()) {
        if (text[index] != '%') {
            ++index;
            continue;
        }
        ++index;  // 跳过 '%'
        if (index >= text.size()) {
            // 结尾一个孤零零的 '%'：printf 家族的行为未定义，按不可信处理。
            return false;
        }
        if (text[index] == '%') {
            ++index;  // 字面百分号，不消耗参数
            continue;
        }
        std::string spec;
        while (index < text.size() && isPrintfFlag(text[index])) {
            spec.push_back(text[index]);
            ++index;
        }
        if (index < text.size() && text[index] == '*') {
            spec.push_back('*');
            ++index;
        } else {
            while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                spec.push_back(text[index]);
                ++index;
            }
        }
        if (index < text.size() && text[index] == '.') {
            spec.push_back('.');
            ++index;
            if (index < text.size() && text[index] == '*') {
                spec.push_back('*');
                ++index;
            } else {
                while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                    spec.push_back(text[index]);
                    ++index;
                }
            }
        }
        // 长度修饰符：先看两字符的 hh / ll，再看单字符的 h/l/j/z/t/L。
        if (index + 1 < text.size() && ((text[index] == 'h' && text[index + 1] == 'h') ||
                                        (text[index] == 'l' && text[index + 1] == 'l'))) {
            spec.push_back(text[index]);
            spec.push_back(text[index + 1]);
            index += 2;
        } else if (index < text.size() && isPrintfLength(text[index])) {
            spec.push_back(text[index]);
            ++index;
        }
        if (index >= text.size()) {
            return false;  // 只有修饰符、没有转换字符
        }
        spec.push_back(text[index]);
        ++index;
        specs.push_back(std::move(spec));
    }
    return true;
}

/**
 * @brief 判断译文里的 printf 转换说明符序列是否与内置 fallback 完全一致。
 *
 * 只比较"参数怎么用"（个数、顺序、类型/长度/宽度），不比较文字——译文的职责就是改
 * 文字、调词序。`%n` 是写内存的转换，译文里出现一律拒绝（哪怕 fallback 也有：那是
 * 内置表自己的问题，不该被译文放大）。
 *
 * @param[in] translated 译文（来自 tr()，可能来自用户可编辑的语言包，不可信）。
 * @param[in] fallback   内置 fallback 格式串（可信，与调用点实参一一对应）。
 * @return true = 可以安全地把译文当格式串用；false = 必须退回 fallback。
 */
inline bool samePrintfFormat(const char* translated, const char* fallback) {
    if (translated == nullptr || fallback == nullptr) {
        return false;
    }
    if (std::strcmp(translated, fallback) == 0) {
        // 未翻译（tr 原样返回 fallback）是最常见的情况，直接放行。
        return true;
    }
    std::vector<std::string> translatedSpecs;
    std::vector<std::string> fallbackSpecs;
    if (!scanPrintfSpecs(translated, translatedSpecs) ||
        !scanPrintfSpecs(fallback, fallbackSpecs)) {
        return false;
    }
    for (const std::string& spec : translatedSpecs) {
        if (!spec.empty() && spec.back() == 'n') {
            return false;
        }
    }
    return translatedSpecs == fallbackSpecs;
}

/**
 * @brief 用**已经校验过**的格式串 + va_list 把文本拼出来（trFormat 的低层实现）。
 *
 * 调用方必须先拿到可信格式串（内置 fallback，或过了 samePrintfFormat 的译文）；
 * 本函数不再校验，因为参数已经按 va_list 展开，没法回头改。
 *
 * @param[in] format 可信的 printf 格式串；nullptr 时返回空串。
 * @param[in] args   与 format 的转换说明符一一对应的参数表（调用方负责 va_end）。
 * @return 拼好的文本；本插件的提示都很短，512 字节缓冲够用，截断时返回已写入的前缀，
 *         格式化失败（返回码 <= 0）时返回空串。
 */
inline std::string formatTextVa(const char* format, va_list args) {
    if (format == nullptr) {
        return std::string();
    }
    char buffer[512] = {};
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
    if (written <= 0) {
        return std::string();
    }
    return std::string(buffer);
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
    std::string options;        /**< ENUM 候选（"luma|average"）；PATH 用 "directory"/"file" 选对话框。 */
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
    std::string colorHex;   /**< COLOR 与 STRING 颜色控件的十六进制编辑缓冲，与 value 同步。 */
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
    std::string description;     /**< plugin_list 的 metadata description；读不到列表时为空。 */
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
 * @brief 一次"按组合键录入"会话（定义与实现在 settings_hotkey.cpp）。
 *
 * 只在 UI 线程访问。目标配置项用 owner + key 两个字符串标识，而不是 `ConfigItem*`：
 * 录入期间宿主可能发 `lists.changed` / `plugins.changed` 重建模型，指针会失效，字符串不会。
 */
struct HotkeyCapture {
    bool active = false;   /**< 是否有进行中的录入会话（true 时每帧渲染模态弹窗）。 */
    std::string owner;     /**< 目标配置项的提供者 id（config_list 的 owner）。 */
    std::string key;       /**< 目标配置项的配置键（config_list 的 id）。 */
    std::string hint;      /**< 弹窗内的临时提示（例如按了不受支持的键）；空 = 显示默认说明。 */
};

/**
 * @brief 一次"自定义颜色"取色会话（定义与实现在 settings_color.cpp）。
 *
 * 与 HotkeyCapture 同理，目标项用 owner + key 标识而不是 `ConfigItem*`：取色期间
 * 宿主可能发 `lists.changed` 重建模型，指针会失效。值本身不放在这里——取色窗每次
 * 都从 `findConfigItem` 拿当前项并实时写回本地编辑缓冲，提交仍由 settings_apply 负责。
 */
struct ColorCustomEdit {
    bool active = false;           /**< 是否有进行中的取色会话。 */
    bool requestOpen = false;      /**< 下一帧要 OpenPopup（弹窗按 ImGui id 打开一次即可）。 */
    std::string owner;             /**< 目标配置项的提供者 id（config_list 的 owner）。 */
    std::string key;               /**< 目标配置项的配置键（config_list 的 id）。 */
    std::string originalValue;     /**< 打开弹窗时的值；「取消」用它回滚实时预览。 */
};

/**
 * @brief PATH 配置项的"浏览…"该弹哪种对话框。
 *
 * 描述符里没有独立的"文件/目录"字段（ABI 2.0 不再为数据形状加结构体），所以约定用
 * `ShotConfigDesc::options` 表达：`"directory"` = 选目录，`"file"` = 选文件，其它/空
 * = 按当前值推断（已存在目录就选目录，否则选文件）。见 document/SETTINGS.md 的 kind 表。
 */
enum class PathPicker {
    Auto,       /**< 描述符没声明：按当前值是否为已存在目录推断。 */
    File,       /**< 描述符声明 "file"：只弹文件对话框。 */
    Directory,  /**< 描述符声明 "directory"（或 dir/folder）：只弹目录对话框。 */
};

/**
 * @brief SettingsPanel 的实现：本地模型 + 内部状态 + 只在 UI 线程上用的操作。
 *
 * PIMPL，定义在内部头里：声明集中在这个结构里，定义按职责分散到多个 .cpp
 * （STYLE.md §6.7 要求类的每个具名函数都能在头文件里找到）。
 */
struct SettingsPanel::Impl {
    const ShotHostServices* host = nullptr;
    const shot::I18n* i18n = nullptr;  // 窗口层在 start() 绑定、stop() 解除；空 = 安静降级

    std::vector<ProviderSection> sections;         // 配置提供者，渲染时 core 固定在最前
    std::vector<ProviderSection> removedSections;  // 上次刷新后消失的提供者：灰掉显示一节
    std::vector<PluginRow> plugins;                // 插件管理表
    HotkeyCapture hotkeyCapture;                   // 热键录入会话（settings_hotkey.cpp）
    ColorCustomEdit colorCustom;                   // 自定义颜色取色会话（settings_color.cpp）

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

    // ---- 多语言（settings_form.cpp 定义；i18n 为空时安静降级） ----
    /**
     * @brief 查设置页自己的文案。
     * @param[in] key      文案 key（settings_ui.*）。
     * @param[in] fallback 查不到时的中文原文；i18n 为空时直接返回它。
     * @return 当前语言文本；跨语言切换后失效，不要长期持有。
     */
    const char* tr(const char* key, const char* fallback) const;

    /**
     * @brief 把 config_list 里的 label/tooltip/ENUM 选项解析成当前语言文本。
     * @param[in] owner     贡献方 id（core 或插件 id）。
     * @param[in] textOrKey key 或字面量。
     * @return 当前语言文本；i18n 为空或查不到时原样返回 textOrKey。
     */
    const char* resolve(const char* owner, const char* textOrKey) const;

    /**
     * @brief 取译文并按 printf 格式化；译文与内置 fallback 的转换说明符不一致时退回 fallback。
     *
     * 为什么需要这一层：语言包是用户/第三方可编辑的，把 tr() 的结果直接交给 vsnprintf，
     * 只要译文多一个 `%s`、少一个 `%d` 或者写了 `%n`，就是 UB（`%n` 会往栈上写地址）。
     * 所以先按 samePrintfFormat 做纯字符校验，不一致就退回可信的中文 fallback 并记 WARN。
     *
     * @param[in] key            文案 key（settings_ui.*）。
     * @param[in] fallbackFormat 内置中文格式串，**必须与后面的实参一一对应**。
     * @param[in] ...            格式实参。
     * @return 拼好的文本；格式化失败时返回空串（调用方按空提示处理）。
     * @note 校验只能保证"译文与 fallback 一致"，纠正不了 fallback 本身的错误；新增调用点
     *       时请确认 fallbackFormat 的转换说明符与实参类型匹配。
     */
    std::string trFormat(const char* key, const char* fallbackFormat, ...) const;

    // ---- 渲染（settings_panel.cpp 的顶层框架 + settings_form.cpp / settings_plugin_table.cpp） ----
    void render();
    void renderActionBar();
    void renderConfigSections();
    void renderProviderSection(ProviderSection& section);
    void renderConfigItem(ConfigItem& item, const std::string& owner);
    bool renderItemControl(ConfigItem& item, const std::string& owner);
    /**
     * @brief 画「插件」页签：注册表不可用/没有插件时的降级文本 + 插件管理表。
     *
     * 拆成独立页签后，插件管理不再是配置流中的一节：可用性降级只影响本页，
     * 不会把「常规」页的配置区一起挤掉（反过来也一样）。
     */
    void renderPluginPage();
    void renderPluginTable();
    void renderPluginRow(PluginRow& row);

    // ---- 热键录入控件（settings_hotkey.cpp） ----
    /**
     * @brief 画热键录入的模态弹窗；没有进行中的会话时只做一次空查询。
     *
     * 在 render() 里每帧调用一次：录入会话可能在配置行的渲染路径之外被刷新/取消，
     * 弹窗必须始终能被画出来、被取消。
     */
    void renderHotkeyCapturePopup();

    /**
     * @brief 画一行热键控件：当前组合键 + 「录入…」按钮（值由弹窗写回，本函数不改值）。
     * @param[in,out] item  配置项；只读 value，不写。
     * @param[in]     owner 该配置项的提供者 id（config_list 的 owner）。
     */
    void renderHotkeyControl(ConfigItem& item, const std::string& owner);

    /**
     * @brief 开始录入：记录目标项并发布 `hotkey.suspend`（让 hotkey 插件撤掉全局注册）。
     * @param[in] owner 目标配置项的提供者 id。
     * @param[in] key   目标配置项的配置键。
     */
    void beginHotkeyCapture(const std::string& owner, const std::string& key);

    /** @brief 结束录入并发布 `hotkey.resume`（重复调用是空操作）。 */
    void endHotkeyCapture();

    /**
     * @brief 判断一个描述符是不是"热键组合"（STRING + options == "hotkey"）。
     * @param[in] desc 配置项声明。
     * @return true = 应该渲染录入控件而不是普通文本框。
     */
    static bool isHotkeyDesc(const ConfigDesc& desc);

    /**
     * @brief 扫描本帧按下的键，凑一个可写进配置的组合键。
     *
     * 主键范围与 hotkey_binding.h 的语法一致：A-Z、0-9、F1..F12、PrintScreen（小键盘数字按数字算）；
     * 修饰键从 ImGuiIO 的当前状态取，规范化顺序固定为 Ctrl/Alt/Shift/Win。
     *
     * @param[out] label       成功时写入规范化文本（如 "Ctrl+Alt+S"）。
     * @param[out] hasModifier 成功时表示是否带修饰键（裸键允许，但需要提示会拦截输入）。
     * @param[out] unsupported 本帧按了不支持的键（不是修饰键、也不是可录入的主键）。
     * @return true = 得到一个可录入的组合；false = 本帧没有可用按键。
     */
    static bool scanHotkeyPress(std::string& label, bool& hasModifier, bool& unsupported);

    // ---- 颜色控件（settings_color.cpp） ----
    /**
     * @brief 画一行颜色控件：预设色块 + 「自定义…」+ `#RRGGBB` 输入框。
     *
     * 所有改动只写本地编辑缓冲（item.value / item.colorHex），提交仍归「应用」流程。
     *
     * @param[in,out] item  配置项；只读 desc，读写 value / colorHex。
     * @param[in]     owner 该配置项的提供者 id（给取色会话记录目标用）。
     * @return true = 这一帧改了值。
     */
    bool renderColorControl(ConfigItem& item, const std::string& owner);

    /**
     * @brief 画"自定义颜色"取色弹窗；没有进行中的会话时只做一次空查询。
     *
     * 和热键弹窗一样在 render() 里每帧调用：目标行可能因为滚动 / 列表刷新而不再
     * 渲染，但取色会话必须始终能关闭。
     */
    void renderColorCustomPopup();

    /**
     * @brief 开始一次自定义取色：记录目标项与打开时的值，下一帧弹取色窗。
     * @param[in] owner 目标配置项的提供者 id。
     * @param[in] key   目标配置项的配置键。
     */
    void beginColorCustom(const std::string& owner, const std::string& key);

    /**
     * @brief 结束取色会话；revert = true 时把值回滚到打开弹窗时。
     * @param[in] revert true = 取消（回滚实时预览）。
     */
    void endColorCustom(bool revert);

    /**
     * @brief 判断一个描述符是不是"预设色块 + 自定义取色"颜色控件（STRING + options 以 '#' 开头）。
     * @param[in] desc 配置项声明。
     * @return true = 应该渲染颜色控件而不是普通文本框。
     */
    static bool isColorStringDesc(const ConfigDesc& desc);

    /**
     * @brief 解析颜色控件的值：严格的 `#RRGGBB`（大小写不敏感）→ 0..1 RGB。
     * @param[in]  text 配置值。
     * @param[out] out  成功时写入 RGB，失败时写黑色。
     * @return true = 格式合法。
     */
    static bool parseRgbColorValue(std::string_view text, float out[3]);

    /**
     * @brief 把 0..1 RGB 写成规范化的大写 `#RRGGBB`。
     * @param[in] color RGB，越界分量会被 clamp。
     * @return `#RRGGBB` 文本。
     */
    static std::string formatRgbHex(const float color[3]);

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

    /**
     * @brief 判断一个 PATH 配置项的"浏览…"该弹文件还是目录对话框。
     *
     * 先看描述符的 options 约定（"directory"/"dir"/"folder" 或 "file"）；没声明时返回
     * PathPicker::Auto，由调用方按当前值是否为已存在目录推断。
     * @param[in] desc 配置项声明。
     * @return 对话框类型；ENUM 的候选列表同样放在 options 里，但只有 PATH 会调用本函数。
     */
    static PathPicker pathPickerFor(const ConfigDesc& desc);
};

}  // namespace settings_ui
