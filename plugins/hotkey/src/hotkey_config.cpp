// 热键的配置项：固定 enabled + hotkey_list 动态条目。
//
// 动态项的声明每调用一次现算（条目数、key、label、default 都来自 HotkeyCatalog），
// 这样列表变化后设置页下一次枚举就能看到新条目，不需要重建插件实例。
// 值仍然按旧机制存在宿主配置 hotkey.main/<id> 里，本文件只做转发。

#include "hotkey_config.h"

#include <cstring>
#include <string>
#include <vector>

#include "hotkey_catalog.h"

namespace hotkey {
namespace {

// 唯一的静态配置项：总开关。三个热键不再是静态项——它们由 hotkey.main 自己
// 贡献到 hotkey_list，再由本类的动态声明路径暴露给设置页。
const char kKeyEnabled[] = "enabled";

// label / tooltip 存的是文案 key，不是最终显示文本（设置页按 display(owner,key)
// 解析；owner = "hotkey.main"，语言表在 plugin_entry.cpp 的 kLangTables）。
const ShotConfigDesc kEnabledDesc = {
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
};

/**
 * @brief 把配置里的布尔文本解析成 bool。
 *
 * 只把 "0" / "false" 当成关：宿主升级后可能传别的写法（"off" 之类），按
 * "不认识就当默认值（开）"处理，比把用户配置的开关意外关掉安全。
 *
 * @param[in] value 配置值文本，不能为空。
 * @return true = 开；false = 关（含 "0" / "false"）。
 * @note 无状态的纯解析函数，不属于 HotkeyConfig 的接口，所以放匿名命名空间（STYLE §6.7）。
 */
bool parseBool(const char* value) {
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

}  // namespace

HotkeyConfig::HotkeyConfig(HotkeyCatalog& catalog) : m_catalog(catalog) {}

std::uint32_t HotkeyConfig::configCount() const noexcept {
    // 枚举前刷新：见头文件注释（同一轮 tick 里 lists.add 已处理、lists.changed 还没发）。
    // 刷新失败只保留旧快照，不影响宿主继续枚举；绝不能把异常抛给宿主。
    try {
        m_catalog.reload();
    } catch (...) {
    }
    return 1u + m_catalog.entryCount();
}

std::int32_t HotkeyConfig::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (out == nullptr) {
        return 1;
    }
    if (index == 0) {
        return copyDesc(&kEnabledDesc, out);
    }
    const std::vector<HotkeyEntry> entries = m_catalog.entries();
    const std::size_t entryIndex = static_cast<std::size_t>(index) - 1;
    if (entryIndex >= entries.size()) {
        return 1;
    }
    const HotkeyEntry& entry = entries[entryIndex];
    // m_desc* 是返回值里指针的宿主：宿主一次只问一项，下一次调用前这些指针有效。
    std::lock_guard<std::mutex> lock(m_descMutex);
    m_descKey = entry.id;
    m_descLabel = entry.label.empty() ? entry.id : entry.label;
    m_descDefault = entry.defaultCombo;
    const ShotConfigDesc desc = {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_STRING,
        .key           = m_descKey.c_str(),
        .label         = m_descLabel.c_str(),
        .default_value = m_descDefault.c_str(),
        .options       = "hotkey",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "",
    };
    return copyDesc(&desc, out);
}

std::int32_t HotkeyConfig::getValue(const char* key, char* buf, std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        return copyValue(m_enabled.load() ? "1" : "0", buf, bufSize);
    }
    HotkeyEntry entry;
    if (!m_catalog.readConfigValue(key, entry)) {
        return -1;  // 不认识的 key：安全忽略（列表已变化时宿主可能多问一项）
    }
    return copyValue(entry.value.c_str(), buf, bufSize);
}

std::int32_t HotkeyConfig::setValue(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        m_enabled.store(parseBool(value));
        return 0;
    }
    return m_catalog.writeConfigValue(key, value) ? 0 : -1;
}

bool HotkeyConfig::enabled() const noexcept { return m_enabled.load(); }

std::int32_t HotkeyConfig::copyValue(const char* value, char* buf, std::uint32_t bufSize) {
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

std::int32_t HotkeyConfig::copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
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

}  // namespace hotkey
