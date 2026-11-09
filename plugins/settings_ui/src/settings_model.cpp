// settings_model —— 设置页的数据模型：从宿主列表服务的 config_list JSON 与插件注册表
// 重建本地编辑缓冲；同时提供跨 .cpp 共用的纯值工具（解析/格式化/颜色 hex/默认值）。
//
// 这里不碰 ImGui、不碰窗口/平台 API：模型只有字符串与脏标记，渲染在 settings_form.cpp，
// 提交在 settings_apply.cpp。
//
// config_list 的条目 schema（document/LISTS.md，宿主生成）：
//   {"owner","owner_name","id","label","kind","value","default",
//    "options","min","max","step","tooltip"}
// 宿主按 owner 分组、按它自己的顺序返回；本文件再把它们收成 ProviderSection 模型。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "settings_panel_impl.h"
#include "shot_plugin_helpers.h"

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

// 下面三个 json* 小工具只服务 parseConfigListJson（把 config_list 条目读成本地副本），
// 与任何类的状态无关，所以放匿名命名空间（STYLE.md §6.7）。

/**
 * @brief 读一个"字符串语义"的 JSON 字段。
 *
 * schema 规定的 value/default/options/tooltip 都是字符串；但宿主序列化版本不同、
 * 或者手写测试数据时会出现数字/布尔，这里统一宽容地转成字符串，不让整个列表失败。
 */
std::string jsonText(const nlohmann::json& object, const char* field) {
    const auto it = object.find(field);
    if (it == object.end() || it->is_null()) {
        return {};
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_boolean()) {
        return it->get<bool>() ? "1" : "0";
    }
    if (it->is_number()) {
        return it->dump();  // 交给 nlohmann 输出最短的十进制文本
    }
    return {};
}

/** @brief 读一个数值 JSON 字段；缺失或类型不是数字时返回 fallback。 */
double jsonNumber(const nlohmann::json& object, const char* field, double fallback) {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_number()) {
        return fallback;
    }
    return it->get<double>();
}

/** @brief 读 kind 字段（SHOT_CONFIG_KIND_*）；兼容数字与数字字符串，非法时返回 0。 */
std::uint32_t jsonKind(const nlohmann::json& object) {
    const auto it = object.find("kind");
    if (it == object.end()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        return it->get<std::uint32_t>();
    }
    if (it->is_number_integer()) {
        const long long value = it->get<long long>();
        return (value > 0) ? static_cast<std::uint32_t>(value) : 0;
    }
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (end != nullptr && end != text.c_str() && *end == '\0' && value > 0) {
            return static_cast<std::uint32_t>(value);
        }
    }
    return 0;
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

/** @brief 把 0..1 的 RGBA 统一写成 "#RRGGBBAA"（SETTINGS.md / LISTS.md 规定的存盘格式）。 */
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
 * @brief 声明里没写 default 时给一个类型上合理的默认值。
 *
 * BOOL 给 "0"、INT/FLOAT 给下界，避免「恢复默认」把空串写进插件。
 */
