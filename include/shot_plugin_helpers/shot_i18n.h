#pragma once

// ============================================================================
//  include/shot_plugin_helpers/shot_i18n.h —— 插件侧多语言封装 shot::I18n
//
//  解决两件事：
//    1. 插件自己的界面文案：作者用 R"json(...)json" 写每语言一张表，
//       tr(key, fallback) 按当前语言查本地表（O(1)，不碰宿主）；
//    2. 其它插件/宿主贡献的文案：数据列表里的 label/tooltip/ENUM 选项是"文本
//       key 或字面量"，resolve(owner, value) 查宿主生成的 i18n_list 快照缓存。
//
//  协议（零 ABI 改动，详见 document/I18N.md）：
//    · loadTables() 把本地表归一成 {id, texts:{tag:text}} 后 lists.add 到
//      i18n_source_list（按 64 KiB 上限分块）；
//    · 当前语言、跨插件文本来自宿主生成的只读列表 i18n_list；首条 @meta 带
//      language/fallback，其余条目是 {owner,id,text}；
//    · 宿主发布 lists.changed({list:"i18n_list"}) 时清缓存，下一次查询重读；
//    · 老宿主没有列表服务/没有 i18n_list：available() 为 false，tr() 用第一张表，
//      resolve() 原样返回 key —— 安静降级，不影响其它功能。
//
//  这层不跨 DLL 边界（只被插件源码包含），所以直接用 nlohmann-json 做解析：
//  header-only、不引入运行库；插件工程需要 find_package(nlohmann_json)。
//  本头文件只声明一个类；插件通常只包含汇总头 shot_plugin_helpers.h。
// ============================================================================

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "plugin_api.h"
#include "shot_plugin_helpers/shot_event_bus.h"
#include "shot_plugin_helpers/shot_logger.h"
#include "shot_plugin_helpers/shot_plugin_lists.h"

namespace shot {

/**
 * @brief 插件侧多语言：本地语言表 + 跨插件文本解析。
 *
 * 生命周期：在 create()/init() 里构造（宿主此时就能给它服务表），在
 * destroy()/deinit() 里销毁；析构会退订 lists.changed。对象不可拷贝/移动
 * （内部有互斥量），插件状态里用 std::unique_ptr<shot::I18n> 或
 * std::optional<shot::I18n> 持有。
 *
 * 线程模型：
 *  - 事件回调跑在宿主分发线程，只置脏标记；真正的快照重读发生在下一次
 *    tr()/resolve()/language()（插件自己的渲染线程），重读的列表服务任意线程可调；
 *  - 本地表（loadTables）与语言切换最好都在插件自己的线程/生命周期回调里串行做。
 *
 * 用法：
 * ```cpp
 * const char* const kLangTables[] = {
 *     R"json({"@meta":"zh-CN","config.strength.label":"强度"})json",
 *     R"json({"@meta":"en-US","config.strength.label":"Strength"})json",
 * };
 * // create() 里：
 * m_i18n = std::make_unique<shot::I18n>(host, "hotkey.main");
 * m_i18n->loadTables(kLangTables, std::size(kLangTables));
 * // 渲染时：
 * label = m_i18n->tr("config.strength.label", "强度");
 * name  = m_i18n->resolve(owner, item.label);
 * ```
 */
class I18n {
public:
    /**
     * @brief 构造并订阅 lists.changed（只关心 i18n_list）。
     * @param[in] host    宿主传给 create()/init() 的服务表指针；nullptr 时全功能降级。
     * @param[in] ownerId 本插件 id（lists.add 的 owner，必须等于宿主已加载的插件 id）；
     *                    可以为 nullptr，此时不发布源列表、只能用 tr()。
     */
    explicit I18n(const ShotHostServices* host, const char* ownerId);

    /** @brief 退订 lists.changed；返回后不会再收到宿主回调。 */
    ~I18n();

