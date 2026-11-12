// 会话控制插件唯一的配置项：enabled（默认开）。
//
// 只负责声明与存值，不注册 D-Bus：值一律按契约用字符串表达（"1" / "0"）。
// label / tooltip 存的是文案 key，不是最终显示文本：设置页按
// display("session_control.main", key) 到 i18n_list 里取当前语言（语言表在
// session_control_plugin.cpp 的 kLangTables）。
//
// 返回码约定（include/plugin_api/configurable.h）：get 返回 >= 0 是值长度
// （不含 NUL），负数只表示 key 不认识或参数非法；set 返回 0 = 已生效。

#include "session_control_config.h"

#include <cstring>
#include <iterator>

namespace session_control {
namespace {

// 配置键：全小写 + 下划线，发布后不能改名（它是存盘标识，见 ABI_POLICY.md §5）。
// 存盘/投影时宿主会加插件 id 前缀：session_control.main/enabled。
const char kKeyEnabled[] = "enabled";
const char kDefaultEnabled[] = "1";

// 文案 key：命名规范 config.<配置 key>.label / config.<配置 key>.tooltip，
// 写成常量是为了让 kConfig 一眼能扫出"这里放的是 key 而不是中文"。
const char kLabelEnabled[] = "config.enabled.label";
const char kTooltipEnabled[] = "config.enabled.tooltip";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyEnabled,
        .label         = kLabelEnabled,
        .default_value = kDefaultEnabled,
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = kTooltipEnabled,
    },
};

/**
 * @brief 把配置里的布尔文本解析成 bool。
 *
 * 只把 "0" / "false" 当成关：宿主升级后可能传别的写法（"off" 之类），按
 * "不认识就当默认值（开）"处理，比把用户显式打开的开关意外关掉更安全。
 *
 * @param[in] value 配置值文本，不能为空。
 * @return true = 开；false = 关（含 "0" / "false"）。
 * @note 无状态的纯解析函数，不属于 SessionControlConfig 的接口，所以放匿名
 *       命名空间（STYLE §6.7 的合法例外）。
 */
bool parseBool(const char* value) {
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

}  // namespace

SessionControlConfig::SessionControlConfig() = default;

std::uint32_t SessionControlConfig::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t SessionControlConfig::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t SessionControlConfig::getValue(const char* key, char* buf,
                                            std::uint32_t buf_size) const {
    if (key == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        return copyValue(m_enabled.load() ? "1" : "0", buf, buf_size);
    }
    return -1;  // 不认识的 key：安全忽略（宿主升级后多传一项也不会踩这里）
}

std::int32_t SessionControlConfig::setValue(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        m_enabled.store(parseBool(value));
        return 0;
    }
    return -1;
}

bool SessionControlConfig::enabled() const noexcept {
    return m_enabled.load();
}

std::int32_t SessionControlConfig::copyValue(const char* value, char* buf,
                                             std::uint32_t buf_size) {
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

std::int32_t SessionControlConfig::copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
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

}  // namespace session_control
