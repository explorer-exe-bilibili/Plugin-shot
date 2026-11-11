// 语言包 → i18n_source_list 条目的合并与分块发布。
//
// 合并规则与宿主 mergeText 完全一致（src/hostListCatalog.cpp）：
//   · 同一 (target, key, tag)：priority 大者胜；同 priority 时扫描顺序靠后者胜；
//   · 语言声明（@language/<tag>）：同一 tag 取 priority/顺序更靠后的显示名与 fallback。
// 必须在插件内先合并，原因见头文件：宿主按 (list, owner, id) 存条目，本插件只有
// 一个 owner，同一 key 发两次只会互相覆盖。
//
// 发布形状（document/I18N.md §2.1）：
//   {"list":"i18n_source_list","owner":"translator.main","items":[
//     {"id":"@language/ja-JP","texts":{"ja-JP":"日本語"},"fallback":"en-US"},
//     {"id":"settings.open","target":"core","priority":300,"texts":{"ja-JP":"設定"}}]}
// 单条事件负载上限 64 KiB，按 56 KiB 预算分块（留 8 KiB 给外壳与转义）。

#include "source_publisher.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "shot_plugin_helpers.h"

namespace translator {
namespace {

const char kSourceListId[] = "i18n_source_list";
const char kCoreOwner[] = "core";
const char kLanguagePrefix[] = "@language/";

// 单条 lists.add 负载的硬上限是 64 KiB；56 KiB 给外壳（list/owner/items 与 JSON
// 转义）留足余量，与 helper 的分块预算一致。
constexpr std::size_t kChunkBudget = 56 * 1024;

/** @brief 一个 (target,key,tag) 的胜出候选。 */
struct TextCandidate {
    std::string text;         /**< 译文 */
    std::int32_t priority = 0; /**< 来源优先级：官方 200 / 用户 300 */
    std::size_t order = 0;    /**< 扫描顺序：同 priority 时大者胜 */
};

/** @brief 一个语言的显示名/回退声明（多包同 tag 时合并成一条）。 */
struct Declaration {
    std::string tag;       /**< 规范语言标签 */
    std::string name;      /**< 语言显示名（@meta.name，空则用 tag） */
    std::string fallback;  /**< @meta.fallback（可空 = 宿主默认） */
    std::int32_t priority = 0; /**< 来源优先级 */
    std::size_t order = 0;     /**< 扫描顺序 */
    bool valid = false;        /**< 是否已有候选（首次出现的 tag 总是胜出） */
};

/**
 * @brief 新候选是否胜过现有候选（宿主规则：priority 大者胜、同 priority 后到者胜）。
 * @param[in] priority 新候选优先级。
 * @param[in] order    新候选扫描顺序。
 * @param[in] current  现有候选。
 * @return true = 应覆盖现有候选。
 */
bool wins(std::int32_t priority, std::size_t order, const TextCandidate& current) {
    if (current.priority != priority) {
        return priority > current.priority;
    }
    return order >= current.order;
}

/**
 * @brief 语言包的 target owner 是否值得发布。
 *
 * 宿主只接受 target=core 或"当前已加载插件"，其余整条丢弃并 WARN。发布前先过滤，
 * 是为了让日志把原因归到"包写错了 owner"而不是"宿主丢了条目"；注册表不可用时
 * 不做过滤（宁可多一条宿主 WARN，也不误伤真正可用的文本）。
 *
 * @param[in]     owner  条目声明的 target。
 * @param[in]     loaded 已加载插件 id 集合；空集 = 不做过滤。
 * @param[in,out] warned 已经 WARN 过的 owner（同一个 owner 只报一次）。
 * @param[in]     log    宿主日志。
 * @return true = 可以发布给这个 target。
 */
bool acceptsTarget(const std::string& owner, const std::set<std::string>& loaded,
                   std::set<std::string>& warned, const shot::Logger& log) {
    if (owner.empty()) {
        return false;
    }
    if (owner == kCoreOwner) {
        return true;
    }
    if (loaded.empty()) {
        return true;
    }
    if (loaded.count(owner) != 0) {
        return true;
    }
    if (warned.insert(owner).second) {
        log.warn("[translator] 语言包里的 owner=" + owner +
                 " 既不是 core 也不是已加载插件，跳过它的文本");
    }
    return false;
}

}  // namespace

SourcePublisher::SourcePublisher(const ShotHostServices* host, std::string ownerId)
    : m_host(host), m_owner(std::move(ownerId)) {}

std::set<std::string> SourcePublisher::publish(const std::vector<ParsedPack>& packs) {
    std::set<std::string> publishedIds;
    const shot::Logger log(m_host);
    if (packs.empty()) {
        return publishedIds;
    }
    const std::set<std::string> loaded = loadedOwners();
    std::set<std::string> warnedTargets;

    // 第一步：合并语言声明与文本候选。map 保证输出顺序稳定，日志与快照都可复现。
    std::map<std::string, Declaration> declarations;
    std::map<std::pair<std::string, std::string>, std::map<std::string, TextCandidate>> merged;
    for (const ParsedPack& pack : packs) {
        Declaration& declaration = declarations[pack.tag];
        if (!declaration.valid ||
            pack.priority > declaration.priority ||
            (pack.priority == declaration.priority && pack.order >= declaration.order)) {
            declaration.tag = pack.tag;
            declaration.name = pack.name.empty() ? pack.tag : pack.name;
            declaration.fallback = pack.fallback;
            declaration.priority = pack.priority;
            declaration.order = pack.order;
            declaration.valid = true;
        }
        for (const auto& owner : pack.texts) {
            if (!acceptsTarget(owner.first, loaded, warnedTargets, log)) {
                continue;
            }
            for (const auto& key : owner.second) {
                TextCandidate& current = merged[{owner.first, key.first}][pack.tag];
                if (wins(pack.priority, pack.order, current)) {
                    current.text = key.second;
                    current.priority = pack.priority;
                    current.order = pack.order;
                }
            }
        }
    }

    // 第二步：序列化条目。语言声明是"给语言目录的元数据"，不参与文本合并，
    // 所以和文本条目一起发但形状不同（没有 target/priority，宿主按 id 前缀识别）。
    std::vector<std::string> items;
    items.reserve(declarations.size() + merged.size());
    for (const auto& [tag, declaration] : declarations) {
        nlohmann::json texts = nlohmann::json::object();
        texts[tag] = declaration.name;
        const nlohmann::json item = {
            {"id", std::string(kLanguagePrefix) + tag},
            {"texts", std::move(texts)},
            {"fallback", declaration.fallback},
        };
        publishedIds.insert(std::string(kLanguagePrefix) + tag);
        items.push_back(item.dump());
    }
    for (const auto& [key, candidates] : merged) {
        nlohmann::json texts = nlohmann::json::object();
        std::int32_t priority = 0;
        for (const auto& [tag, candidate] : candidates) {
            texts[tag] = candidate.text;
            priority = std::max(priority, candidate.priority);
        }
        const nlohmann::json item = {
            {"id", key.second},
            {"target", key.first},
            {"priority", priority},
            {"texts", std::move(texts)},
        };
        publishedIds.insert(key.second);
        items.push_back(item.dump());
    }
    if (items.empty()) {
        log.warn("[translator] 合并后没有可发布的条目（语言包里的 target 都不可用），已跳过");
        return publishedIds;
    }
    log.info("[translator] 合并 " + std::to_string(packs.size()) + " 个语言包：语言声明 " +
             std::to_string(declarations.size()) + " 个、文本条目 " +
             std::to_string(merged.size()) + " 条");
    publishItems(items);
    return publishedIds;
}

void SourcePublisher::withdraw() {
    // ids 省略 = 撤回本 owner 在该列表的全部条目：比逐个列 id 更省负载，也不存在
    // id 数组撑爆单条事件的问题。宿主在插件卸载后会自己清一遍，这里只是让它更早生效。
    const shot::EventBus bus(m_host);
    const nlohmann::json payload = {
        {"list", kSourceListId},
        {"owner", m_owner},
    };
    if (!bus.publish("lists.remove", payload.dump())) {
        // 插件已经不在"已加载"列表里时宿主会拒绝并自己清；这里只记一条 DEBUG 级的
        // 事实，不当作错误（卸载路径本来就可能走到这一步）。
        shot::Logger(m_host).debug("[translator] 撤回语言包条目未入队（总线不可用或队列满）");
    }
}

void SourcePublisher::removeIds(const std::set<std::string>& ids) {
    if (ids.empty()) {
        return;
    }
    const shot::EventBus bus(m_host);
    if (!bus.available()) {
        shot::Logger(m_host).warn("[translator] 宿主没有事件总线，无法撤回不再产出的条目");
        return;
    }
    const shot::Logger log(m_host);
    // ids 数组也可能撑爆单条事件，与 lists.add 用同一套 56 KiB 预算分块。
    nlohmann::json chunk = nlohmann::json::array();
    std::size_t chunkBytes = 0;
    std::size_t removed = 0;
    const auto flush = [&]() {
        if (chunk.empty()) {
            return;
        }
        const nlohmann::json payload = {
            {"list", kSourceListId},
            {"owner", m_owner},
            {"ids", chunk},
        };
        if (!bus.publish("lists.remove", payload.dump())) {
            log.warn("[translator] 发布 lists.remove 失败（总线不可用或队列满），旧译文可能残留");
        }
        chunk = nlohmann::json::array();
        chunkBytes = 0;
    };
    for (const std::string& id : ids) {
        const std::size_t idBytes = nlohmann::json(id).dump().size() + 1;
        if (idBytes > kChunkBudget) {
            log.warn("[translator] 要撤回的 id 过大（" + std::to_string(idBytes) + " 字节），已跳过");
            continue;
        }
        if (!chunk.empty() && chunkBytes + idBytes > kChunkBudget) {
            flush();
        }
        chunk.push_back(id);
        chunkBytes += idBytes;
        ++removed;
    }
    flush();
    if (removed > 0) {
        log.info("[translator] 已撤回 " + std::to_string(removed) + " 条不再产出的条目");
    }
}

void SourcePublisher::publishItems(const std::vector<std::string>& items) {
    const shot::EventBus bus(m_host);
    if (!bus.available()) {
        shot::Logger(m_host).warn("[translator] 宿主没有事件总线，语言包无法贡献到 i18n_source_list");
        return;
    }
    const shot::Logger log(m_host);
    std::string chunk;  // 已拼好的 items 数组内容（逗号分隔的条目 JSON）
    std::size_t chunkBytes = 0;
    std::size_t published = 0;
    std::size_t chunks = 0;
    const auto flush = [&]() {
        if (chunk.empty()) {
            return;
        }
        // owner 用 JSON 序列化（本插件 id 是固定 ASCII，这里只是不假设它永远安全）。
        const std::string payload = std::string("{\"list\":\"") + kSourceListId +
                                    "\",\"owner\":" + nlohmann::json(m_owner).dump() +
                                    ",\"items\":[" + chunk + "]}";
        if (bus.publish("lists.add", payload)) {
            ++chunks;
        } else {
            log.warn("[translator] 发布 lists.add 失败（总线不可用或队列满），部分译文可能不生效");
        }
        chunk.clear();
        chunkBytes = 0;
    };
    for (const std::string& item : items) {
        const std::size_t itemBytes = item.size() + 1;
        if (itemBytes > kChunkBudget) {
            // 单条就超预算：发出去宿主的 64 KiB 上限也会整条丢弃，不如当场跳过并说明。
            log.warn("[translator] 单条条目过大（" + std::to_string(itemBytes) +
                     " 字节，预算 " + std::to_string(kChunkBudget) + "），已跳过");
            continue;
        }
        const std::size_t separator = chunk.empty() ? 0 : 1;
        if (!chunk.empty() && chunkBytes + separator + itemBytes > kChunkBudget) {
            flush();
        }
        if (!chunk.empty()) {
            chunk.push_back(',');
        }
        chunk += item;
        chunkBytes += separator + itemBytes;
        ++published;
    }
    flush();
    if (published > 0) {
        log.info("[translator] i18n_source_list 已发布 " + std::to_string(published) + " 条条目（" +
                 std::to_string(chunks) + " 块）");
    }
}

std::set<std::string> SourcePublisher::loadedOwners() const {
    std::set<std::string> owners;
    const shot::PluginRegistry registry(m_host);
    if (!registry.available()) {
        return owners;
    }
    const std::uint32_t count = registry.installedCount();
    for (std::uint32_t index = 0; index < count; ++index) {
        shot::PluginStatus status;
        if (registry.installedInfo(index, status) && status.state == SHOT_PLUGIN_STATE_LOADED) {
            owners.insert(status.id);
        }
    }
    return owners;
}

}  // namespace translator