    /** @brief 禁止拷贝：内部持有订阅句柄（this）与互斥量。 */
    I18n(const I18n&) = delete;
    /** @brief 禁止赋值：内部持有订阅句柄（this）与互斥量。 */
    I18n& operator=(const I18n&) = delete;

    /** @brief 源列表 id（插件贡献语言表，宿主只读）。 */
    static const char* sourceListId();
    /** @brief 宿主生成的语言快照列表 id（只读）。 */
    static const char* resolvedListId();

    /**
     * @brief 宿主是否提供了列表服务与事件总线（老宿主可能没有）。
     * @return true = 能读 i18n_list、能发布 i18n_source_list。
     */
    bool available() const noexcept;

    /**
     * @brief 解析作者写的语言表并发布到 i18n_source_list。
     *
     * 每张表是一个 JSON 对象，必须带字符串 "@meta"（语言标签）；其余键值都是
     * 文案。非法 JSON / 缺 @meta / 各语言 key 集不一致只记 WARN，能用的部分照常
     * 加载（插件不因此失败）。分块发布，单条事件负载不超过 64 KiB。
     *
     * @param[in] tables 语言表数组（每语言一个 JSON 文本）。
     * @param[in] count  表个数；0 表示没有文案。
     * @return true = 至少解析成功一张表（即使没有发布成功）；false = 一张都没解析出来。
     * @note 只在 create()/init() 里调用一次；重复调用会先撤回旧贡献再重新发布。
     */
    bool loadTables(const char* const* tables, std::size_t count);

    /**
     * @brief 当前界面语言（BCP-47 规范标签）。
     * @return 语言标签，如 "en-US"；未加载 i18n_list 时用第一张表的 @meta。
     * @note 指针在下一次语言切换前有效；要长期持有请拷成 std::string。
     */
    const char* language() const;

    /**
     * @brief 查插件自己的文案：本地表 O(1)，按当前语言与回退链取。
     * @param[in] key      文案 key（与语言表里的键一致）。
     * @param[in] fallback 查不到时的兜底文本（通常是中文原文）；不会为 nullptr 时的返回值。
     * @return 文案指针，生命周期与 map 节点相同（对象存活期间有效）；查不到返回 fallback。
     */
    const char* tr(const char* key, const char* fallback) const;

    /**
     * @brief 把数据列表里的"文本 key 或字面量"解析成当前语言文本。
     *
     * 规则与所有消费方一致：display(owner, value) = i18n_list[(owner,value)] ?? value。
     * 查询走本地缓存，宿主列表变化后自动重读一次，渲染帧里只有 map 查找。
     *
     * @param[in] owner     贡献方 id（config_list / tray_command_list 条目的 owner）。
     * @param[in] textOrKey key 或字面量；nullptr 时原样返回 nullptr。
     * @return 当前语言文本；查不到时原样返回 textOrKey。
     * @note 返回指针在下一次 i18n_list 变化时失效；要长期持有请拷成 std::string。
     */
    const char* resolve(const char* owner, const char* textOrKey) const;

    /**
     * @brief 强制重读 i18n_list 快照（一般不需要：列表变化时自动重读）。
     *
     * 用于"插件自己刚发布完语言表、又想在宿主处理前给个初值"的场景；宿主还没
     * 生成 i18n_list 时是空操作。
     */
    void refresh() const;

private:
    /**
     * @brief lists.changed 的宿主回调：只置脏标记，不做任何重活。
     * @param[in] user  this。
     * @param[in] topic 主题（固定 "lists.changed"）。
     * @param[in] data  负载字节；可为 nullptr。
     * @param[in] len   负载字节数。
     */
    static void onListsChanged(void* user, const char* topic, const void* data, uint32_t len);

    /**
     * @brief 把标签规范成 BCP-47 写法（语言小写、地区大写、`_` 换成 `-`）。
     * @param[in] tag 任意写法。
     * @return 规范标签；空输入返回空串。
     */
    static std::string normalizeTag(std::string_view tag);

