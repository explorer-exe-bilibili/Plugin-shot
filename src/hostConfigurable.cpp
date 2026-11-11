#include "hostConfigurable.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>

#include "hostI18n.h"
#include "hostLogging.h"
#include "version.h"  // CMake 从 include/version.h.in 生成：VERSION_STRING

namespace {

// 配置项 key 不带 "core/" 前缀：前缀只由配置目录在存盘时拼，见 SETTINGS.md §2。
const char kKeyLanguage[] = "language";  // 与 Host::I18n::preferenceKey() 相同
const char kKeyLoggingLevel[] = "logging.level";
const char kKeyDebugConsole[] = "debug.console";
const char kKeyLoggingFile[] = "logging.file";

// ENUM 的候选值按契约用 | 分隔；写值时会逐项比对，不认识的候选直接拒绝。
const char kLevelOptions[] = "debug|info|warn|error";

/**
 * @brief 语言 ENUM 的候选值：auto + 语言目录（内置先、外部语言包声明按 tag 排序）。
 *
 * 目录会被语言包动态扩展（`@language/` 条目），所以不能像以前那样只算一次：
 * 按 Host::I18n::catalogVersion() 缓存，版本变了才重算。返回静态存储的引用——
 * ShotConfigDesc 的字符串字段约定"由提供者持有"，宿主在 get_config_desc 返回后
 * 立刻把 options 抄进 config_list 的 JSON，期间不会再有目录变化。
 *
 * 只在宿主 main 线程（config_list 重建 / lists.update）调用；版本号在锁外取，
 * 重算在缓存锁内做，避免两个线程同时扩容同一个 std::string。
 */
const std::string& languageOptions() {
    static std::mutex mutex;
    static std::uint64_t cachedVersion = 0;
    static std::string options;
    const Host::I18n& i18n = Host::I18n::instance();
    const std::uint64_t version = i18n.catalogVersion();
    std::lock_guard<std::mutex> lock(mutex);
    if (options.empty() || version != cachedVersion) {
        std::string rebuilt = Host::I18n::autoPreference();
        for (const Host::I18n::CatalogLanguage& language : i18n.catalog()) {
            rebuilt += "|";
            rebuilt += language.tag;
        }
        options = std::move(rebuilt);
        cachedVersion = version;
    }
    return options;
}

/**
 * @brief 设置页显示文案的 key；config_list 里 core 的 label/tooltip 都是 key，
 *        由消费方用 i18n_list 解析（document/I18N.md），所以配置表里不写中文。
 */
const char kLabelLanguage[] = "config.language.label";
const char kTooltipLanguage[] = "config.language.tooltip";
const char kLabelLoggingLevel[] = "config.logging.level.label";
const char kTooltipLoggingLevel[] = "config.logging.level.tooltip";
const char kLabelDebugConsole[] = "config.debug.console.label";
const char kTooltipDebugConsole[] = "config.debug.console.tooltip";
const char kLabelLoggingFile[] = "config.logging.file.label";
const char kTooltipLoggingFile[] = "config.logging.file.tooltip";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeyLanguage,
        .label         = kLabelLanguage,
        .default_value = "auto",
        // options 由 getConfigDesc 用 languageOptions() 填：候选来自语言注册表。
        .options       = nullptr,
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipLanguage,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeyLoggingLevel,
        .label         = kLabelLoggingLevel,
        .default_value = "info",
        .options       = kLevelOptions,
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipLoggingLevel,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyDebugConsole,
        .label         = kLabelDebugConsole,
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = kTooltipDebugConsole,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyLoggingFile,
        .label         = kLabelLoggingFile,
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = kTooltipLoggingFile,
    },
};

constexpr uint32_t kConfigCount = static_cast<uint32_t>(std::size(kConfig));

/**
 * @brief 解析布尔配置值。
 *
 * 只接受契约里约定的四种写法（"1"/"0"/"true"/"false"），与 shot::Config::getBool
 * 的读法保持对称；其它值一律当非法，避免把 "yes" 静默当成 false。
 *
 * @param[in]  text  配置值字符串。
 * @param[out] value 解析结果。
 * @return true = 是合法布尔值。
 */
