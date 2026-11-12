// hotkey_list 的读取、内置贡献与动态配置值仓库。
//
// 列表驱动之后，热键的"数据"与"注册"分开：
//   · 数据：任何插件（含 hotkey.main 自己）用 lists.add 向 hotkey_list 贡献条目；
//     本文件在 create 阶段订阅 lists.changed，并按快照刷新注册表用的模型；
//   · 注册：HotkeyWindow 只读本类的快照，不直接碰列表服务（依赖方向单一）。
//
// 实际组合键仍存在宿主配置 hotkey.main/<id> 里，与旧版 shot.configurable 的
// 存盘键完全一致（设置页 lists.update -> set_config -> 宿主写 hotkey.main/<id>），
// 所以这里不需要新存储，也不自己写 config.json。

#include "hotkey_catalog.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "shot_plugin_helpers.h"

namespace hotkey {
namespace {

const char kHotkeyMainId[] = "hotkey.main"; /**< 本 provider 的插件 id，也是内置条目的 owner。 */
const char kHotkeyListId[] = "hotkey_list"; /**< 列表驱动热键的 list id。 */
const char kListsChangedTopic[] = "lists.changed"; /**< 宿主 → 所有：列表内容变化。 */

/**
 * @brief "配置键不存在"的哨兵值。
 *
 * shot::Config::getString 用 fallback 表达"没有这个键"，但空串是热键的合法值
 * （空串 = 这项不注册），所以不能用空串当 fallback。哨兵里带控制字符，正常
 * 组合键不可能等于它。
 */
const char kMissingConfigValue[] = "\x01hotkey.missing";

/** @brief 旧版静态配置键 → 新条目 id（迁移只写新键，旧键留给回滚）。 */
struct LegacyMapping {
    const char* oldKey; /**< 旧配置键，如 "hotkey.main/capture_hotkey"。 */
    const char* newId;  /**< 新条目 id，如 "capture"。 */
};

const LegacyMapping kLegacyMappings[] = {
    {"hotkey.main/capture_hotkey", "capture"},
    {"hotkey.main/region_hotkey", "region"},
    {"hotkey.main/settings_hotkey", "settings"},
};

/**
 * @brief 从 JSON 条目里取字符串字段；类型不对时按缺失处理。
 * @param[in] item 条目对象。
 * @param[in] key  字段名。
 * @return 字段文本；不存在/不是字符串时返回空串。
 */
std::string jsonString(const nlohmann::json& item, const char* key) {
    const auto it = item.find(key);
    if (it == item.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

/**
 * @brief 解析 hotkey_list 快照；坏条目只跳过并 WARN。
 * @param[in]  host 宿主服务表（只用于日志，可为 nullptr）。
 * @param[in]  text JSON 数组文本。
 * @param[out] out  解析出的条目（已按 id 去重，保持列表顺序）。
 * @return true = JSON 合法且是数组；false = 整体不可用（调用方按"没有贡献"降级）。
 */
bool parseList(const ShotHostServices* host, const std::string& text,
               std::vector<HotkeyEntry>& out) {
    const nlohmann::json data = nlohmann::json::parse(text, nullptr, false);
    if (!data.is_array()) {
        shot::Logger(host).warn("[hotkey] hotkey_list 快照不是 JSON 数组，按没有贡献条目处理");
        return false;
    }
    std::vector<std::string> ids;
    for (const nlohmann::json& item : data) {
        if (!item.is_object()) {
            shot::Logger(host).warn("[hotkey] hotkey_list 里有非对象条目，已跳过");
            continue;
        }
        HotkeyEntry entry;
        entry.id = jsonString(item, "id");
        if (entry.id.empty()) {
            shot::Logger(host).warn("[hotkey] hotkey_list 条目缺少 id，已跳过");
            continue;
        }
        if (std::find(ids.begin(), ids.end(), entry.id) != ids.end()) {
            shot::Logger(host).warn("[hotkey] hotkey_list 条目 id 重复：" + entry.id +
                                    "（配置键会冲突），只保留先到的一条");
            continue;
        }
        if (entry.id == "enabled") {
            // enabled 是 provider 的固定配置键（BOOL 总开关），不能同时当动态热键 id。
            shot::Logger(host).warn(
                "[hotkey] hotkey_list 条目 id=enabled 与总开关配置键冲突，已跳过");
            continue;
        }
        entry.owner = jsonString(item, "owner");
        entry.label = jsonString(item, "label");
        if (entry.label.empty()) {
            entry.label = entry.id;  // schema：label 可省略，缺省用 id
        }
        entry.defaultCombo = jsonString(item, "combo");
        entry.event = jsonString(item, "event");
        if (entry.event.empty()) {
            shot::Logger(host).warn("[hotkey] hotkey_list 条目 " + entry.id +
                                    " 缺少 event，已跳过（按下后没有事件可发）");
            continue;
        }
        // data 约定是字符串；宽容接受对象/数组并序列化成 JSON 文本，空/null = 空负载。
        const auto dataIt = item.find("data");
        if (dataIt != item.end() && !dataIt->is_null()) {
            if (dataIt->is_string()) {
                entry.data = dataIt->get<std::string>();
            } else {
                entry.data = dataIt->dump();
            }
        }
        ids.push_back(entry.id);
        out.push_back(std::move(entry));
    }
    return true;
}

}  // namespace

HotkeyCatalog::~HotkeyCatalog() { stop(); }

bool HotkeyCatalog::start(const ShotHostServices* host) {
    m_host = host;
    // 迁移必须在第一次 reload 之前：设置页第一次读动态配置项时就要看到旧值。
    try {
        migrateLegacyConfig();
        publishContribution();
    } catch (...) {
        // 迁移/发布失败不阻止插件：内置三个条目仍然在，热键按默认值注册。
        shot::Logger(host).warn("[hotkey] 初始化 hotkey_list 贡献时发生异常，已按内置条目继续");
    }

    bool subscribed = false;
    shot::EventBus bus(host);
    if (bus.available()) {
        subscribed = bus.subscribe(kListsChangedTopic, &HotkeyCatalog::onBusEvent, this);
        if (!subscribed) {
            shot::Logger(host).warn(
                "[hotkey] 订阅 lists.changed 失败，hotkey_list 变化不会自动重新注册");
        }
    } else {
        shot::Logger(host).warn(
            "[hotkey] 宿主未提供事件总线（旧版宿主？），hotkey_list 变化只在重新加载时生效");
    }
    m_subscribed = subscribed;
    reload();
    shot::Logger(m_host).info("[hotkey] hotkey_list 已刷新：共 " +
                              std::to_string(entryCount()) + " 条热键");
    return subscribed;
}

void HotkeyCatalog::stop() {
    if (m_host != nullptr && m_subscribed) {
        shot::EventBus(m_host).unsubscribe(this);
        m_subscribed = false;
    }
    setChangeHandler({});
}

void HotkeyCatalog::setChangeHandler(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_changeHandler = std::move(handler);
}

std::uint32_t HotkeyCatalog::entryCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<std::uint32_t>(m_entries.size());
}

std::vector<HotkeyEntry> HotkeyCatalog::entries() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries;
}

bool HotkeyCatalog::readConfigValue(const std::string& id, HotkeyEntry& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [&id](const HotkeyEntry& entry) { return entry.id == id; });
    if (it == m_entries.end()) {
        return false;
    }
    out = *it;
    return true;
}

