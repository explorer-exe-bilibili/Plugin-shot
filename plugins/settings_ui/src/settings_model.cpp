// settings_model —— 设置页的数据模型：从宿主服务读出配置目录与插件注册表，
// 重建本地编辑缓冲；同时提供跨 .cpp 共用的纯值工具（解析/格式化/颜色 hex/默认值）。
//
// 这里不碰 ImGui、不碰 Win32：模型只有字符串与脏标记，渲染在 settings_form.cpp，
// 提交在 settings_apply.cpp。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "settings_panel_impl.h"

namespace settings_ui {
namespace {

// 十六进制数字 -> 0..15；不是十六进制返回 -1。只服务 parseColorValue，所以留在
// 本 TU 的匿名命名空间，不占类的接口。

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

bool SettingsPanel::Impl::parseBoolValue(std::string_view text) {
    // 契约规定 BOOL 存 "1"/"0"；true/false 也认，宿主/插件手写配置时更宽容。
    return text == "1" || text == "true" || text == "TRUE" || text == "yes";
}

int SettingsPanel::Impl::parseIntValue(std::string_view text, int fallback) {
    const std::string buffer(text);
    if (buffer.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(buffer.c_str(), &end, 10);
    if (end == nullptr || end == buffer.c_str() || *end != '\0') {
        return fallback;
    }
    return static_cast<int>(value);
}

double SettingsPanel::Impl::parseDoubleValue(std::string_view text, double fallback) {
    const std::string buffer(text);
    if (buffer.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == nullptr || end == buffer.c_str() || *end != '\0') {
        return fallback;
    }
    return value;
}

std::string SettingsPanel::Impl::formatIntValue(int value) {
    return std::to_string(value);
}

std::string SettingsPanel::Impl::formatDoubleValue(double value) {
    // %.6g 兼顾精度和可读性：0.1 -> "0.1"，100 -> "100"，不会写成 "100.000000"。
    char buffer[32] = {};
    const int written = std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    if (written <= 0) {
        return std::string();
    }
    return std::string(buffer, static_cast<std::size_t>(written));
}

/**
 * @brief 解析颜色字符串，统一转成 0..1 的 RGBA。
 *
 * 兼容两种存盘格式：#RRGGBBAA / #RRGGBB（alpha 补 FF）与 "R,G,B,A"（0..255）。
 * 解析失败返回 false，由调用方回退成黑色——不要让一个坏值把整页渲染拖崩。
 */
bool SettingsPanel::Impl::parseColorValue(std::string_view text, float out[4]) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
    if (text.empty()) {
        return false;
    }
    if (text.front() == '#') {
        const std::string_view hex = text.substr(1);
        if (hex.size() != 6 && hex.size() != 8) {
            return false;
        }
        std::uint32_t rgba = 0;
        for (const char c : hex) {
            const int digit = hexDigit(c);
            if (digit < 0) {
                return false;
            }
            rgba = (rgba << 4) | static_cast<std::uint32_t>(digit);
        }
        if (hex.size() == 6) {
            rgba = (rgba << 8) | 0xFFu;  // "#RRGGBB" 视为不透明
        }
        out[0] = static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f;
        out[1] = static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f;
        out[2] = static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f;
        out[3] = static_cast<float>(rgba & 0xFFu) / 255.0f;
        return true;
    }
    // "R,G,B,A"：sscanf 的空白匹配会吃掉逗号两侧的空格；少写 alpha 时按不透明处理。
    const std::string buffer(text);
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 255.0;
    const int count = std::sscanf(buffer.c_str(), "%lf , %lf , %lf , %lf", &r, &g, &b, &a);
    if (count < 3) {
        return false;
    }
    out[0] = static_cast<float>(std::clamp(r, 0.0, 255.0) / 255.0);
    out[1] = static_cast<float>(std::clamp(g, 0.0, 255.0) / 255.0);
    out[2] = static_cast<float>(std::clamp(b, 0.0, 255.0) / 255.0);
    out[3] = static_cast<float>(std::clamp(a, 0.0, 255.0) / 255.0);
    return true;
}

/** @brief 把 0..1 的 RGBA 统一写成 "#RRGGBBAA"（SETTINGS.md §6 规定的存盘格式）。 */
std::string SettingsPanel::Impl::formatColorHex(const float color[4]) {
    const int r = static_cast<int>(std::lround(std::clamp(color[0], 0.0f, 1.0f) * 255.0f));
    const int g = static_cast<int>(std::lround(std::clamp(color[1], 0.0f, 1.0f) * 255.0f));
    const int b = static_cast<int>(std::lround(std::clamp(color[2], 0.0f, 1.0f) * 255.0f));
    const int a = static_cast<int>(std::lround(std::clamp(color[3], 0.0f, 1.0f) * 255.0f));
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", r, g, b, a);
    return buffer;
}

/** @brief 按 '|' 切分 ENUM 的候选列表（契约规定 options 就是这个格式）。 */
std::vector<std::string> SettingsPanel::Impl::splitOptions(std::string_view options) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = options.find('|', start);
        const std::size_t length =
            (end == std::string_view::npos) ? options.size() - start : end - start;
        if (length > 0) {
            result.emplace_back(options.substr(start, length));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

/**
 * @brief 声明里没写 default_value 时给一个类型上合理的默认值。
 *
 * BOOL 给 "0"、INT/FLOAT 给下界，避免「恢复默认」把空串写进插件。
 */
std::string SettingsPanel::Impl::defaultValueFor(const shot::ConfigDesc& desc) {
    if (!desc.defaultValue.empty()) {
        return desc.defaultValue;
    }
    switch (desc.kind) {
        case SHOT_CONFIG_KIND_BOOL:
            return "0";
        case SHOT_CONFIG_KIND_INT:
            return formatIntValue(static_cast<int>(desc.min));
        case SHOT_CONFIG_KIND_FLOAT:
            return formatDoubleValue(desc.min);
        default:
            return std::string();
    }
}

/**
 * @brief 把 COLOR 项当前值同步进十六进制编辑缓冲；其它 kind 是空操作。
 *
 * 三处会改 COLOR 的值（读取模型 / 恢复默认 / 取消），刷新缓冲的逻辑只有这一份。
 *
 * @param[in,out] item 配置项；只改 colorHex 的展示形态，不改语义。
 */
void SettingsPanel::Impl::syncColorHex(ConfigItem& item) {
    if (item.desc.kind != SHOT_CONFIG_KIND_COLOR) {
        return;
    }
    float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    item.colorHex = parseColorValue(item.value, color) ? formatColorHex(color) : "#00000000";
}

void SettingsPanel::Impl::refresh() {
    refreshConfig();
    refreshPlugins();
}

void SettingsPanel::Impl::refreshConfig() {
    // 先记下旧模型里还在的提供者：刷新后消失的多半是刚被热卸载，渲染成"已卸载"灰节。
    std::vector<ProviderSection> previous = std::move(sections);
    sections.clear();
    removedSections.clear();
    configUnavailable = false;

    if (host == nullptr) {
        configUnavailable = true;
        return;
    }
    shot::PluginConfig config(host);
    if (!config.available()) {
        configUnavailable = true;
        return;
    }

    const std::uint32_t providerCount = config.configurableCount();
    for (std::uint32_t index = 0; index < providerCount; ++index) {
        // configurable_info / config_desc 返回的字符串归提供者所有，插件被卸载后失效，
        // 所以这里逐个当场拷进 std::string（shot::ConfigDesc 已经这么做了）。
        ShotPluginInfo info{};
        if (!config.configurableInfo(index, info)) {
            continue;  // 身份都读不到：跳过这个提供者，别让整个页面失败
        }
        ProviderSection section;
        section.id = (info.id != nullptr) ? info.id : "";
        if (section.id.empty()) {
            continue;
        }
        section.name = (info.name != nullptr && info.name[0] != '\0') ? info.name : section.id;
        section.version = (info.version != nullptr) ? info.version : "";

        const std::uint32_t itemCount = config.configCount(section.id.c_str());
        for (std::uint32_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
            shot::ConfigDesc desc;
            if (!config.configDesc(section.id.c_str(), itemIndex, desc)) {
                continue;  // 这一项声明坏了：跳过它，别让整节失败
            }
            ConfigItem item;
            item.desc = std::move(desc);
            item.value = config.getString(section.id.c_str(), item.desc.key.c_str(),
                                          item.desc.defaultValue);
            item.baseline = item.value;
            if (item.desc.kind == SHOT_CONFIG_KIND_ENUM) {
                item.enumOptions = splitOptions(item.desc.options);
            }
            syncColorHex(item);
            section.items.push_back(std::move(item));
        }
        sections.push_back(std::move(section));
    }

    for (ProviderSection& oldSection : previous) {
        const bool stillThere = std::any_of(
            sections.begin(), sections.end(),
            [&oldSection](const ProviderSection& section) { return section.id == oldSection.id; });
        if (!stillThere) {
            oldSection.removed = true;
            removedSections.push_back(std::move(oldSection));
        }
    }
}

void SettingsPanel::Impl::refreshPlugins() {
    plugins.clear();
    registryUnavailable = false;
    if (host == nullptr) {
        registryUnavailable = true;
        return;
    }
    shot::PluginRegistry registry(host);
    if (!registry.available()) {
        registryUnavailable = true;
        return;
    }
    const std::uint32_t count = registry.installedCount();
    for (std::uint32_t index = 0; index < count; ++index) {
        shot::PluginStatus status;
        if (!registry.installedInfo(index, status)) {
            continue;
        }
        PluginRow row;
        row.status = std::move(status);
        // isEnabled 返回 -1 表示宿主不认识这个 id（理论上不发生在已安装列表里），
        // 那按"没加载"处理，别把一个未知状态显示成可勾选的真值。
        row.enabledNow = (registry.isEnabled(row.status.id.c_str()) == 1);
        row.enabledPending = row.enabledNow;
        plugins.push_back(std::move(row));
    }
}

bool SettingsPanel::Impl::hasPendingChanges() const {
    for (const ProviderSection& section : sections) {
        for (const ConfigItem& item : section.items) {
            if (item.value != item.baseline) {
                return true;
            }
        }
    }
    for (const PluginRow& row : plugins) {
        if (row.enabledPending != row.enabledNow) {
            return true;
        }
    }
    return false;
}

}  // namespace settings_ui
