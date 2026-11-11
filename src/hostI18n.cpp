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
};

// ---- 会发布进 i18n_list 的宿主文案 ----------------------------------------
// id 的两类用法：
//   · 数据列表里的 key（tray_command_list 的 label、config_list 的 label/tooltip）；
//   · ENUM 候选值（core 的语言与日志级别），设置页按 display(owner, value) 解析。
// core 的 owner_name 也走这里：设置页把 owner_name 当显示名，不解析的话英文界面
// 会剩一个"宿主"。
const std::vector<Host::I18n::CoreText> kDisplayTexts = {
    {"core.owner_name", "宿主", "Host"},
    {"capture.request", "截屏", "Capture"},
    {"region.capture", "区域截屏", "Region capture"},
    {"settings.open", "打开设置", "Open settings"},
    {"app.quit", "退出", "Quit"},
    {"config.language.label", "界面语言", "Language"},
    {"config.language.tooltip", "auto = 跟随系统；切换后界面立即刷新",
     "Auto follows the system; the UI refreshes immediately"},
    {"config.logging.level.label", "日志级别", "Log level"},
    {"config.logging.level.tooltip", "host / plugin 两个 logger 一起改",
     "Applies to both the host and plugin loggers"},
    {"config.debug.console.label", "控制台日志", "Console log"},
    {"config.debug.console.tooltip", "关闭后控制台不再输出，文件日志不受影响",
     "Console output stops when off; the log file is unaffected"},
    {"config.logging.file.label", "文件日志", "Log file"},
    {"config.logging.file.tooltip", "写入 <数据目录>/logs/host.log；文件打不开时恒为关闭",
     "Writes to <data dir>/logs/host.log; stays off if the file cannot be opened"},
    // 语言下拉的原生显示名：换语言时语言名保持各自母语写法（用户认得出）。
    {"auto", "跟随系统", "Follow system"},
    {"zh-CN", "简体中文", "简体中文"},
    {"zh-TW", "繁體中文", "繁體中文"},
    {"en-US", "English", "English"},
    // 日志级别 ENUM 的候选：值是技术值（debug/info/...），显示名走同一套解析。
    {"debug", "调试", "Debug"},
    {"info", "信息", "Info"},
    {"warn", "警告", "Warning"},
    {"error", "错误", "Error"},
};