bool HotkeyCatalog::writeConfigValue(const std::string& id, const std::string& value) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                     [&id](const HotkeyEntry& entry) { return entry.id == id; });
        if (it != m_entries.end()) {
            it->value = value;
            it->hasValue = true;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    // 先写宿主配置：set_config 返回后宿主还会写一次同一个键（幂等），但注册路径
    // 在本函数返回后立刻重注册，必须已经能读到新值。
    writeHostValue(id, value);
    return true;
}

void HotkeyCatalog::onBusEvent(void* user, const char* topic, const void* data,
                               std::uint32_t len) {
    auto* self = static_cast<HotkeyCatalog*>(user);
    if (self == nullptr || topic == nullptr) {
        return;
    }
    if (std::strcmp(topic, kListsChangedTopic) != 0) {
        return;  // 只订阅了 lists.changed；其它主题安静忽略
    }
    // 富例外边界：回调由宿主分发线程直接调用，任何分配/解析异常都不能穿出去。
    try {
        // 负载只有 {"list":"..."}：不是 hotkey_list 就与我们无关。空负载按"保守刷新"
        // 处理（老宿主/损坏负载时宁可多读一次快照）。
        if (data != nullptr && len != 0) {
            const std::string text(static_cast<const char*>(data), len);
            const nlohmann::json payload = nlohmann::json::parse(text, nullptr, false);
            if (payload.is_object() && jsonString(payload, "list") != kHotkeyListId) {
                return;
            }
        }
        self->reload();
        shot::Logger(self->m_host).info("[hotkey] hotkey_list 已刷新：共 " +
                                        std::to_string(self->entryCount()) + " 条热键");
        std::function<void()> handler;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            handler = self->m_changeHandler;
        }
        if (handler) {
            handler();
        }
    } catch (...) {
        shot::Logger(self->m_host).warn("[hotkey] 处理 lists.changed 失败，已忽略这一条");
    }
}

