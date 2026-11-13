#include "hostI18n.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "config.h"

// ============================================================================
//  hostI18n.cpp —— 语言目录与当前语言
//
//  这里只管"有哪些语言可选、当前是哪一个、它的回退下一站是谁"：
//    · 内置注册表 kLanguages：加语种只加一行；
//    · 外部语言包声明（i18n_source_list 的 `@language/` 条目）：运行期由
//      HostListCatalog 通过 setExternalLanguages 换入，语言目录 = 内置 ∪ 外部，
//      设置页的下拉候选随目录动态扩展（不用改宿主代码）；
//    · 当前语言：core/language 偏好 + auto 系统探测；
//    · fallbackTag：目录声明的 fallback > 注册表 fallback > 默认语言。
//
//  文案表、统一回退链与 Message 在 hostI18nText.cpp：一个文件一个关注点
//  （STYLE §6.8），两边共用 Host::I18n 的同一把锁纪律。
// ============================================================================

namespace {

// ---- 内置语言注册表 --------------------------------------------------------
// 加语种只加一行：tag 必须是 BCP-47 规范写法，nativeName 是该语言自己的显示名，
// fallback 是该语言缺文本时的下一站（空串 = 用默认语言）。内置语言的顺序就是
// 设置页 ENUM 里前置部分的顺序；外部语言包声明的语言排在它们后面、按 tag 排序。
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

/**
 * @brief 由内置注册表 + 外部声明构造完整语言目录。
 *
 * 外部声明可以覆盖内置语言的显示名 / fallback（用户包可能给 zh-CN 起别的名字、
 * 或改它的下一站），但不会改变内置语言的顺序；新语言按调用方排好的 tag 顺序
 * 追加在后面（目录顺序 = 设置页下拉顺序，稳定可预期）。
 *
 * @param[in] external 已规范化、按 tag 排序的外部声明。
 * @return 目录：内置语言在前，外部新语言在后。
 */
std::vector<Host::I18n::CatalogLanguage> buildCatalog(
    const std::vector<Host::I18n::ExternalLanguage>& external) {
    std::vector<Host::I18n::CatalogLanguage> catalog;
    const std::vector<Host::I18n::Language>& builtin = Host::I18n::languages();
    catalog.reserve(builtin.size() + external.size());
    for (const Host::I18n::Language& language : builtin) {
        Host::I18n::CatalogLanguage entry;
        entry.tag = Host::I18n::normalizeTag(language.tag);
        if (language.nativeName != nullptr) {
            entry.name = language.nativeName;
        }
        if (language.fallback != nullptr) {
            entry.fallback = Host::I18n::normalizeTag(language.fallback);
        }
        for (const Host::I18n::ExternalLanguage& declared : external) {
            if (declared.tag != entry.tag) {
                continue;
            }
            if (!declared.name.empty()) {
                entry.name = declared.name;
            }
            if (!declared.fallback.empty()) {
                entry.fallback = declared.fallback;
            }
        }
        catalog.push_back(std::move(entry));
    }
    for (const Host::I18n::ExternalLanguage& declared : external) {
        const bool known = std::any_of(
            catalog.begin(), catalog.end(),
            [&declared](const Host::I18n::CatalogLanguage& entry) {
                return entry.tag == declared.tag;
            });
        if (known) {
            continue;  // 内置语言：名字/fallback 已在上面覆盖过，不重复追加
        }
        Host::I18n::CatalogLanguage entry;
        entry.tag = declared.tag;
        entry.name = declared.name.empty() ? declared.tag : declared.name;
        entry.fallback = declared.fallback;
        catalog.push_back(std::move(entry));
    }
    return catalog;
}

/**
 * @brief 在目录里按规范标签找条目。
 * @param[in] catalog 语言目录（标签已规范化）。
 * @param[in] tag     规范标签。
 * @return 目录项；没有时返回 nullptr。
 */
const Host::I18n::CatalogLanguage* findInCatalog(
    const std::vector<Host::I18n::CatalogLanguage>& catalog, std::string_view tag) {
    for (const Host::I18n::CatalogLanguage& entry : catalog) {
        if (entry.tag == tag) {
            return &entry;
        }
    }
    return nullptr;
}

/**
 * @brief 两份外部声明是否完全一样（幂等判断用，避免每轮重建都翻版本号）。
 * @return true = tag / 显示名 / fallback 逐项相同。
 */
bool sameExternalLanguages(const std::vector<Host::I18n::ExternalLanguage>& left,
                           const std::vector<Host::I18n::ExternalLanguage>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].tag != right[index].tag || left[index].name != right[index].name ||
            left[index].fallback != right[index].fallback) {
            return false;
        }
    }
    return true;
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
    // 单例可能在任何地方第一次被取用（配置未初始化、插件目录未就绪），所以构造
    // 不做系统语言探测、也不查语言目录：只给 auto + 默认语言。真正的偏好在
    // HostRuntime::init 第一行的 loadPreference() 里生效；在那之前查询拿到的是
    // en-US 文案，不会读到半初始化的目录（构造期调 instance() 是未定义行为）。
    m_preference = autoPreference();
    m_language = defaultLanguage();
}

