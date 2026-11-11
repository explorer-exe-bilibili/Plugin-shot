#include "hostI18n.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ============================================================================
//  hostI18nText.cpp —— 宿主文案表、统一回退链与 Message
//
//  两张静态表（清单内容见各自 banner）：
//    · displayTexts：会被发布进 i18n_list 的 key -> 文本；
//    · 动态消息表 kMessages：带 {0}/{1} 参数、只有宿主知道参数的文案。
//
//  回退链是**唯一实现**（document/I18N.md §3.2）：完全匹配 -> 该语言的 fallback
//  -> 主语言族（zh-TW 命中 zh-CN）-> 默认语言 en-US -> en 语言族 -> 第一条非空
//  文本。插件侧 shot::I18n 的 select 必须与这里一致，两边注释互相点名。
//
//  空串视为缺失：作者常用空串"占位"（某个语种还没翻），不能把它当译文返回。
//
//  注意：语言目录（内置注册表 ∪ 语言包声明）在 hostI18n.cpp；本文件只按传入的
//  语言标签选文本，不在选文本时回头读当前语言（调用方必须一次取一次快照，
//  否则语言切换的瞬间 @meta 与条目文本可能来自两种语言）。
// ============================================================================

namespace {

// ---- 会发布进 i18n_list 的宿主文案 ----------------------------------------
// id 的两类用法：
//   · 数据列表里的 key（tray_command_list 的 label、config_list 的 label/tooltip）；
//   · ENUM 候选值（core 的语言与日志级别），设置页按 display(owner, value) 解析。
// 每条文案的 texts 是"语言标签 -> 文本"的**任意长度**列表：加语种只在这里加一项
// （以及各插件语言表），不需要改代码。顺序只用于"所有语言都缺"时的第一条兜底。
// core 的 owner_name 也走这里：设置页把 owner_name 当显示名，不解析的话英文界面
// 会剩一个"宿主"。
const std::vector<Host::I18n::CoreText> kDisplayTexts = {
    {"core.owner_name", {{"zh-CN", "宿主"}, {"en-US", "Host"}}},
    {"capture.request", {{"zh-CN", "截屏"}, {"en-US", "Capture"}}},
    {"region.capture", {{"zh-CN", "区域截屏"}, {"en-US", "Region capture"}}},
    {"settings.open", {{"zh-CN", "打开设置"}, {"en-US", "Open settings"}}},
    {"app.quit", {{"zh-CN", "退出"}, {"en-US", "Quit"}}},
    {"config.language.label", {{"zh-CN", "界面语言"}, {"en-US", "Language"}}},
    {"config.language.tooltip", {{"zh-CN", "auto = 跟随系统；切换后界面立即刷新"}, {"en-US", "Auto follows the system; the UI refreshes immediately"}}},
    {"config.logging.level.label", {{"zh-CN", "日志级别"}, {"en-US", "Log level"}}},
    {"config.logging.level.tooltip", {{"zh-CN", "host / plugin 两个 logger 一起改"}, {"en-US", "Applies to both the host and plugin loggers"}}},
    {"config.debug.console.label", {{"zh-CN", "控制台日志"}, {"en-US", "Console log"}}},
    {"config.debug.console.tooltip", {{"zh-CN", "关闭后控制台不再输出，文件日志不受影响"}, {"en-US", "Console output stops when off; the log file is unaffected"}}},
    {"config.logging.file.label", {{"zh-CN", "文件日志"}, {"en-US", "Log file"}}},
    {"config.logging.file.tooltip", {{"zh-CN", "写入 <数据目录>/logs/host.log；文件打不开时恒为关闭"}, {"en-US", "Writes to <data dir>/logs/host.log; stays off if the file cannot be opened"}}},
    // 语言标签的"原生显示名"不在这里：由语言目录提供、HostListCatalog 生成
    // i18n_list 时投影成 core 条目。只有 auto 不是语言标签，需要本地化文本。
    {"auto", {{"zh-CN", "跟随系统"}, {"en-US", "Follow system"}}},
    // 语言下拉里"这个语种还没有译文"的后缀：由宿主拼在显示名后面（目录里的 tag
    // 有没有任何文本，见 HostListCatalog 的 translated 判断）。中文用全角括号、
    // 英文自带前导空格，两种情况拼起来都不需要调用方再补空格。
    {"language.untranslated", {{"zh-CN", "（未翻译）"}, {"en-US", " (untranslated)"}}},
    {"debug", {{"zh-CN", "调试"}, {"en-US", "Debug"}}},
    {"info", {{"zh-CN", "信息"}, {"en-US", "Info"}}},
    {"warn", {{"zh-CN", "警告"}, {"en-US", "Warning"}}},
    {"error", {{"zh-CN", "错误"}, {"en-US", "Error"}}},
};

// ---- 宿主生成的动态消息（带模板参数）-------------------------------------
// 这些字符串会被 settings_ui / notify 原样展示，必须按当前语言生成；内部日志
// 用 Message::chinese() 固定中文（日志不翻译，e2e 按中文日志断言）。
// texts 同样是任意语言列表，加语种加一项即可。
const std::vector<Host::I18n::CoreText> kMessages = {
    {"plugin.error.no_export", {{"zh-CN", "没有 shot_module_entry 导出"}, {"en-US", "Missing shot_module_entry export"}}},
    {"plugin.error.empty_module", {{"zh-CN", "shot_module_entry 返回空"}, {"en-US", "shot_module_entry returned null"}}},
    {"plugin.error.module_too_short", {{"zh-CN", "模块表太短（{0} 字节）"}, {"en-US", "Module table too short ({0} bytes)"}}},
    {"plugin.error.abi_major", {{"zh-CN", "abi major 不兼容（插件 {0}，宿主 {1}）"}, {"en-US", "Incompatible abi major (plugin {0}, host {1})"}}},
    {"plugin.error.missing_create_destroy", {{"zh-CN", "缺少 create/destroy"}, {"en-US", "Missing create/destroy"}}},
    {"plugin.error.init_rejected", {{"zh-CN", "init 拒绝了本宿主（{0}）"}, {"en-US", "init rejected this host ({0})"}}},
    {"plugin.error.dep_syntax", {{"zh-CN", "依赖语法错误：{0}"}, {"en-US", "Invalid requirement syntax: {0}"}}},
    {"plugin.error.core_version", {{"zh-CN", "需要 {0}，宿主是 {1}"}, {"en-US", "Requires {0}, host is {1}"}}},
    {"plugin.error.no_provider", {{"zh-CN", "没有提供者满足 {0}"}, {"en-US", "No provider satisfies {0}"}}},
    {"plugin.error.provider_skipped", {{"zh-CN", "提供者 {0} 被跳过了"}, {"en-US", "Provider {0} was skipped"}}},
    {"plugin.error.provider_no_version", {{"zh-CN", "提供者 {0} 没有报告版本"}, {"en-US", "Provider {0} reported no version"}}},
    {"plugin.error.version_mismatch", {{"zh-CN", "要求 {0}，实际提供 {1}"}, {"en-US", "Requires {0}, actually provided {1}"}}},
    {"plugin.error.dll_load", {{"zh-CN", "DLL 加载失败（{0}）"}, {"en-US", "Failed to load DLL ({0})"}}},
    {"plugin.error.create_null", {{"zh-CN", "create 返回空"}, {"en-US", "create returned null"}}},
    {"plugin.error.ui_init", {{"zh-CN", "UIProvider::init 失败"}, {"en-US", "UIProvider::init failed"}}},
    {"plugin.error.no_metadata", {{"zh-CN", "没有 metadata 且没有本宿主认识的任何能力"}, {"en-US", "No metadata and no capability this host understands"}}},
    {"plugin.error.empty_id", {{"zh-CN", "插件 id 为空"}, {"en-US", "Plugin id is empty"}}},
    {"plugin.error.no_such_plugin", {{"zh-CN", "没有这个插件"}, {"en-US", "No such plugin"}}},
    {"plugin.error.required_by", {{"zh-CN", "被 {0} 依赖"}, {"en-US", "Required by {0}"}}},
    {"plugin.error.dependency_enable_failed", {{"zh-CN", "依赖 {0} 启用失败：{1}"}, {"en-US", "Failed to enable dependency {0}: {1}"}}},
    {"plugin.state.not_loaded", {{"zh-CN", "未加载"}, {"en-US", "Not loaded"}}},
    {"plugin.state.disabled", {{"zh-CN", "已在配置中禁用"}, {"en-US", "Disabled in configuration"}}},
    {"capture.error.host_shutting_down", {{"zh-CN", "宿主正在退出，请求被丢弃"}, {"en-US", "Host is shutting down; request dropped"}}},
    {"capture.error.no_capture_plugin", {{"zh-CN", "没有可用的屏幕采集插件"}, {"en-US", "No screen capture plugin available"}}},
    {"capture.error.describe_failed", {{"zh-CN", "describe 失败（返回 {0}）"}, {"en-US", "describe failed (returned {0})"}}},
    {"capture.error.invalid_size", {{"zh-CN", "describe 返回的尺寸非法（{0}x{1}）"}, {"en-US", "describe returned an invalid size ({0}x{1})"}}},
    {"capture.error.unsupported_pixel_format", {{"zh-CN", "不支持的像素格式（{0}）"}, {"en-US", "Unsupported pixel format ({0})"}}},
    {"capture.error.too_large", {{"zh-CN", "采集尺寸过大（{0} 字节）"}, {"en-US", "Capture size too large ({0} bytes)"}}},
    {"capture.error.capture_failed", {{"zh-CN", "capture 失败（返回 {0}）"}, {"en-US", "capture failed (returned {0})"}}},
    {"capture.error.no_saver_plugin", {{"zh-CN", "没有可用的图像保存插件"}, {"en-US", "No image saver plugin available"}}},
    {"capture.error.save_failed", {{"zh-CN", "save 失败（返回 {0}）"}, {"en-US", "save failed (returned {0})"}}},
    {"list.error.bad_payload", {{"zh-CN", "负载不是合法 JSON 对象"}, {"en-US", "Payload is not a valid JSON object"}}},
    {"list.error.update_only_config", {{"zh-CN", "lists.update 只支持 config_list"}, {"en-US", "lists.update only supports config_list"}}},
    {"list.error.commit_only_config", {{"zh-CN", "lists.commit 只支持 config_list"}, {"en-US", "lists.commit only supports config_list"}}},
    {"list.error.changes_array", {{"zh-CN", "changes 必须是数组"}, {"en-US", "changes must be an array"}}},
    {"list.error.bad_param", {{"zh-CN", "参数非法：owner/id/value 必须都是字符串"}, {"en-US", "Invalid parameter: owner/id/value must be strings"}}},
    {"list.error.provider_unavailable", {{"zh-CN", "提供者未加载或不支持 shot.configurable"}, {"en-US", "Provider not loaded or does not support shot.configurable"}}},
    {"list.error.plugin_rejected", {{"zh-CN", "插件拒绝（返回码 {0}）"}, {"en-US", "Plugin rejected (returned {0})"}}},
    {"list.error.save_failed", {{"zh-CN", "保存失败"}, {"en-US", "Failed to save"}}},
};

/**
 * @brief 在一张文案表里按 id 找表项（线性查找：两张表合起来几十条，够用）。
 * @param[in] table 文案表。
 * @param[in] id    文本 id；nullptr 时返回 nullptr。
 * @return 表项；没有时返回 nullptr。
 */
const Host::I18n::CoreText* findInTable(const std::vector<Host::I18n::CoreText>& table,
                                        const char* id) {
    if (id == nullptr) {
        return nullptr;
    }
    for (const Host::I18n::CoreText& entry : table) {
        if (entry.id != nullptr && std::strcmp(entry.id, id) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

/**
 * @brief 替换模板里的 {0} / {1} 占位符。
 * @param[in] text 模板文本。
 * @param[in] args 参数；缺失的参数保持原占位符（便于发现漏传参数）。
 * @return 替换后的文本。
 */
std::string substitutePlaceholders(const std::string& text,
                                   const std::array<std::string, 2>& args) {
    std::string out = text;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index].empty()) {
            continue;
        }
        const std::string placeholder = "{" + std::to_string(index) + "}";
        std::string::size_type position = 0;
        while ((position = out.find(placeholder, position)) != std::string::npos) {
            out.replace(position, placeholder.size(), args[index]);
            position += args[index].size();
        }
    }
    return out;
}

// ---- 统一回退链 -----------------------------------------------------------
// 一步 = 一个候选标签 + 是否只按主语言族匹配。链的顺序就是 document/I18N.md
// §3.2 的顺序（宿主与插件 helper 必须一致，shot_i18n.h 的 select 注释点名这里）：
//   1 归一化标签完全匹配；
//   2 该语言的 fallback（目录声明/注册表给出；空串表示没配，跳过）；
//   3 主语言族（zh-TW 没有 zh-TW 文本时命中 zh-CN）；
//   4 默认语言 en-US；
//   5 en 语言族（en-GB 之类的表也能兜住）；
//   6 第一条非空文本（由 selectText / pickFromTexts 的尾部循环做）；
//   7 调用方显示 key 原文（消费方的事，不在本类）。
struct FallbackStep {
    std::string tag;        /**< 候选标签（已规范化） */
    bool familyOnly = false;/**< true = 只按主语言族匹配 */
};

/**
 * @brief 构造回退链的候选步骤（去重、去空）。
 * @param[in] language    目标语言。
 * @param[in] fallbackTag 该语言的 fallback；空串表示没有配置。
 * @return 有序的候选步骤。
 */
std::vector<FallbackStep> fallbackChain(std::string_view language, std::string_view fallbackTag) {
    std::vector<FallbackStep> chain;
    const std::string normalized = Host::I18n::normalizeTag(language);
    const std::string fallback = Host::I18n::normalizeTag(fallbackTag);
    const std::string defaultTag = Host::I18n::defaultLanguage();
    const std::string defaultPrimary = Host::I18n::primaryTag(defaultTag);
    const auto push = [&chain](std::string tag, bool familyOnly) {
        if (tag.empty()) {
            return;
        }
        for (const FallbackStep& step : chain) {
            if (step.tag == tag && step.familyOnly == familyOnly) {
                return;
            }
        }
        chain.push_back(FallbackStep{std::move(tag), familyOnly});
    };
    push(normalized, false);
    push(fallback, false);
    push(normalized, true);
    push(fallback, true);
    push(defaultTag, false);
    push(defaultPrimary, true);
    return chain;
}

/**
 * @brief 在"标签 -> 文本"映射上匹配一步（空串视为缺失）。
 * @param[in]  texts      标签 -> 文本；标签可以是任意写法。
 * @param[in]  tag        候选标签（已规范化）。
 * @param[in]  familyOnly true = 只要求主语言相同；false = 完全匹配。
 * @param[out] out        命中时写入文本。
 * @return true = 命中。
 */
bool matchInMap(const std::map<std::string, std::string>& texts, std::string_view tag,
                bool familyOnly, std::string& out) {
    const std::string normalized = Host::I18n::normalizeTag(tag);
    if (normalized.empty()) {
        return false;
    }
    const std::string primary = Host::I18n::primaryTag(normalized);
    for (const auto& [key, value] : texts) {
        if (value.empty()) {
            continue;  // 空串 = 作者留的占位，当作缺失
        }
        if (familyOnly ? (Host::I18n::primaryTag(key) == primary)
                       : (Host::I18n::normalizeTag(key) == normalized)) {
            out = value;
            return true;
        }
    }
    return false;
}

/**
 * @brief 在"语言列表"（静态表）上匹配一步；语义同 matchInMap。
 * @param[in] texts      语言列表；标签可以是任意写法。
 * @param[in] tag        候选标签（已规范化）。
 * @param[in] familyOnly true = 只要求主语言相同。
 * @return 命中时返回表内文本指针；否则 nullptr。
 */
const char* matchInTexts(const std::vector<Host::I18n::LanguageText>& texts,
                         std::string_view tag, bool familyOnly) {
    const std::string normalized = Host::I18n::normalizeTag(tag);
    if (normalized.empty()) {
        return nullptr;
    }
    const std::string primary = Host::I18n::primaryTag(normalized);
    for (const Host::I18n::LanguageText& text : texts) {
        if (text.tag == nullptr || text.text == nullptr || text.text[0] == '\0') {
            continue;  // 空串视为缺失
        }
        if (familyOnly ? (Host::I18n::primaryTag(text.tag) == primary)
                       : (Host::I18n::normalizeTag(text.tag) == normalized)) {
            return text.text;
        }
    }
    return nullptr;
}

}  // namespace