void HotkeyCatalog::reload() {
    std::vector<HotkeyEntry> parsed;
    if (m_host != nullptr) {
        const std::string text = shot::PluginLists(m_host).json(kHotkeyListId);
        if (!text.empty()) {
            parseList(m_host, text, parsed);
        }
    }
    // 内置三个 id 只允许 hotkey.main 自己贡献：别的 owner 用同名 id 会让配置键
    // hotkey.main/<id> 撞车，这里先到者（内置）胜，坏条目只 WARN 丢弃。
    const std::vector<HotkeyEntry> builtins = builtinEntries();
    std::vector<HotkeyEntry> merged;
    std::vector<std::string> ids;
    const auto alreadyPresent = [&ids](const std::string& id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    for (const HotkeyEntry& seed : builtins) {
        const auto it = std::find_if(parsed.begin(), parsed.end(),
                                     [&seed](const HotkeyEntry& entry) {
                                         return entry.id == seed.id && entry.owner != kHotkeyMainId;
                                     });
        if (it != parsed.end()) {
            shot::Logger(m_host).warn("[hotkey] hotkey_list 条目 " + seed.id +
                                      " 由 " + it->owner +
                                      " 贡献，但该 id 是 hotkey.main 的内置配置键，已忽略");
            parsed.erase(it);
        }
    }
    // 快照没给（老宿主 / 自己贡献的事件还没被处理）时用内置条目兜底；正常情况
    // 下列表里就有这三条，下面会按列表顺序原样使用。
    for (const HotkeyEntry& seed : builtins) {
        if (std::none_of(parsed.begin(), parsed.end(),
                         [&seed](const HotkeyEntry& entry) { return entry.id == seed.id; })) {
            HotkeyEntry entry = seed;
            entry.owner = kHotkeyMainId;
            ids.push_back(entry.id);
            merged.push_back(std::move(entry));
        }
    }
    for (HotkeyEntry& entry : parsed) {
        if (alreadyPresent(entry.id)) {
            shot::Logger(m_host).warn("[hotkey] hotkey_list 条目 id 重复：" + entry.id +
                                      "，只保留先到的一条");
            continue;
        }
        ids.push_back(entry.id);
        merged.push_back(std::move(entry));
    }
    // 解析实际值：配置里有就用原值（空串也算"显式清空"），没有才用列表默认组合键。
    for (HotkeyEntry& entry : merged) {
        std::string raw;
        entry.hasValue = readHostValue(entry.id, raw);
        entry.value = entry.hasValue ? raw : entry.defaultCombo;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries = std::move(merged);
    }
}

void HotkeyCatalog::migrateLegacyConfig() {
    if (m_host == nullptr) {
        return;
    }
    shot::Config config(m_host);
    if (!config.available()) {
        shot::Logger(m_host).warn("[hotkey] 宿主未提供配置服务，跳过旧热键配置迁移");
        return;
    }
    for (const LegacyMapping& mapping : kLegacyMappings) {
        const std::string oldValue = config.getString(mapping.oldKey, kMissingConfigValue);
        if (oldValue == kMissingConfigValue) {
            continue;  // 旧键不存在：没什么可迁移的
        }
        // 新键不存在才迁移；新键存在（哪怕是用户清空后的空串）绝不覆盖。
        // 禁用→启用会重复 create，但第二次新键已经存在，所以这段天然幂等。
        const std::string newKey = std::string(kHotkeyMainId) + "/" + mapping.newId;
        if (config.getString(newKey.c_str(), kMissingConfigValue) != kMissingConfigValue) {
            continue;
        }
        if (config.setString(newKey.c_str(), oldValue)) {
            shot::Logger(m_host).info("[hotkey] 旧配置迁移：" + std::string(mapping.oldKey) +
                                      " → " + newKey);
        } else {
            shot::Logger(m_host).warn("[hotkey] 旧配置迁移失败：" + newKey +
                                      "（宿主拒绝写入），继续使用默认组合键");
        }
    }
}

void HotkeyCatalog::publishContribution() {
    if (m_host == nullptr) {
        return;
    }
    nlohmann::json items = nlohmann::json::array();
    for (const HotkeyEntry& entry : builtinEntries()) {
        items.push_back({
            {"id", entry.id},
            {"combo", entry.defaultCombo},
            {"event", entry.event},
            {"data", entry.data},
            {"label", entry.label},
        });
    }
    nlohmann::json payload = {
        {"list", kHotkeyListId},
        {"owner", kHotkeyMainId},
        {"items", std::move(items)},
    };
    const std::string text = payload.dump();
    if (!shot::EventBus(m_host).publish("lists.add", text)) {
        shot::Logger(m_host).warn(
            "[hotkey] 发布 hotkey_list 内置条目失败（旧版宿主或队列满），只使用本地内置条目");
        return;
    }
    shot::Logger(m_host).info("[hotkey] 已向 hotkey_list 贡献 3 个内置热键（capture / region / settings）");
}

bool HotkeyCatalog::readHostValue(const std::string& id, std::string& value) const {
    if (m_host == nullptr) {
        return false;
    }
    shot::Config config(m_host);
    if (!config.available()) {
        return false;
    }
    const std::string key = std::string(kHotkeyMainId) + "/" + id;
    const std::string text = config.getString(key.c_str(), kMissingConfigValue);
    if (text == kMissingConfigValue) {
        return false;
    }
    value = text;
    return true;
}

bool HotkeyCatalog::writeHostValue(const std::string& id, const std::string& value) const {
    if (m_host == nullptr) {
        return false;
    }
    shot::Config config(m_host);
    if (!config.available()) {
        return false;
    }
    const std::string key = std::string(kHotkeyMainId) + "/" + id;
    const bool ok = config.setString(key.c_str(), value);
    if (!ok) {
        shot::Logger(m_host).warn("[hotkey] 写入宿主配置 " + key + " 失败，注册将使用默认组合键");
    }
    return ok;
}

std::vector<HotkeyEntry> HotkeyCatalog::builtinEntries() {
    // label 是 i18n 文本 key，设置页按 display("hotkey.main", label) 解析；语言表在
    // plugin_entry.cpp 的 kLangTables。
    return {
        HotkeyEntry{"capture", kHotkeyMainId, "config.capture.label", "Ctrl+Alt+S",
                    "capture.request", "", "", false},
        HotkeyEntry{"region", kHotkeyMainId, "config.region.label", "Ctrl+Alt+A",
                    "capture.request", R"json({"mode":"region"})json", "", false},
        HotkeyEntry{"settings", kHotkeyMainId, "config.settings.label", "Ctrl+Alt+O",
                    "settings.open", "", "", false},
    };
}

}  // namespace hotkey