I18n& I18n::instance() {
    static I18n instance;
    return instance;
}

const std::vector<I18n::Language>& I18n::languages() {
    return kLanguages;
}

std::vector<I18n::CatalogLanguage> I18n::catalog() const {
    std::vector<ExternalLanguage> external;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        external = m_external;  // 锁内只拷一份，拼目录在锁外做
    }
    return buildCatalog(external);
}

bool I18n::setExternalLanguages(std::vector<ExternalLanguage> languages) {
    // 锁外规范化/排序：外部声明来自插件（语言包），空标签与重复标签都可能在。
    for (ExternalLanguage& language : languages) {
        language.tag = normalizeTag(language.tag);
        language.fallback = normalizeTag(language.fallback);
    }
    std::erase_if(languages, [](const ExternalLanguage& language) {
        return language.tag.empty();
    });
    std::stable_sort(languages.begin(), languages.end(),
                     [](const ExternalLanguage& left, const ExternalLanguage& right) {
                         return left.tag < right.tag;
                     });
    languages.erase(std::unique(languages.begin(), languages.end(),
                                [](const ExternalLanguage& left, const ExternalLanguage& right) {
                                    return left.tag == right.tag;
                                }),
                    languages.end());

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (sameExternalLanguages(m_external, languages)) {
            return false;  // 幂等：i18n_list 每轮重建都会调它，内容没变不能翻版本号
        }
        m_external = languages;
        ++m_catalogVersion;
    }
    // 目录补全后补认启动时还认不出的语言偏好（外部语言包是插件加载后才声明的）。
    adoptUnresolvedPreference();
    return true;
}

std::uint64_t I18n::catalogVersion() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_catalogVersion;
}

bool I18n::hasLanguage(std::string_view tag) {
    const std::string normalized = normalizeTag(tag);
    if (normalized.empty()) {
        return false;
    }
    const std::vector<CatalogLanguage> languages = instance().catalog();
    return findInCatalog(languages, normalized) != nullptr;
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
        const std::vector<CatalogLanguage> languages = instance().catalog();
        if (findInCatalog(languages, tag) != nullptr) {
            return tag;
        }
        // Windows 可能报 zh-HK / zh-Hans-CN 这类目录里没有的标签：按主语言族
        // 落到目录里的同族语言（zh 族 → zh-CN），而不是直接掉到 en-US。
        const std::string primary = primaryTag(tag);
        if (!primary.empty()) {
            for (const CatalogLanguage& language : languages) {
                if (primaryTag(language.tag) == primary) {
                    return language.tag;
                }
            }
        }
    }
