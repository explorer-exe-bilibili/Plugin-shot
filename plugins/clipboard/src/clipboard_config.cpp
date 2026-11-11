// 两个配置项：enabled（写不写剪贴板）与 also_file_drop（要不要再放文件列表）。
//
// 跨边界一律用字符串（契约：只传定宽类型 + const char*），本文件负责字符串与内部
// 布尔的转换：只把 "0" / "false" 当"关"，其它写法按"开"处理——配置来自可被手改的
// 文件，把用户本想打开的开关意外关掉，比多写一次剪贴板更烦人。
//
// label / tooltip 写的是语言表 key，不是中文：设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析，文案本体在
// clipboard_plugin.cpp 的 kLangTables 里（两边的 key 拼写必须严格一致）。
//
// 返回码约定（include/plugin_api.h 的 ShotConfigurableV1）：get 返回 >= 0 是值长度
// （不含 NUL），负数只表示 key 不认识或参数非法；set 返回 0 = 已生效。

#include "clipboard_config.h"

#include <cstring>
#include <iterator>

namespace clipboard {
namespace {

const char kKeyEnabled[] = "enabled";
const char kKeyAlsoFileDrop[] = "also_file_drop";

// 默认值只在描述符里写一遍：构造函数也从这两个常量取初始值，避免"描述符默认值"
// 与"初始值"两处漂移。
const char kDefaultEnabled[] = "1";
const char kDefaultAlsoFileDrop[] = "0";

// 文案 key：命名规范 config.<配置 key>.label / config.<配置 key>.tooltip，
// 写成常量是为了让 kConfig 一眼能扫出"这里放的是 key 而不是中文"。
const char kLabelEnabled[] = "config.enabled.label";
const char kTooltipEnabled[] = "config.enabled.tooltip";
const char kLabelAlsoFileDrop[] = "config.also_file_drop.label";
const char kTooltipAlsoFileDrop[] = "config.also_file_drop.tooltip";

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
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyAlsoFileDrop,
        .label         = kLabelAlsoFileDrop,
        .default_value = kDefaultAlsoFileDrop,
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = kTooltipAlsoFileDrop,
    },
};

/**
 * @brief 把配置里的布尔文本解析成 bool。
 *
 * 只把 "0" / "false" 当成关：宿主升级后可能传别的写法（"off" 之类），按"不认识
 * 就当默认值（开）"处理。
 *
 * @param[in] value 配置值文本，不能为空。
 * @return true = 开；false = 关（含 "0" / "false"）。
 * @note 无状态的纯解析函数，不属于 ClipboardConfig 的接口，所以放匿名命名空间
 *       （STYLE.md §6.7 的合法例外）。
 */
bool parseBool(const char* value) {
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

}  // namespace

ClipboardConfig::ClipboardConfig()
    : m_enabled(parseBool(kDefaultEnabled)), m_alsoFileDrop(parseBool(kDefaultAlsoFileDrop)) {}

std::uint32_t ClipboardConfig::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t ClipboardConfig::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t ClipboardConfig::getValue(const char* key, char* buf, std::uint32_t buf_size) const {
    if (key == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        return copyValue(m_enabled.load() ? "1" : "0", buf, buf_size);
    }
    if (std::strcmp(key, kKeyAlsoFileDrop) == 0) {
        return copyValue(m_alsoFileDrop.load() ? "1" : "0", buf, buf_size);
    }
    return -1;  // 不认识的 key：安全忽略（宿主升级后多传一项也不会踩这里）
}

std::int32_t ClipboardConfig::setValue(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        m_enabled.store(parseBool(value));
        return 0;
    }
    if (std::strcmp(key, kKeyAlsoFileDrop) == 0) {
        m_alsoFileDrop.store(parseBool(value));
        return 0;
    }
    return -1;  // 不认识的 key：安静忽略
}

bool ClipboardConfig::enabled() const noexcept {
    return m_enabled.load();
}

bool ClipboardConfig::alsoFileDrop() const noexcept {
    return m_alsoFileDrop.load();
}

std::int32_t ClipboardConfig::copyValue(const char* value, char* buf, std::uint32_t buf_size) {
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

std::int32_t ClipboardConfig::copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (out == nullptr || src == nullptr) {
        return 1;
    }
    // 老宿主的结构可能比本插件认识的短：按 struct_size 判断能写到哪，绝不越界。
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = *src;
    return 0;
}

}  // namespace clipboard