// ---- 宿主生成的动态消息（带模板参数）-------------------------------------
// 这些字符串会被 settings_ui / notify 原样展示，必须按当前语言生成；内部日志
// 用 Message::chinese() 固定中文（日志不翻译，e2e 按中文日志断言）。
const std::vector<Host::I18n::CoreText> kMessages = {
    // 插件加载/依赖/启停原因（PluginManager -> plugin_registry.reason / plugins.changed）
    {"plugin.error.no_export", "没有 shot_module_entry 导出", "Missing shot_module_entry export"},
    {"plugin.error.empty_module", "shot_module_entry 返回空", "shot_module_entry returned null"},
    {"plugin.error.module_too_short", "模块表太短（{0} 字节）",
     "Module table too short ({0} bytes)"},
    {"plugin.error.abi_major", "abi major 不兼容（插件 {0}，宿主 {1}）",
     "Incompatible abi major (plugin {0}, host {1})"},
    {"plugin.error.missing_create_destroy", "缺少 create/destroy", "Missing create/destroy"},
    {"plugin.error.init_rejected", "init 拒绝了本宿主（{0}）", "init rejected this host ({0})"},
    {"plugin.error.dep_syntax", "依赖语法错误：{0}", "Invalid requirement syntax: {0}"},
    {"plugin.error.core_version", "需要 {0}，宿主是 {1}", "Requires {0}, host is {1}"},
    {"plugin.error.no_provider", "没有提供者满足 {0}", "No provider satisfies {0}"},
    {"plugin.error.provider_skipped", "提供者 {0} 被跳过了", "Provider {0} was skipped"},
    {"plugin.error.provider_no_version", "提供者 {0} 没有报告版本",
     "Provider {0} reported no version"},
    {"plugin.error.version_mismatch", "要求 {0}，实际提供 {1}",
     "Requires {0}, actually provided {1}"},
    {"plugin.error.dll_load", "DLL 加载失败（{0}）", "Failed to load DLL ({0})"},
    {"plugin.error.create_null", "create 返回空", "create returned null"},
    {"plugin.error.ui_init", "UIProvider::init 失败", "UIProvider::init failed"},
    {"plugin.error.no_metadata", "没有 metadata 且没有本宿主认识的任何能力",
     "No metadata and no capability this host understands"},
    {"plugin.error.empty_id", "插件 id 为空", "Plugin id is empty"},
    {"plugin.error.no_such_plugin", "没有这个插件", "No such plugin"},
    {"plugin.error.required_by", "被 {0} 依赖", "Required by {0}"},
    {"plugin.error.dependency_enable_failed", "依赖 {0} 启用失败：{1}",
     "Failed to enable dependency {0}: {1}"},
    {"plugin.state.not_loaded", "未加载", "Not loaded"},
    {"plugin.state.disabled", "已在配置中禁用", "Disabled in configuration"},
    // 采集失败原因（capture.failed.reason，notify 气泡直接展示）
    {"capture.error.host_shutting_down", "宿主正在退出，请求被丢弃",
     "Host is shutting down; request dropped"},
    {"capture.error.no_capture_plugin", "没有可用的屏幕采集插件",
     "No screen capture plugin available"},
    {"capture.error.describe_failed", "describe 失败（返回 {0}）", "describe failed (returned {0})"},
    {"capture.error.invalid_size", "describe 返回的尺寸非法（{0}x{1}）",
     "describe returned an invalid size ({0}x{1})"},
    {"capture.error.unsupported_pixel_format", "不支持的像素格式（{0}）",
     "Unsupported pixel format ({0})"},
    {"capture.error.too_large", "采集尺寸过大（{0} 字节）", "Capture size too large ({0} bytes)"},
    {"capture.error.capture_failed", "capture 失败（返回 {0}）", "capture failed (returned {0})"},
    {"capture.error.no_saver_plugin", "没有可用的图像保存插件",
     "No image saver plugin available"},
    {"capture.error.save_failed", "save 失败（返回 {0}）", "save failed (returned {0})"},
    // 列表写值/落盘失败原因（lists.updated.reason，设置页在配置项旁标红显示）
    {"list.error.bad_payload", "负载不是合法 JSON 对象", "Payload is not a valid JSON object"},
    {"list.error.update_only_config", "lists.update 只支持 config_list",
     "lists.update only supports config_list"},
    {"list.error.commit_only_config", "lists.commit 只支持 config_list",
     "lists.commit only supports config_list"},
    {"list.error.changes_array", "changes 必须是数组", "changes must be an array"},
    {"list.error.bad_param", "参数非法：owner/id/value 必须都是字符串",
     "Invalid parameter: owner/id/value must be strings"},
    {"list.error.provider_unavailable", "提供者未加载或不支持 shot.configurable",
     "Provider not loaded or does not support shot.configurable"},
    {"list.error.plugin_rejected", "插件拒绝（返回码 {0}）", "Plugin rejected (returned {0})"},
    {"list.error.save_failed", "保存失败", "Failed to save"},
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

const char* I18n::textColumn(std::string_view language) {
    // 本批只有两列：zh 语言族用简体中文列，其它（含默认 en-US 与未翻译语种）用英文列。
    return (primaryTag(language) == "zh") ? "zh-CN" : "en-US";
}

const char* I18n::text(const char* id) const {
    const CoreText* entry = findText(id);
    if (entry == nullptr) {
        return id;
    }
    const std::string column = textColumn(currentLanguage());
    return (column == "zh-CN") ? entry->zhCN : entry->enUS;
}

std::string I18n::textIn(std::string_view language, const char* id) const {
    const CoreText* entry = findText(id);
    if (entry == nullptr) {
        return (id != nullptr) ? std::string(id) : std::string();
    }
    const std::string column =
        textColumn(language.empty() ? std::string_view(currentLanguage()) : language);
    return (column == "zh-CN") ? std::string(entry->zhCN) : std::string(entry->enUS);
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