#else
    // POSIX：按 gettext 的惯例从环境变量取 locale，优先 LANGUAGE（可能是
    // "zh_CN:en_US" 这样的优先级列表），再 LC_ALL / LC_MESSAGES / LANG。
    // 值形如 "zh_CN.UTF-8@modifier"：去掉编码与修饰符、把 '_' 换成 '-'，再按
    // 语言目录做精确匹配；目录里没有就落到同主语言族（zh_CN → zh-CN）。
    const char* const names[] = {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"};
    const std::vector<CatalogLanguage> languages = instance().catalog();
    for (const char* name : names) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            continue;
        }
        std::string raw(value);
        const std::size_t listEnd = raw.find(':');
        if (listEnd != std::string::npos) {
            raw.resize(listEnd);
        }
        const std::size_t encoding = raw.find('.');
        if (encoding != std::string::npos) {
            raw.resize(encoding);
        }
        const std::size_t modifier = raw.find('@');
        if (modifier != std::string::npos) {
            raw.resize(modifier);
        }
        if (raw.empty()) {
            continue;
        }
        std::replace(raw.begin(), raw.end(), '_', '-');
        if (raw == "C" || raw == "POSIX") {
            continue;  // C locale 不是用户语言，继续看下一个变量
        }
        const std::string tag = normalizeTag(raw);
        if (findInCatalog(languages, tag) != nullptr) {
            return tag;
        }
        const std::string primary = primaryTag(tag);
        if (!primary.empty()) {
            for (const CatalogLanguage& language : languages) {
                if (primaryTag(language.tag) == primary) {
                    return language.tag;
                }
            }
        }
    }
#endif
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
        // 配置里可能存着语言包语言（如 ja-JP）：它的 @language/ 声明要等插件加载
        // 才出现，此刻不在目录里。先退回 auto，同时记下这个值——目录补全时
        // setExternalLanguages 会自动切回去，否则用户每次重启都要重选语言。
        setPreference(autoPreference());
        rememberUnresolvedPreference(stored);
    }
}

bool I18n::setPreference(std::string_view value) {
    // 先在锁外算好两个字符串（含 auto 的系统探测与目录查询），锁内只做赋值：
    // 临界区最短，也不会在持锁时调用可能阻塞的 Win32 API 或再取一次目录锁。
    const std::string normalized = normalizeTag(value);
    if (normalized.empty()) {
        return false;
    }
    std::string preference;
    std::string language;
    if (normalized == autoPreference()) {
        preference = autoPreference();
        language = resolveLanguage(preference);
    } else if (hasLanguage(normalized)) {
        preference = normalized;
        language = normalized;
    } else {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_preference = std::move(preference);
    m_language = std::move(language);
    m_unresolvedPreference.clear();  // 用户/配置已经选定了，不再补认
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
    return fallbackTag(currentLanguage());
}

std::string I18n::fallbackTag(std::string_view language) {
    const std::string normalized = normalizeTag(language);
    if (normalized.empty()) {
        return defaultLanguage();
    }
    // 先查内置注册表（绝大多数调用落在这里，不用拷外部声明）。
    for (const Language& entry : kLanguages) {
        if (normalizeTag(entry.tag) == normalized) {
            if (entry.fallback != nullptr && entry.fallback[0] != '\0') {
                return normalizeTag(entry.fallback);
            }
            return defaultLanguage();
        }
    }
    // 再查语言包声明：锁内只拷一份外部声明，比较在锁外做。
    const I18n& self = instance();
    std::vector<ExternalLanguage> external;
    {
        std::lock_guard<std::mutex> lock(self.m_mutex);
        external = self.m_external;
    }
    for (const ExternalLanguage& entry : external) {
        if (entry.tag == normalized) {
            return entry.fallback.empty() ? defaultLanguage() : entry.fallback;
        }
    }
    return defaultLanguage();
}

std::string I18n::resolveLanguage(std::string_view preference) {
    if (normalizeTag(preference) == autoPreference()) {
        // detectSystemLanguage 已经返回目录里的标签（或默认语言），不用再兜底。
        return detectSystemLanguage();
    }
    const std::string normalized = normalizeTag(preference);
    if (hasLanguage(normalized)) {
        return normalized;
    }
    return defaultLanguage();
}

void I18n::rememberUnresolvedPreference(std::string_view value) {
    const std::string normalized = normalizeTag(value);
    if (normalized.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_unresolvedPreference = normalized;
}

void I18n::adoptUnresolvedPreference() {
    std::string pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pending = m_unresolvedPreference;
    }
    if (pending.empty()) {
        return;
    }
    // 目录在锁外拼，锁内只做"仍是同一个待认领值"的复查与赋值。
    const std::vector<CatalogLanguage> languages = catalog();
    if (findInCatalog(languages, pending) == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_unresolvedPreference != pending) {
        return;
    }
    m_preference = pending;
    m_language = pending;
    m_unresolvedPreference.clear();
}

}  // namespace Host
