// 消息提示插件的配置项声明与值读写。
//
// 配置项声明、字符串值解析、当前值读写都在这个文件里；ABI 回调在
// plugin_entry.cpp 只做参数转发。值一律按契约用字符串表达：
//   · enabled / show_on_success / show_on_error / sound → "1" / "0"
//
// 线程模型：set_config 可能在宿主任意线程上被调用（init 的 apply_stored、设置页
// 应用），通知线程同时可能正在读开关；四个布尔用 std::atomic 保存，读侧永远拿到
// 完整值、不需要加锁。

#include "notify_config.h"

#include <cstring>
#include <iterator>

namespace notify {
namespace {

// 配置键：全小写 + 下划线，发布后不能改名（它是存盘标识，见 ABI_POLICY.md §5）。
const char kKeyEnabled[] = "enabled";
const char kKeyShowOnSuccess[] = "show_on_success";
const char kKeyShowOnError[] = "show_on_error";
const char kKeySound[] = "sound";

// label / tooltip 是 i18n key，不是最终显示文本：宿主把 key 原样放进 config_list，
// 设置页用 i18n_list 解析成当前语言（display(owner, key) ?? key）。对应文案在
// plugin_entry.cpp 的 kLangTables 里（config.<配置键>.label / .tooltip），两边必须
// 一起改；查不到时设置页显示 key 本身，便于发现漏写。
const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyEnabled,
        .label         = "config.enabled.label",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "config.enabled.tooltip",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyShowOnSuccess,
        .label         = "config.show_on_success.label",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "config.show_on_success.tooltip",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyShowOnError,
        .label         = "config.show_on_error.label",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "config.show_on_error.tooltip",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeySound,
        .label         = "config.sound.label",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "config.sound.tooltip",
    },
};

/**
 * @brief 把配置里的布尔文本解析成 bool。
 *
 * 只把 "0" / "false" 当成关：宿主升级后可能传别的写法，按"不认识就当默认值（开）"
 * 处理，比把用户配置的开关意外关掉安全（与 tray / clipboard 的约定一致）。
 *
 * @param[in] value 配置值文本，不能为空。
 * @return true = 开；false = 关（含 "0" / "false"）。
 * @note 无状态的纯解析函数，不属于 NotifyConfig 的接口，所以放匿名命名空间（STYLE §6.7）。
 */
bool parseBool(const char* value) {
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

}  // namespace

NotifyConfig::NotifyConfig() = default;

std::uint32_t NotifyConfig::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t NotifyConfig::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t NotifyConfig::getValue(const char* key, char* buf, std::uint32_t buf_size) const {
    if (key == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        return copyValue(m_enabled.load() ? "1" : "0", buf, buf_size);
    }
    if (std::strcmp(key, kKeyShowOnSuccess) == 0) {
        return copyValue(m_showOnSuccess.load() ? "1" : "0", buf, buf_size);
    }
    if (std::strcmp(key, kKeyShowOnError) == 0) {
        return copyValue(m_showOnError.load() ? "1" : "0", buf, buf_size);
    }
    if (std::strcmp(key, kKeySound) == 0) {
        return copyValue(m_sound.load() ? "1" : "0", buf, buf_size);
    }
    return -1;  // 不认识的 key：安全忽略（宿主升级后多传一项也不会踩这里）
}

std::int32_t NotifyConfig::setValue(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        m_enabled.store(parseBool(value));
        return 0;
    }
    if (std::strcmp(key, kKeyShowOnSuccess) == 0) {
        m_showOnSuccess.store(parseBool(value));
        return 0;
    }
    if (std::strcmp(key, kKeyShowOnError) == 0) {
        m_showOnError.store(parseBool(value));
        return 0;
    }
    if (std::strcmp(key, kKeySound) == 0) {
        m_sound.store(parseBool(value));
        return 0;
    }
    return -1;
}

bool NotifyConfig::enabled() const noexcept {
    return m_enabled.load();
}

bool NotifyConfig::showOnSuccess() const noexcept {
    return m_showOnSuccess.load();
}

bool NotifyConfig::showOnError() const noexcept {
    return m_showOnError.load();
}

bool NotifyConfig::soundEnabled() const noexcept {
    return m_sound.load();
}

std::int32_t NotifyConfig::copyValue(const char* value, char* buf, std::uint32_t buf_size) {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    // 契约：buf_size 大于返回值才写入（含结尾 NUL）；装不下只报长度，不算失败。
    if (buf != nullptr && buf_size > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

std::int32_t NotifyConfig::copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (src == nullptr || out == nullptr) {
        return 1;
    }
    // 老宿主的结构可能比本插件认识的短：整段拷贝前必须确认它装得下完整声明，
    // 否则 *out = *src 会写到宿主缓冲区之外（与 tray / clipboard 的约定一致）。
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = *src;
    return 0;
}

}  // namespace notify
