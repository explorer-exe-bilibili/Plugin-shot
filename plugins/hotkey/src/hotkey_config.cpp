// 全局热键的配置项：enabled + 三个热键字符串。
//
// 配置项声明、字符串值解析后的当前值读写都在这个文件里；ABI 回调在
// plugin_entry.cpp 只做参数转发。值一律按契约用字符串表达：
//   · enabled          → "1" / "0"
//   · capture_hotkey   → "Ctrl+Alt+S" 这类文本（语法见 hotkey_binding.h）
//   · region_hotkey    → 同上
//   · settings_hotkey  → 同上
//
//  三个热键项的 ShotConfigDesc.options 写 "hotkey"：设置页据此把 STRING 渲染成
//  "按组合键录入"控件（document/LISTS.md §5.1、document/SETTINGS.md §6）。
//  options 只是给设置页的约定，值语法 / 解析 / 存值都不变。
//
// 线程模型：set_config 可能在宿主任意线程上被调用（init 的 apply_stored、设置页
// 应用），热键线程同时可能正在读这三个值做重注册；所以字符串用 mutex 保护、
// 布尔开关用 std::atomic，读侧永远拿到完整值。

#include "hotkey_config.h"

#include <cstring>
#include <iterator>
#include <string>

namespace hotkey {
namespace {

// 配置键：全小写 + 下划线，发布后不能改名（它是存盘标识，见 ABI_POLICY.md §5）。
const char kKeyEnabled[] = "enabled";
const char kKeyCaptureHotkey[] = "capture_hotkey";
const char kKeyRegionHotkey[] = "region_hotkey";
const char kKeySettingsHotkey[] = "settings_hotkey";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyEnabled,
        .label         = "启用全局热键",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "关闭后撤销全部热键注册；热键线程与窗口保留，重新启用立即生效",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_STRING,
        .key           = kKeyCaptureHotkey,
        .label         = "截屏热键",
        .default_value = "Ctrl+Alt+S",
        .options       = "hotkey",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "发布 capture.request（空负载）；语法 Ctrl/Alt/Shift/Win + A-Z / 0-9 / F1-F12 / PrintScreen，大小写不敏感",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_STRING,
        .key           = kKeyRegionHotkey,
        .label         = "区域截屏热键",
        .default_value = "Ctrl+Alt+A",
        .options       = "hotkey",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "发布 capture.request，负载 {\"mode\":\"region\"}；语法同上",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_STRING,
        .key           = kKeySettingsHotkey,
        .label         = "打开设置热键",
        .default_value = "Ctrl+Alt+O",
        .options       = "hotkey",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "发布 settings.open（空负载）；语法同上",
    },
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

HotkeyConfig::HotkeyConfig()
    : m_captureHotkey("Ctrl+Alt+S"),
      m_regionHotkey("Ctrl+Alt+A"),
      m_settingsHotkey("Ctrl+Alt+O") {}

std::uint32_t HotkeyConfig::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t HotkeyConfig::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t HotkeyConfig::getValue(const char* key, char* buf, std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        return copyValue(m_enabled.load() ? "1" : "0", buf, bufSize);
    }
    // 三个热键共用一把锁：锁内拷一份副本，出锁后再写宿主缓冲区。
    std::string value;
    {
        std::lock_guard<std::mutex> lock(m_hotkeyMutex);
        if (std::strcmp(key, kKeyCaptureHotkey) == 0) {
            value = m_captureHotkey;
        } else if (std::strcmp(key, kKeyRegionHotkey) == 0) {
            value = m_regionHotkey;
        } else if (std::strcmp(key, kKeySettingsHotkey) == 0) {
            value = m_settingsHotkey;
        } else {
            return -1;  // 不认识的 key：安全忽略（宿主升级后多传一项也不会踩这里）
        }
    }
    return copyValue(value.c_str(), buf, bufSize);
}

std::int32_t HotkeyConfig::setValue(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        m_enabled.store(parseBool(value));
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(m_hotkeyMutex);
        if (std::strcmp(key, kKeyCaptureHotkey) == 0) {
            m_captureHotkey = value;
            return 0;
        }
        if (std::strcmp(key, kKeyRegionHotkey) == 0) {
            m_regionHotkey = value;
            return 0;
        }
        if (std::strcmp(key, kKeySettingsHotkey) == 0) {
            m_settingsHotkey = value;
            return 0;
        }
    }
    return -1;
}

bool HotkeyConfig::enabled() const noexcept {
    return m_enabled.load();
}

std::string HotkeyConfig::captureHotkey() const {
    std::lock_guard<std::mutex> lock(m_hotkeyMutex);
    return m_captureHotkey;
}

std::string HotkeyConfig::regionHotkey() const {
    std::lock_guard<std::mutex> lock(m_hotkeyMutex);
    return m_regionHotkey;
}

std::string HotkeyConfig::settingsHotkey() const {
    std::lock_guard<std::mutex> lock(m_hotkeyMutex);
    return m_settingsHotkey;
}

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