std::string SettingsPanel::Impl::defaultValueFor(const ConfigDesc& desc) {
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

PathPicker SettingsPanel::Impl::pathPickerFor(const ConfigDesc& desc) {
    // options 对 ENUM 是候选列表（"png|jpeg"），对 PATH 借用来声明对话框类型：
    // ABI 里没有独立的"文件/目录"字段，而为它加一个可选字段不值得（数据形状不进 ABI）。
    // 这里只做大小写无关的关键词匹配，认不出就当没声明，交给调用方按当前值推断。
    std::string hint;
    hint.reserve(desc.options.size());
    for (const char c : desc.options) {
        hint.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
    }
    if (hint == "directory" || hint == "dir" || hint == "folder") {
        return PathPicker::Directory;
    }
    if (hint == "file") {
        return PathPicker::File;
    }
    return PathPicker::Auto;
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

bool SettingsPanel::Impl::parseConfigListJson(std::string_view text,
                                              std::vector<ProviderSection>& out) {
    out.clear();
    if (text.empty()) {
        return false;
    }
    const nlohmann::json document =
        nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_array()) {
        return false;
    }
    for (const nlohmann::json& entry : document) {
        if (!entry.is_object()) {
            continue;
        }
        const std::string owner = jsonText(entry, "owner");
        const std::string id = jsonText(entry, "id");
        // owner/id 是事件与存盘键的两段身份，缺一个就没法提交；跳过这一项，别让整页失败。
        if (owner.empty() || id.empty()) {
            continue;
        }
        std::size_t sectionIndex = out.size();
        for (std::size_t index = 0; index < out.size(); ++index) {
            if (out[index].id == owner) {
                sectionIndex = index;
                break;
            }
        }
        if (sectionIndex == out.size()) {
            ProviderSection section;
            section.id = owner;
            section.name = jsonText(entry, "owner_name");
            if (section.name.empty()) {
                section.name = owner;
            }
            out.push_back(std::move(section));
        }
        ProviderSection& section = out[sectionIndex];

        ConfigItem item;
        item.desc.kind = jsonKind(entry);
        item.desc.key = id;
        item.desc.label = jsonText(entry, "label");
        if (item.desc.label.empty()) {
            item.desc.label = id;
        }
        item.desc.defaultValue = jsonText(entry, "default");
        item.desc.options = jsonText(entry, "options");
        item.desc.min = jsonNumber(entry, "min", 0.0);
        item.desc.max = jsonNumber(entry, "max", 0.0);
        item.desc.step = jsonNumber(entry, "step", 0.0);
        item.desc.tooltip = jsonText(entry, "tooltip");
        const auto valueIt = entry.find("value");
        item.value = (valueIt != entry.end() && !valueIt->is_null())
                         ? jsonText(entry, "value")
                         : item.desc.defaultValue;
        item.baseline = item.value;
        if (item.desc.kind == SHOT_CONFIG_KIND_ENUM) {
            item.enumOptions = splitOptions(item.desc.options);
        }
        syncColorHex(item);
        section.items.push_back(std::move(item));
    }
    return true;
}

void SettingsPanel::Impl::mergeDirtyValues(const std::vector<ProviderSection>& previous,
                                           std::vector<ProviderSection>& sections) {
    for (const ProviderSection& oldSection : previous) {
        if (oldSection.removed) {
            continue;  // 已卸载节的旧值只留给 removedSections 渲染，不参与新模型
        }
        for (const ConfigItem& oldItem : oldSection.items) {
            // 只有"用户改过没应用"或"上次被拒绝"的项才需要保住本地状态；干净的项用
            // 新读到的基线，否则刷新会把宿主的当前值覆盖回旧值。
            const bool hasLocalEdit = oldItem.value != oldItem.baseline;
            if (!hasLocalEdit && oldItem.error.empty() && oldItem.hint.empty()) {
                continue;
            }
            ConfigItem* target = nullptr;
            for (ProviderSection& section : sections) {
                if (section.id != oldSection.id) {
                    continue;
                }
                for (ConfigItem& item : section.items) {
                    if (item.desc.key == oldItem.desc.key) {
                        target = &item;
                        break;
                    }
                }
            }
            if (target == nullptr) {
                continue;  // 这一项在新列表里没了（插件删了声明）：本地修改只能丢弃
            }
            target->value = oldItem.value;
            target->error = oldItem.error;
            target->hint = oldItem.hint;
            target->colorHex = oldItem.colorHex;
        }
    }
}

void SettingsPanel::Impl::refresh() {
    refreshConfig();
    refreshPlugins();
}

void SettingsPanel::Impl::refreshConfig() {
    // 先记下旧模型：刷新后消失的提供者多半是刚被热卸载，渲染成"已卸载"灰节；
    // 还在的提供者要把未应用的本地修改合并回去。
    std::vector<ProviderSection> previous = std::move(sections);
    sections.clear();
    removedSections.clear();
    configUnavailable = false;
    configUnavailableReason.clear();

    if (host == nullptr) {
        configUnavailable = true;
        configUnavailableReason = "宿主服务表为空：当前读不到配置目录。";
        return;
    }
    // 列表服务是 ABI 2.0 才有的：老宿主没有它时安静降级，只剩插件管理。
    shot::PluginLists lists(host);
    if (!lists.available()) {
        configUnavailable = true;
        configUnavailableReason = "宿主不支持列表服务（shot.host.plugin_lists）："
                                  "当前宿主不能读插件配置，设置页只剩插件管理。";
        return;
    }
    const std::string text = lists.json("config_list");
    if (text.empty()) {
        configUnavailable = true;
        configUnavailableReason = "宿主暂未提供 config_list：当前没有可读的配置项。";
        return;
    }
    if (!parseConfigListJson(text, sections)) {
        configUnavailable = true;
        configUnavailableReason = "config_list 不是合法的 JSON 数组：配置区暂时不可用（详见宿主日志）。";
        shot::Logger(host).warn("settings_ui：config_list 解析失败（不是合法 JSON 数组）");
        return;
    }
    mergeDirtyValues(previous, sections);

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

bool SettingsPanel::Impl::hasPendingConfigChanges() const {
    for (const ProviderSection& section : sections) {
        if (section.removed) {
            continue;
        }
        for (const ConfigItem& item : section.items) {
            // 被拒绝过的项也算"未收尾"：否则用户把值改回基线后，红色标记再也清不掉
            // （「应用」会变灰，而「取消」也跟着灰）。
            if (item.value != item.baseline || !item.error.empty()) {
                return true;
            }
        }
    }
    return false;
}

bool SettingsPanel::Impl::hasPendingPluginChanges() const {
    for (const PluginRow& row : plugins) {
        if (row.enabledPending != row.enabledNow) {
            return true;
        }
    }
    return false;
}

bool SettingsPanel::Impl::hasPendingChanges() const {
    return hasPendingConfigChanges() || hasPendingPluginChanges();
}

ConfigItem* SettingsPanel::Impl::findConfigItem(const std::string& owner,
                                                const std::string& key) {
    for (ProviderSection& section : sections) {
        if (section.id != owner) {
            continue;
        }
        for (ConfigItem& item : section.items) {
            if (item.desc.key == key) {
                return &item;
            }
        }
    }
    return nullptr;
}

const std::string* SettingsPanel::Impl::findPendingChangeValue(const std::string& owner,
                                                               const std::string& key) const {
    for (const PendingConfigChange& change : pendingChanges) {
        if (change.owner == owner && change.key == key) {
            return &change.value;
        }
    }
    return nullptr;
}

}  // namespace settings_ui