    /**
     * @brief 取标签的主语言（第一个 `-` 之前的部分）。
     * @param[in] tag 任意写法标签。
     * @return 主语言（小写）。
     */
    static std::string primaryTag(std::string_view tag);

    /**
     * @brief 在"标签 -> 文本"里按当前语言 + 回退链找一条。
     * @param[in] texts 已规范化的 标签 -> 文本；调用方必须持 m_mutex。
     * @return 命中文本；没有时 nullptr。
     */
    const std::string* pickTagged(
        const std::unordered_map<std::string, std::string>& texts) const;

    /**
     * @brief 脏标记为真时重读 i18n_list（内部用 m_mutex，任意线程可调）。
     */
    void refreshIfDirty() const;

    /**
     * @brief 真正重读快照并重建解析缓存；调用方必须持 m_mutex。
     */
    void refreshLocked() const;

    /**
     * @brief 解析一张语言表，把文本并进 local / keys；非法表只 WARN。
     * @param[in] table     一张 R"json(...)json" 文本。
     * @param[in] language  该表的 @meta 语言标签（已规范化）；非法时返回 false。
     * @param[in,out] local key -> (tag -> text)。
     * @param[in,out] keys  首次出现的 key 顺序（各语言合并后的并集）。
     * @param[in,out] keySets 每个语言的"tag\t排序后的 key 列表"，用来逐 key 检查
     *                        各语言 key 集是否一致（只比个数会漏掉"数量相同但 key
     *                        不同"的漏翻）。
     * @return true = 这张表可用。
     */
    bool parseTable(const char* table, std::string& language,
                    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& local,
                    std::vector<std::string>& keys, std::vector<std::string>& keySets) const;

    /** @brief 发布一块 i18n_source_list 的 lists.add；超过 64 KiB 之前分批。 */
    void publishSourceEntry(
        const std::vector<std::string>& keys,
        const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& local)
        const;

