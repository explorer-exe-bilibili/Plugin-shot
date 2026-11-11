// 外部语言包的两个配置项：enabled（默认关）+ pack_dirs（目录覆盖）。
//
// 只负责声明与存值，不做扫描、不发事件：值一律按契约用字符串表达，
//   · enabled   → "1" / "0"
//   · pack_dirs → 目录路径（分号分隔多个；空串 = 用官方 + 用户默认目录）
//
// label / tooltip 存的是文案 key，不是最终显示文本：设置页按
// display("translator.main", key) 到 i18n_list 里取当前语言（语言表在
// translator_plugin.cpp 的 kLangTables）。

#include "translator_config.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

namespace translator {
namespace {

// 配置键：全小写 + 下划线，发布后不能改名（它是存盘标识，见 ABI_POLICY.md §5）。
// 存盘/投影时宿主会加插件 id 前缀：translator.main/enabled、translator.main/pack_dirs。
const char kKeyEnabled[] = "enabled";
const char kKeyPackDirs[] = "pack_dirs";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyEnabled,
        .label         = "config.enabled.label",
        .default_value = "0",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "config.enabled.tooltip",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_PATH,
        .key           = kKeyPackDirs,
        .label         = "config.pack_dirs.label",
        .default_value = "",
        .options       = "directory",  // 设置页「浏览…」弹选目录对话框
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "config.pack_dirs.tooltip",
    },
};

/**
 * @brief 把配置里的布尔文本解析成 bool。
 *
 * 只把 "0" / "false" 当成关：宿主升级后可能传别的写法（"off" 之类），按
 * "不认识就当默认值（关）"处理，比把用户显式打开的开关意外关掉/打开更安全。
 *
 * @param[in] value 配置值文本，不能为空。
 * @return true = 开；false = 关（含 "0" / "false"）。
 * @note 无状态的纯解析函数，不属于 TranslatorConfig 的接口，所以放匿名命名空间（STYLE §6.7）。
 */
bool parseBool(const char* value) {
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

}  // namespace

TranslatorConfig::TranslatorConfig() = default;

std::uint32_t TranslatorConfig::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t TranslatorConfig::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t TranslatorConfig::getValue(const char* key, char* buf,
                                        std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        return copyValue(m_enabled.load() ? "1" : "0", buf, bufSize);
    }
    if (std::strcmp(key, kKeyPackDirs) == 0) {
        std::string value;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            value = m_packDirs;
        }
        return copyValue(value.c_str(), buf, bufSize);
    }
    return -1;  // 不认识的 key：安全忽略（宿主升级后多传一项也不会踩这里）
}

std::int32_t TranslatorConfig::setValue(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        m_enabled.store(parseBool(value));
        return 0;
    }
    if (std::strcmp(key, kKeyPackDirs) == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_packDirs = value;
        return 0;
    }
    return -1;
}

bool TranslatorConfig::enabled() const noexcept {
    return m_enabled.load();
}

std::string TranslatorConfig::packDirs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_packDirs;
}

std::int32_t TranslatorConfig::copyValue(const char* value, char* buf, std::uint32_t bufSize) {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    // 契约：buf_size 大于返回值才写入（含结尾 NUL）；装不下只报长度，不算失败。
    if (buf != nullptr && bufSize > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

std::int32_t TranslatorConfig::copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (src == nullptr || out == nullptr) {
        return 1;
    }
    // 宿主可能比本插件老：装不下 tooltip 就报 2，绝不写到调用方内存之外。
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = *src;
    return 0;
}

}  // namespace translator