bool parseBool(const char* text, bool& value) {
    if (std::strcmp(text, "1") == 0 || std::strcmp(text, "true") == 0 ||
        std::strcmp(text, "TRUE") == 0) {
        value = true;
        return true;
    }
    if (std::strcmp(text, "0") == 0 || std::strcmp(text, "false") == 0 ||
        std::strcmp(text, "FALSE") == 0) {
        value = false;
        return true;
    }
    return false;
}

/**
 * @brief 判断 level 是否是合法的日志级别名。
 * @param[in] text 候选级别名（大小写不敏感）。
 * @return true = 属于 ENUM 的候选项。
 */
bool isValidLevel(const char* text) {
    for (const char* option : {"debug", "info", "warn", "error"}) {
        if (std::strcmp(text, option) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

const char* Host::HostConfigurable::id() { return "core"; }

// 显示名也是 key：设置页按 display(core, owner_name) 解析成"宿主"/"Host"。
const char* Host::HostConfigurable::name() { return "core.owner_name"; }

const char* Host::HostConfigurable::version() { return VERSION_STRING; }

const ShotConfigurableV1* Host::HostConfigurable::service() {
    static const ShotConfigurableV1 kService = {
        .struct_size       = sizeof(ShotConfigurableV1),
        .version           = 1,
        .get_config_count  = &HostConfigurable::getConfigCount,
        .get_config_desc   = &HostConfigurable::getConfigDesc,
        .get_config        = &HostConfigurable::getConfig,
        .set_config        = &HostConfigurable::setConfig,
    };
    return &kService;
}

uint32_t Host::HostConfigurable::getConfigCount(void* /*self*/) {
    return kConfigCount;
}

int32_t Host::HostConfigurable::getConfigDesc(void* /*self*/, uint32_t index, ShotConfigDesc* out) {
    if (out == nullptr || index >= kConfigCount) {
        return 1;
    }
    // 宿主的结构可能比本实现老：装不下 tooltip 就报 2，绝不写到调用方内存之外。
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = kConfig[index];
    if (out->key != nullptr && std::strcmp(out->key, kKeyLanguage) == 0) {
        out->options = languageOptions().c_str();
    }
    return 0;
}

int32_t Host::HostConfigurable::getConfig(void* /*self*/, const char* key, char* buf,
                                           uint32_t bufSize) {
    if (key == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyLanguage) == 0) {
        return copyValue(buf, bufSize, Host::I18n::instance().preference());
    }
    if (std::strcmp(key, kKeyLoggingLevel) == 0) {
        return copyValue(buf, bufSize, Logging::level());
    }
    if (std::strcmp(key, kKeyDebugConsole) == 0) {
        return copyValue(buf, bufSize, Logging::consoleEnabled() ? "1" : "0");
    }
    if (std::strcmp(key, kKeyLoggingFile) == 0) {
        return copyValue(buf, bufSize, Logging::fileEnabled() ? "1" : "0");
    }
    return -1;  // 不认识的 key 安全忽略：宿主升级后多传配置项也不会踩这里
}

int32_t Host::HostConfigurable::setConfig(void* /*self*/, const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyLanguage) == 0) {
        // 语言偏好立即生效（与其它 core 配置项一致，落盘由 lists.commit 负责）；
        // i18n_list 的重建由 HostListCatalog 在下一轮 tick 完成并通知消费方。
        return Host::I18n::instance().setPreference(value) ? 0 : -1;
    }
    if (std::strcmp(key, kKeyLoggingLevel) == 0) {
        if (!isValidLevel(value)) {
            return -1;
        }
        Logging::setLevel(value);
        return 0;
    }
    bool enabled = false;
    if (!parseBool(value, enabled)) {
        return -1;
    }
    if (std::strcmp(key, kKeyDebugConsole) == 0) {
        return Logging::setConsoleEnabled(enabled) ? 0 : -1;
    }
    if (std::strcmp(key, kKeyLoggingFile) == 0) {
        return Logging::setFileEnabled(enabled) ? 0 : -1;
    }
    return -1;
}

int32_t Host::HostConfigurable::copyValue(char* buf, uint32_t bufSize, const std::string& value) {
    const std::size_t len = value.size();
    if (buf != nullptr && bufSize > len) {
        std::memcpy(buf, value.data(), len);
        buf[len] = '\0';
    }
    return static_cast<int32_t>(len);
}