    std::string m_owner;                       /**< 本插件 id（lists.add 的 owner） */
    EventBus m_bus;                            /**< lists.add / lists.changed */
    PluginLists m_lists;                       /**< 读 i18n_list 快照 */
    Logger m_log;                              /**< 解析告警（走宿主 plugin logger） */
    mutable std::mutex m_mutex;                /**< 保护下面全部可变状态 */
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_local;
    std::vector<std::string> m_keys;           /**< 已发布的 key（重复 loadTables 时先撤回） */
    mutable std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
        m_resolved;                            /**< owner -> (id -> 当前语言文本) */
    mutable std::string m_language;            /**< 当前界面语言（BCP-47） */
    mutable std::atomic<bool> m_dirty{true};   /**< i18n_list 快照需要重读 */
    bool m_subscribed = false;                 /**< 是否订阅了 lists.changed */
    bool m_loaded = false;                     /**< 是否已成功加载过语言表 */
};

inline const char* I18n::sourceListId() { return "i18n_source_list"; }

inline const char* I18n::resolvedListId() { return "i18n_list"; }

inline I18n::I18n(const ShotHostServices* host, const char* ownerId)
    : m_owner(ownerId != nullptr ? ownerId : ""),
      m_bus(host),
      m_lists(host),
      m_log(host) {
    // 当前语言先按默认值起步：loadTables 会用第一张表的 @meta 覆盖，
    // i18n_list 可读时再以 @meta.language 为准（宿主是唯一事实）。
    m_language = "en-US";
    if (m_bus.available()) {
        m_subscribed = m_bus.subscribe("lists.changed", &I18n::onListsChanged, this);
    }
}

inline I18n::~I18n() {
    if (m_subscribed) {
        m_bus.unsubscribe(this);
    }
}

inline bool I18n::available() const noexcept {
    return m_lists.available() && m_bus.available();
}

inline std::string I18n::normalizeTag(std::string_view tag) {
    std::string out;
    std::size_t start = 0;
    int part = 0;
    while (start < tag.size()) {
        const std::size_t separator = tag.find_first_of("-_", start);
        const std::size_t end = (separator == std::string_view::npos) ? tag.size() : separator;
        const std::string_view piece = tag.substr(start, end - start);
        start = (separator == std::string_view::npos) ? tag.size() : separator + 1;
        if (piece.empty()) {
            continue;
        }
        if (part > 0) {
            out.push_back('-');
        }
        // 与宿主 Host::I18n::normalizeTag 同一套规则：语言小写、脚本首字母大写、
        // 地区/数字大写；这样 "zh_cn" 与 "zh-CN" 能对上同一条文案。
        bool allAlpha = true;
        bool allDigit = true;
        for (const char c : piece) {
            const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            const bool digit = (c >= '0' && c <= '9');
            allAlpha = allAlpha && alpha;
            allDigit = allDigit && digit;
        }
        if (part == 0 || !(allAlpha || allDigit)) {
            for (const char c : piece) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        } else if (allAlpha && piece.size() == 4) {
            for (std::size_t index = 0; index < piece.size(); ++index) {
                const char c = piece[index];
                out.push_back(static_cast<char>(index == 0 ? std::toupper(static_cast<unsigned char>(c))
                                                           : std::tolower(static_cast<unsigned char>(c))));
            }
        } else if ((allAlpha && piece.size() == 2) || (allDigit && piece.size() == 3)) {
            for (const char c : piece) {
                out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
        } else {
            for (const char c : piece) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        ++part;
    }
    return out;
}

inline std::string I18n::primaryTag(std::string_view tag) {
    const std::string normalized = normalizeTag(tag);
    const std::size_t separator = normalized.find('-');
    return (separator == std::string::npos) ? normalized : normalized.substr(0, separator);
}

inline bool I18n::parseTable(
    const char* table, std::string& language,
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& local,
    std::vector<std::string>& keys, std::vector<std::string>& keySets) const {
    if (table == nullptr) {
        m_log.warn("[i18n] 语言表是空指针，已跳过");
        return false;
    }
    const nlohmann::json doc = nlohmann::json::parse(table, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        m_log.warn("[i18n] 语言表不是合法 JSON 对象，已跳过");
        return false;
    }
    const auto metaIt = doc.find("@meta");
    if (metaIt == doc.end() || !metaIt->is_string()) {
        m_log.warn("[i18n] 语言表缺少字符串 @meta，已跳过");
        return false;
    }
    language = normalizeTag(metaIt->get<std::string>());
    if (language.empty()) {
        m_log.warn("[i18n] 语言表的 @meta 为空，已跳过");
        return false;
    }

    std::vector<std::string> tableKeys;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (it.key() == "@meta") {
            continue;
        }
        if (!it->is_string()) {
            m_log.warn("[i18n] 语言表 " + language + " 的键 " + it.key() + " 不是字符串，已跳过");
            continue;
        }
        local[it.key()][language] = it->get<std::string>();
        tableKeys.push_back(it.key());
    }
    // 各语言的 key 集必须一致：缺了某语言的 key 时该语言会落到回退链（可能显示
    // 英文或中文），能运行但通常是作者漏写，记 WARN 便于发现。这里存"排序后的
    // 完整 key 列表"而不是个数——数量相同但 key 不同时同样要报出来。
    std::vector<std::string> sortedKeys = tableKeys;
    std::sort(sortedKeys.begin(), sortedKeys.end());
    std::string canonical = language;
    canonical.push_back('\t');
    for (const std::string& key : sortedKeys) {
        canonical += key;
        canonical.push_back(',');
    }
    keySets.push_back(std::move(canonical));
    if (keys.empty()) {
        keys = tableKeys;
    }
    return true;
}

inline void I18n::publishSourceEntry(
    const std::vector<std::string>& keys,
    const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& local)
    const {
    if (!m_bus.available() || m_owner.empty()) {
        return;
    }
    // 重复 loadTables：先撤回旧 key，避免删掉的文案残留在源列表里。
    if (!m_keys.empty()) {
        nlohmann::json removePayload = {
            {"list", sourceListId()},
            {"owner", m_owner},
            {"ids", m_keys},
        };
        m_bus.publish("lists.remove", removePayload.dump());
    }
    // 单条事件负载上限 64 KiB（document/EVENT_BUS.md §5）：按估算大小分块，
    // 留 8 KiB 给 list/owner/items 外壳与 JSON 转义。
    constexpr std::size_t kChunkLimit = 56 * 1024;
    nlohmann::json chunk = nlohmann::json::array();
    std::size_t chunkBytes = 0;
    for (const std::string& key : keys) {
        const auto textsIt = local.find(key);
        if (textsIt == local.end() || textsIt->second.empty()) {
            continue;
        }
        nlohmann::json item = {
            {"id", key},
            {"texts", nlohmann::json::object()},
        };
        for (const auto& [tag, text] : textsIt->second) {
            item["texts"][tag] = text;
        }
        const std::size_t itemBytes = item.dump().size() + 1;
        if (!chunk.empty() && chunkBytes + itemBytes > kChunkLimit) {
            const nlohmann::json payload = {
                {"list", sourceListId()},
                {"owner", m_owner},
                {"items", chunk},
            };
            m_bus.publish("lists.add", payload.dump());
            chunk = nlohmann::json::array();
            chunkBytes = 0;
        }
        chunk.push_back(std::move(item));
        chunkBytes += itemBytes;
    }
    if (!chunk.empty()) {
        const nlohmann::json payload = {
            {"list", sourceListId()},
            {"owner", m_owner},
            {"items", chunk},
        };
        m_bus.publish("lists.add", payload.dump());
    }
}

inline bool I18n::loadTables(const char* const* tables, std::size_t count) {
    if (tables == nullptr || count == 0) {
        return false;
    }
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> local;
    std::vector<std::string> keys;
    std::vector<std::string> keySets;
    std::string firstLanguage;
    bool loadedAny = false;
    for (std::size_t index = 0; index < count; ++index) {
        std::string language;
        if (!parseTable(tables[index], language, local, keys, keySets)) {
            continue;
        }
        if (firstLanguage.empty()) {
            firstLanguage = language;
        }
        loadedAny = true;
    }
    if (!loadedAny) {
        return false;
    }
    // 各语言 key 集不一致只记 WARN（缺的 key 会走回退链），不让插件失败。
    for (std::size_t index = 1; index < keySets.size(); ++index) {
        const std::size_t firstTab = keySets.front().find('\t');
        const std::size_t tab = keySets[index].find('\t');
        const std::string firstKeys =
            (firstTab == std::string::npos) ? std::string() : keySets.front().substr(firstTab + 1);
        const std::string keys =
            (tab == std::string::npos) ? std::string() : keySets[index].substr(tab + 1);
        if (keys != firstKeys) {
            m_log.warn("[i18n] 语言表 key 集不一致：" + keySets.front() + " vs " + keySets[index]);
        }
    }

    publishSourceEntry(keys, local);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_local = std::move(local);
        m_keys = std::move(keys);
        m_loaded = true;
        // 首次 load 前没有 i18n_list 可读：用第一张表的语言作为当前语言。
        if (m_language.empty() || m_dirty.load()) {
            m_language = firstLanguage.empty() ? "en-US" : firstLanguage;
        }
    }
    // 宿主可能已经把 i18n_list 生成好了（热卸载后重新 create）：立刻对齐一次。
    refresh();
    return true;
}

inline void I18n::onListsChanged(void* user, const char* /*topic*/, const void* data,
                                 uint32_t len) {
    auto* self = static_cast<I18n*>(user);
    if (self == nullptr) {
        return;
    }
    // 宿主同一轮对每个 list 只发一条 lists.changed；只关心 i18n_list。
    // 负载为空按"可能是任意列表"保守处理。回调里只置脏标记：重读与解析放到
    // 插件自己的线程（tr/resolve 里），不占宿主分发线程。
    if (data == nullptr || len == 0) {
        self->m_dirty.store(true);
        return;
    }
    const std::string payload(static_cast<const char*>(data), len);
    const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        self->m_dirty.store(true);
        return;
    }
    const auto listIt = doc.find("list");
    if (listIt != doc.end() && listIt->is_string() &&
        listIt->get<std::string>() == resolvedListId()) {
        self->m_dirty.store(true);
    }
}

