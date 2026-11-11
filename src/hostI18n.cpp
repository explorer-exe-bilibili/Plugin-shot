#include "hostI18n.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "config.h"

// ============================================================================
//  hostI18n.cpp —— 宿主 i18n 中心的实现
//
//  文案表刻意写成两张静态表：
//    · displayTexts：会被发布进 i18n_list 的 key -> 文本（托盘 label、配置项
//      label/tooltip、ENUM 候选原生名、core 的 owner_name）。消费方用
//      display(owner, key) 规则解析，所以这些 key 必须恰好等于数据列表里的值。
//    · 动态消息表：带 {0}/{1} 参数的宿主文案，只有宿主知道参数，不走 key。
//
//  语言选择的三条规则（document/I18N.md）：
//    1. 完全匹配（大小写不敏感、下划线视同连字符）；
//    2. 主语言族：zh-TW 没有文本时先用 zh 语言族的表（本批落在 zh-CN）；
//    3. 默认语言 en-US；仍没有就取第一条可用文本。
//  本批文案表只有两列（zh-CN / en-US），所以列选择是"zh 语言族 -> zh-CN，
//  其它 -> en-US"；以后加语种时把语言注册表与这两列一起扩。
// ============================================================================

namespace {

// ---- 语言注册表 ------------------------------------------------------------
// 加语种只加一行：tag 必须是 BCP-47 规范写法，nativeName 是该语言自己的显示名，
// fallback 是该语言缺文本时的下一站（空串 = 用默认语言）。设置页的 ENUM 候选
// 与 i18n_list 的 @meta 都从这里来，不要在别处硬编码语言清单。
const std::vector<Host::I18n::Language> kLanguages = {
    {"zh-CN", "简体中文", "zh"},
    {"zh-TW", "繁體中文", "zh"},
    {"en-US", "English", ""},
    // 以下语种当前只有"可选 + 回退"（首批译文只有 zh-CN / en-US，未翻译文案按回退链
    // 落 en-US）。它们都使用设置页当前字体（msyh.ttc）覆盖的 Latin / Cyrillic /
    // Greek 字母与日文假名/汉字；韩/泰/印地/阿拉伯/希伯来/越南等在当前字体里缺字形
    // （实测 msyh.ttc 无对应 glyph）或需要 RTL 布局的语种
    // 暂不注册，见 document/I18N.md §7。
    {"fr-FR", "Français", ""},
    {"de-DE", "Deutsch", ""},
    {"es-ES", "Español", ""},
    {"pt-BR", "Português (Brasil)", ""},
    {"it-IT", "Italiano", ""},
    {"nl-NL", "Nederlands", ""},
    {"sv-SE", "Svenska", ""},
    {"da-DK", "Dansk", ""},
    {"nb-NO", "Norsk bokmål", ""},
    {"fi-FI", "Suomi", ""},
    {"pl-PL", "Polski", ""},
    {"cs-CZ", "Čeština", ""},
    {"sk-SK", "Slovenčina", ""},
    {"hu-HU", "Magyar", ""},
    {"hr-HR", "Hrvatski", ""},
    {"tr-TR", "Türkçe", ""},
    {"ru-RU", "Русский", ""},
    {"uk-UA", "Українська", ""},
    {"bg-BG", "Български", ""},
    {"el-GR", "Ελληνικά", ""},
    {"ja-JP", "日本語", ""},
    {"ro-RO", "Română", ""},
    {"id-ID", "Bahasa Indonesia", ""},
    {"ms-MY", "Bahasa Melayu", ""},
};

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
    // 语言标签的"原生显示名"不在这里：由语言注册表提供、HostListCatalog 生成
    // i18n_list 时投影成 core 条目。只有 auto 不是语言标签，需要本地化文本。
    {"auto", {{"zh-CN", "跟随系统"}, {"en-US", "Follow system"}}},
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

/** @brief 把 UTF-16 的 Windows locale 名转成 UTF-8。 */
#ifdef _WIN32
std::string wideToUtf8(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}
#endif

}  // namespace