namespace Host {

const I18n::CoreText* I18n::findText(const char* id) {
    const CoreText* entry = findInTable(kDisplayTexts, id);
    return (entry != nullptr) ? entry : findInTable(kMessages, id);
}

std::string I18n::selectText(const std::map<std::string, std::string>& texts,
                             std::string_view language, std::string_view fallbackTag) {
    if (texts.empty()) {
        return {};
    }
    const std::vector<FallbackStep> chain = fallbackChain(language, fallbackTag);
    std::string out;
    for (const FallbackStep& step : chain) {
        if (matchInMap(texts, step.tag, step.familyOnly, out)) {
            return out;
        }
    }
    // 第 6 步：所有语言都缺，取第一条非空文本（映射的键顺序，稳定可预期）。
    for (const auto& [key, value] : texts) {
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

std::string I18n::pickText(const std::map<std::string, std::string>& texts) const {
    return pickTextIn(currentLanguage(), texts);
}

std::string I18n::pickTextIn(std::string_view language,
                             const std::map<std::string, std::string>& texts) const {
    if (texts.empty()) {
        return {};
    }
    const std::string target = language.empty() ? currentLanguage() : std::string(language);
    return selectText(texts, target, fallbackTag(target));
}

const char* I18n::pickFromTexts(const std::vector<LanguageText>& texts,
                                std::string_view language, std::string_view fallbackTag) {
    if (texts.empty()) {
        return nullptr;
    }
    const std::vector<FallbackStep> chain = fallbackChain(language, fallbackTag);
    for (const FallbackStep& step : chain) {
        if (const char* text = matchInTexts(texts, step.tag, step.familyOnly)) {
            return text;
        }
    }
    // 所有语言都缺：取第一条可用文本（作者的表顺序）。
    for (const LanguageText& text : texts) {
        if (text.text != nullptr && text.text[0] != '\0') {
            return text.text;
        }
    }
    return nullptr;
}

const char* I18n::text(const char* id) const {
    const CoreText* entry = findText(id);
    if (entry == nullptr) {
        return id;
    }
    // 语言只取一次：上下两行之间若被 main 线程切了语言，也不能用新语言去配旧链。
    const std::string language = currentLanguage();
    if (const char* text = pickFromTexts(entry->texts, language, fallbackTag(language))) {
        return text;
    }
    return id;
}

std::string I18n::textIn(std::string_view language, const char* id) const {
    const CoreText* entry = findText(id);
    if (entry == nullptr) {
        return (id != nullptr) ? std::string(id) : std::string();
    }
    const std::string target = language.empty() ? currentLanguage() : std::string(language);
    if (const char* text = pickFromTexts(entry->texts, target, fallbackTag(target))) {
        return text;
    }
    return (id != nullptr) ? std::string(id) : std::string();
}

std::string I18n::format(const char* id, const std::vector<std::string>& args) const {
    return formatIn(currentLanguage(), id, args);
}

std::string I18n::formatIn(std::string_view language, const char* id,
                           const std::vector<std::string>& args) const {
    std::array<std::string, 2> fixed;
    for (std::size_t index = 0; index < fixed.size() && index < args.size(); ++index) {
        fixed[index] = args[index];
    }
    std::string text = textIn(language, id);
    return substitutePlaceholders(text, fixed);
}

const std::vector<I18n::CoreText>& I18n::displayTexts() {
    return kDisplayTexts;
}

// ---- I18n::Message --------------------------------------------------------

I18n::Message::Message(const char* id) {
    if (id != nullptr) {
        m_id = id;
    }
}

I18n::Message::Message(const char* id, std::string arg0) : Message(id) {
    m_args[0] = std::move(arg0);
}

I18n::Message::Message(const char* id, std::string arg0, std::string arg1) : Message(id) {
    m_args[0] = std::move(arg0);
    m_args[1] = std::move(arg1);
}

std::string I18n::Message::chinese() const {
    if (m_id.empty()) {
        return {};
    }
    const I18n& i18n = I18n::instance();
    return substitutePlaceholders(i18n.textIn("zh-CN", m_id.c_str()), m_args);
}

std::string I18n::Message::localized() const {
    if (m_id.empty()) {
        return {};
    }
    // 先在锁内拷一份语言标签（language() 返回拷贝），再查静态表：本函数可能在
    // 采集 worker 线程调用（capture.failed.reason），不能直接读宿主状态。
    const I18n& i18n = I18n::instance();
    const std::string language = i18n.language();
    return substitutePlaceholders(i18n.textIn(language, m_id.c_str()), m_args);
}

}  // namespace Host