inline void I18n::refreshLocked() const {
    m_dirty.store(false);
    m_resolved.clear();
    if (!m_lists.available()) {
        return;
    }
    const std::string text = m_lists.json(resolvedListId());
    if (text.empty()) {
        return;  // 宿主要么还没生成 i18n_list，要么是没有这个协议的老宿主
    }
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_array()) {
        return;
    }
    for (const nlohmann::json& entry : doc) {
        if (!entry.is_object()) {
            continue;
        }
        const auto idIt = entry.find("id");
        if (idIt == entry.end() || !idIt->is_string()) {
            continue;
        }
        const std::string id = idIt->get<std::string>();
        if (id == "@meta") {
            const auto languageIt = entry.find("language");
            if (languageIt != entry.end() && languageIt->is_string()) {
                const std::string language = normalizeTag(languageIt->get<std::string>());
                if (!language.empty()) {
                    m_language = language;
                }
            }
            continue;
        }
        const auto ownerIt = entry.find("owner");
        const auto textIt = entry.find("text");
        if (ownerIt == entry.end() || !ownerIt->is_string() || textIt == entry.end() ||
            !textIt->is_string()) {
            continue;
        }
        m_resolved[ownerIt->get<std::string>()][id] = textIt->get<std::string>();
    }
}

inline void I18n::refreshIfDirty() const {
    if (!m_dirty.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_dirty.load()) {
        refreshLocked();
    }
}