namespace Host {

I18n::I18n() {
    m_preference = autoPreference();
    m_language = resolveLanguage(m_preference);
}

I18n& I18n::instance() {
    static I18n instance;
    return instance;
}

const std::vector<I18n::Language>& I18n::languages() {
    return kLanguages;
}

const I18n::Language* I18n::findLanguage(std::string_view tag) {
    const std::string normalized = normalizeTag(tag);
    if (normalized.empty()) {
        return nullptr;
    }
    for (const Language& language : kLanguages) {
        if (normalizeTag(language.tag) == normalized) {
            return &language;
        }
    }
    return nullptr;
}

std::string I18n::normalizeTag(std::string_view tag) {
    std::string out;
    std::size_t start = 0;
    int part = 0;
    while (start < tag.size()) {
        const std::size_t separator = tag.find_first_of("-_", start);
        const std::size_t end = (separator == std::string_view::npos) ? tag.size() : separator;
        const std::string_view piece = tag.substr(start, end - start);
        start = (separator == std::string_view::npos) ? tag.size() : separator + 1;
        if (piece.empty()) {
            continue;
        }
        if (part > 0) {
            out.push_back('-');
        }
        // BCP-47 的常见写法：语言小写、脚本首字母大写、地区大写；其余部分原样小写。
        const bool allAlpha = std::all_of(piece.begin(), piece.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        });
        const bool allDigit = std::all_of(piece.begin(), piece.end(), [](char c) {
            return c >= '0' && c <= '9';
        });
        if (part == 0) {
            for (const char c : piece) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        } else if (allAlpha && piece.size() == 4) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(piece[0]))));
            for (std::size_t index = 1; index < piece.size(); ++index) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(piece[index]))));
            }
        } else if ((allAlpha && piece.size() == 2) || (allDigit && piece.size() == 3)) {
            for (const char c : piece) {
                out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
        } else {
            for (const char c : piece) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        ++part;
    }
    return out;
}

std::string I18n::primaryTag(std::string_view tag) {
    const std::string normalized = normalizeTag(tag);
    const std::size_t separator = normalized.find('-');
    return (separator == std::string::npos) ? normalized : normalized.substr(0, separator);
}

const char* I18n::defaultLanguage() { return "en-US"; }

const char* I18n::autoPreference() { return "auto"; }

const char* I18n::preferenceKey() { return "language"; }

std::string I18n::detectSystemLanguage() {
#ifdef _WIN32
    wchar_t buffer[LOCALE_NAME_MAX_LENGTH] = {0};
    const int length = ::GetUserDefaultLocaleName(buffer, LOCALE_NAME_MAX_LENGTH);
    if (length > 1) {
        const std::string tag = normalizeTag(wideToUtf8(buffer));
        if (findLanguage(tag) != nullptr) {
            return tag;
        }
        // Windows 可能报 zh-HK / zh-Hans-CN 这类注册表里没有的标签：按主语言族
        // 落到已注册的同族语言（zh 族 → zh-CN），而不是直接掉到 en-US。
        const std::string primary = primaryTag(tag);
        if (!primary.empty()) {
            for (const Language& language : kLanguages) {
                if (primaryTag(language.tag) == primary) {
                    return normalizeTag(language.tag);
                }
            }
        }
    }
#endif
    // 非 Windows 平台本期不做系统语言探测：统一回退默认语言（document/I18N.md 限制）。
    return defaultLanguage();
}

void I18n::loadPreference() {
    const std::string configKey = std::string("core/") + preferenceKey();
    core::Config& config = core::Config::getInstance();
    if (!config.contains(configKey)) {
        setPreference(autoPreference());
        return;
    }
    const std::string stored = config.get(configKey).toString();
    if (!setPreference(stored)) {
        setPreference(autoPreference());
    }
}

bool I18n::setPreference(std::string_view value) {
    // 先在锁外算好两个字符串（含 auto 的系统探测），锁内只做赋值：临界区最短，
    // 也不会在持锁时调用可能阻塞的 Win32 API。
    const std::string normalized = normalizeTag(value);
    if (normalized.empty()) {
        return false;
    }
    std::string preference;
    std::string language;
    if (normalized == autoPreference()) {
        preference = autoPreference();
        language = resolveLanguage(preference);
    } else {
        const Language* entry = findLanguage(normalized);
        if (entry == nullptr) {
            return false;
        }
        preference = normalizeTag(entry->tag);
        language = preference;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_preference = std::move(preference);
    m_language = std::move(language);
    return true;
}

std::string I18n::preference() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_preference;
}

std::string I18n::language() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_language;
}

std::string I18n::currentLanguage() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_language;
}

std::string I18n::fallbackLanguage() const {
    const Language* language = findLanguage(currentLanguage());
    if (language != nullptr && language->fallback != nullptr && language->fallback[0] != '\0') {
        return normalizeTag(language->fallback);
    }
    return defaultLanguage();
}

std::string I18n::resolveLanguage(std::string_view preference) {
    if (normalizeTag(preference) == autoPreference()) {
        const std::string detected = detectSystemLanguage();
        if (findLanguage(detected) != nullptr) {
            return normalizeTag(detected);
        }
        return defaultLanguage();
    }
    const Language* language = findLanguage(preference);
    return (language != nullptr) ? normalizeTag(language->tag) : defaultLanguage();
}

const I18n::CoreText* I18n::findText(const char* id) {
    const CoreText* entry = findInTable(kDisplayTexts, id);
    return (entry != nullptr) ? entry : findInTable(kMessages, id);
}

const char* I18n::pickFromTexts(const std::vector<LanguageText>& texts,
                                std::string_view language) {
    if (texts.empty()) {
        return nullptr;
    }
    const std::string normalized = normalizeTag(language);
    // 回退链：完全匹配 -> 主语言族 -> 默认语言 -> 默认语言的主语言 -> 第一条兜底。
    std::vector<std::string> candidates;
    if (!normalized.empty()) {
        candidates.push_back(normalized);
        candidates.push_back(primaryTag(normalized));
    }
    candidates.push_back(defaultLanguage());
    candidates.push_back(primaryTag(defaultLanguage()));

    for (const std::string& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        for (const LanguageText& text : texts) {
            if (text.tag == nullptr || text.text == nullptr) {
                continue;
            }
            if (normalizeTag(text.tag) == candidate) {
                return text.text;  // 完全匹配
            }
        }
        const std::string candidatePrimary = primaryTag(candidate);
        for (const LanguageText& text : texts) {
            if (text.tag == nullptr || text.text == nullptr) {
                continue;
            }
            if (primaryTag(text.tag) == candidatePrimary) {
                return text.text;  // 同语言族（zh-TW -> zh-CN）
            }
        }
    }
    // 所有语言都缺：取第一条可用文本（作者的表顺序）。
    for (const LanguageText& text : texts) {
        if (text.text != nullptr) {
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
    if (const char* text = pickFromTexts(entry->texts, currentLanguage())) {
        return text;
    }
    return id;
}

std::string I18n::textIn(std::string_view language, const char* id) const {
    const CoreText* entry = findText(id);
    if (entry == nullptr) {
        return (id != nullptr) ? std::string(id) : std::string();
    }
    const std::string_view target =
        language.empty() ? std::string_view(currentLanguage()) : language;
    if (const char* text = pickFromTexts(entry->texts, target)) {
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

bool I18n::findTextFor(const std::map<std::string, std::string>& texts, std::string_view tag,
                       std::string& out) {
    const std::string normalized = normalizeTag(tag);
    if (normalized.empty()) {
        return false;
    }
    // 第一优先：完全匹配（映射里的 key 可能是非规范写法，逐个规范化后比较）。
    for (const auto& [key, value] : texts) {
        if (normalizeTag(key) == normalized) {
            out = value;
            return true;
        }
    }
    // 第二优先：同语言族（zh 匹配 zh-CN / zh-TW；en-US 也可以匹配 en-GB 之类的表）。
    const std::string primary = primaryTag(normalized);
    if (!primary.empty() && primary != normalized) {
        for (const auto& [key, value] : texts) {
            if (primaryTag(key) == primary) {
                out = value;
                return true;
            }
        }
    }
    return false;
}

std::string I18n::pickText(const std::map<std::string, std::string>& texts) const {
    return pickTextIn(currentLanguage(), texts);
}

std::string I18n::pickTextIn(std::string_view language,
                             const std::map<std::string, std::string>& texts) const {
    if (texts.empty()) {
        return {};
    }
    const std::string normalized =
        normalizeTag(language.empty() ? std::string_view(currentLanguage()) : language);
    // 回退链：完全匹配 -> 主语言 -> 默认语言 -> 默认语言的主语言；同族匹配在
    // findTextFor 里做，所以主语言那一步也能命中 zh-CN 表。
    std::vector<std::string> candidates;
    candidates.push_back(normalized);
    const std::string primary = primaryTag(normalized);
    if (!primary.empty()) {
        candidates.push_back(primary);
    }
    candidates.push_back(defaultLanguage());
    candidates.push_back(primaryTag(defaultLanguage()));

    std::string out;
    for (const std::string& candidate : candidates) {
        if (findTextFor(texts, candidate, out)) {
            return out;
        }
    }
    return texts.begin()->second;  // 最后的兜底：该 key 第一条可用文本
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