inline void I18n::refresh() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    refreshLocked();
}

inline const char* I18n::language() const {
    refreshIfDirty();
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_language.c_str();
}

inline const std::string* I18n::pickTagged(
    const std::unordered_map<std::string, std::string>& texts) const {
    if (texts.empty()) {
        return nullptr;
    }
    // 回退链：完全匹配 -> 主语言族 -> 默认 en-US -> 默认语言的主语言 -> 第一条。
    // texts 的 key 在解析时已经规范过，所以这里直接按规范标签比较。
    const std::string primary = primaryTag(m_language);
    const std::vector<std::string> candidates = {m_language, primary, "en-US", "en"};
    for (const std::string& candidate : candidates) {
        const auto exact = texts.find(candidate);
        if (exact != texts.end()) {
            return &exact->second;
        }
        const std::string candidatePrimary = primaryTag(candidate);
        for (const auto& [tag, text] : texts) {
            if (primaryTag(tag) == candidatePrimary) {
                return &text;
            }
        }
    }
    return &texts.begin()->second;
}

inline const char* I18n::tr(const char* key, const char* fallback) const {
    if (key == nullptr) {
        return fallback;
    }
    refreshIfDirty();
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_local.find(key);
    if (it == m_local.end()) {
        return fallback;
    }
    const std::string* text = pickTagged(it->second);
    return (text != nullptr) ? text->c_str() : fallback;
}

inline const char* I18n::resolve(const char* owner, const char* textOrKey) const {
    if (owner == nullptr || textOrKey == nullptr) {
        return textOrKey;
    }
    refreshIfDirty();
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto ownerIt = m_resolved.find(owner);
    if (ownerIt != m_resolved.end()) {
        const auto keyIt = ownerIt->second.find(textOrKey);
        if (keyIt != ownerIt->second.end()) {
            return keyIt->second.c_str();
        }
    }
    return textOrKey;
}

}  // namespace shot
