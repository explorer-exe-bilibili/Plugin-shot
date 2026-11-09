#include "hostListCatalog.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "PluginInstances/base.h"
#include "config.h"
#include "hostConfigurable.h"
#include "hostServices.h"
#include "pluginManager.h"

// ============================================================================
//  hostListCatalog.cpp —— 通用列表目录的实现
//
//  三块职责都落在这里（一个类对应一个实现文件）：
//    1. 只读服务：list_count / list_id / list_json（两段式、按列表缓存序列化）；
//    2. 事件协议：lists.add / lists.remove / lists.update / lists.commit 的宿主端，
//       以及 lists.updated / lists.changed 的发布；
//    3. config_list 的生成：core + 所有 shot.configurable 实例的实时投影。
//
//  编译期约定（与头文件里的线程模型一一对应）：
//    · m_mutex 只保护数据，绝不在持锁时调用插件；
//    · 会调用插件能力的路径（applyStoredLocked / rebuildConfigListLocked /
//      lists.update）由调用方持有生命周期锁，本文件只依赖、不重复加锁；
//    · JSON 负载解析一律用不抛异常的 parse(..., allow_exceptions=false)：插件
//      传上来的东西不可信，坏 JSON 只 warn 丢弃。
// ============================================================================

namespace {

// 进程内唯一的列表目录。服务表函数拿不到 ctx（契约里 ctx 是 host_context），
// 所以用这个指针回指宿主；HostListCatalog 构造时登记、析构时注销。
// 用原子指针是因为服务函数可能在插件线程上读它，而注销发生在宿主退出时。
std::atomic<Host::HostListCatalog*> activeCatalog{nullptr};

// ---- 事件主题与列表 id ------------------------------------------------------
// 主题全小写、点分隔（document/EVENT_BUS.md §4）；list id 是数据形状的标识，
// 已注册的列表由 document/LISTS.md 登记。
const char kListsAddTopic[] = "lists.add";          /**< 插件 → 宿主：贡献条目 */
const char kListsRemoveTopic[] = "lists.remove";    /**< 插件 → 宿主：撤回条目 */
const char kListsUpdateTopic[] = "lists.update";    /**< 消费方 → 宿主：写 config_list 值 */
const char kListsCommitTopic[] = "lists.commit";    /**< 消费方 → 宿主：落盘 */
const char kListsUpdatedTopic[] = "lists.updated";  /**< 宿主 → 所有：写值结果 */
const char kListsChangedTopic[] = "lists.changed";  /**< 宿主 → 所有：列表内容变化 */
const char kConfigAppliedTopic[] = "config.applied";/**< 宿主 → 所有：批量配置已落盘 */

const char kConfigListId[] = "config_list";             /**< 宿主生成：配置项目录 */
const char kTrayCommandListId[] = "tray_command_list";  /**< 插件可贡献：托盘命令 */

// "core" 是宿主自己的 owner：core 的托盘命令是预置项，插件既不能添加、也不能删除，
// 否则用户可能把「退出」菜单从托盘里弄丢。
const char kCoreOwner[] = "core";

// order 缺省值：没有 order 的条目排在所有显式 order 之后；同 order 保持插入顺序。
constexpr std::int32_t kDefaultOrder = 1000;

/**
 * @brief kind 是否是契约里已定义的配置种类。
 *
 * 插件声明是它自己写的，宿主必须校验：设置页拿到未知 kind 会画不出来。
 * @param[in] kind SHOT_CONFIG_KIND_* 的值。
 * @return true = 1..7 之一。
 */
bool isKnownKind(std::uint32_t kind) {
    return kind >= SHOT_CONFIG_KIND_BOOL && kind <= SHOT_CONFIG_KIND_PATH;
}

/**
 * @brief 把跨边界回调里拿到的结构体序列化成对外 JSON（不抛异常）。
 * @param[in] value 任意 JSON 值。
 * @return UTF-8 文本；非法 UTF-8 用替换字符兜底，绝不让序列化把宿主带崩。
 */
std::string dumpJson(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

/**
 * @brief 取 JSON 对象里的字符串字段。
 * @return 字段不存在、类型不对时返回空串（调用方按"缺失"处理）。
 */
std::string jsonFieldString(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

/**
 * @brief 解析事件负载为 JSON 对象。
 *
 * 用 allow_exceptions=false 的解析：事件来自插件，任何坏负载都只应被丢弃。
 * @return 解析成功且是对象时返回该对象；否则返回 null（is_object() 为 false）。
 */
nlohmann::json parseJsonObject(const std::string& payload) {
    if (payload.empty()) {
        return nlohmann::json();
    }
    nlohmann::json data = nlohmann::json::parse(payload, nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
        return nlohmann::json();
    }
    return data;
}

/**
 * @brief 把 JSON 数字转成条目排序权重，越界按 int32 边界截断。
 * @return 合法整数/浮点时的权重；缺省或类型不对时返回 kDefaultOrder。
 */
std::int32_t jsonOrder(const nlohmann::json& item) {
    const auto it = item.find("order");
    if (it == item.end()) {
        return kDefaultOrder;
    }
    if (it->is_number_integer()) {
        const std::int64_t value = it->get<std::int64_t>();
        if (value > std::numeric_limits<std::int32_t>::max()) {
            return std::numeric_limits<std::int32_t>::max();
        }
        if (value < std::numeric_limits<std::int32_t>::min()) {
            return std::numeric_limits<std::int32_t>::min();
        }
        return static_cast<std::int32_t>(value);
    }
    if (it->is_number_float()) {
        const double value = it->get<double>();
        if (value > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
            return std::numeric_limits<std::int32_t>::max();
        }
        if (value < static_cast<double>(std::numeric_limits<std::int32_t>::min())) {
            return std::numeric_limits<std::int32_t>::min();
        }
        return static_cast<std::int32_t>(value);
    }
    return kDefaultOrder;
}

/**
 * @brief 两段式服务的公共拷贝逻辑：buf 为空只查长度，否则写入并要求留 NUL 位。
 * @param[in]  text    要返回的文本。
 * @param[out] buf     调用方缓冲；可为空。
 * @param[in]  bufSize buf 的字节数。
 * @return >= 0 = 文本字节数（不含 NUL）；-1 = 缓冲不足或文本长到无法用 int32 表达。
 */
std::int32_t copyOut(const std::string& text, char* buf, std::uint32_t bufSize) {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return -1;
    }
    const auto length = static_cast<std::int32_t>(text.size());
    if (buf == nullptr) {
        return length;  // 第一段：只查长度
    }
    if (bufSize <= text.size()) {
        return -1;  // 必须给结尾 NUL 留一个字节（契约要求 buf_size > 长度）
    }
    std::memcpy(buf, text.data(), text.size());
    buf[text.size()] = '\0';
    return length;
}

/**
 * @brief 发布一条 JSON 负载到事件总线（非阻塞入队）。
 * @param[in] topic   主题。
 * @param[in] payload JSON 负载。
 */
void publishJson(const char* topic, const nlohmann::json& payload) {
    const std::string text = dumpJson(payload);
    Host::HostServices::eventBusPublish(nullptr, topic, text.data(),
                                        static_cast<std::uint32_t>(text.size()));
}

/**
 * @brief 当前线程 id 的稳定字符串形式（日志用）。
 *
 * std::thread::id 没有被 std::format / fmt 直接支持，借 ostream 打一次；
 * 只用于启动与首次写路径的一次性日志，不在热路径上。
 */
std::string currentThreadId() {
    std::ostringstream out;
    out << std::this_thread::get_id();
    return out.str();
}

}  // namespace

namespace Host {

HostListCatalog::HostListCatalog(std::mutex& lifecycleMutex)
    : m_lifecycleMutex(&lifecycleMutex), m_mainThread(std::this_thread::get_id()) {
    activeCatalog.store(this);

    // 预置 core 的托盘命令。托盘只是渲染器，菜单里必须永远有这三项：插件被禁用 /
    // 加载失败时，用户仍然要能截屏、打开设置、退出。owner = "core" 的条目在
    // lists.remove 里被拒绝，所以插件删不掉它们。
    nlohmann::json capture = {
        {"id", "capture.request"},
        {"label", "截屏"},
        {"event", "capture.request"},
        {"data", ""},
        {"order", 10},
        {"separator", false},
    };
    nlohmann::json settings = {
        {"id", "settings.open"},
        {"label", "打开设置"},
        {"event", "settings.open"},
        {"data", ""},
        {"order", 20},
        {"separator", false},
    };
    nlohmann::json quit = {
        {"id", "app.quit"},
        {"label", "退出"},
        {"event", "app.quit"},
        {"data", ""},
        {"order", 100},
        {"separator", false},
    };
    ListState& tray = m_lists[kTrayCommandListId];
    tray.entries.push_back(Entry{kCoreOwner, "capture.request", 10, dumpJson(capture)});
    tray.entries.push_back(Entry{kCoreOwner, "settings.open", 20, dumpJson(settings)});
    tray.entries.push_back(Entry{kCoreOwner, "app.quit", 100, dumpJson(quit)});

    // config_list 先注册成空列表：消费方在 init 阶段读不会拿到"列表不存在"，
    // 真正的条目由 HostRuntime::init 里的 rebuildConfigListLocked 填。
    ListState& config = m_lists[kConfigListId];
    config.generated = true;
    config.cache = "[]";
    config.cacheValid = true;

    spdlog::info("HostListCatalog: 列表写路径绑定到宿主 main 线程（thread={}）", currentThreadId());
}

HostListCatalog::~HostListCatalog() {
    // 只有"当前这一个"才注销：防御将来万一出现第二个实例把别人的登记清掉。
    HostListCatalog* expected = this;
    activeCatalog.compare_exchange_strong(expected, nullptr);
}

void HostListCatalog::bind(Plugin::PluginManager* manager) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_manager = manager;
}

void HostListCatalog::unbind() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_manager = nullptr;
    m_current.reset();
    m_previous.reset();
}

const ShotHostPluginListsV1* HostListCatalog::service() {
    // 表是静态数据，函数指针只在类内部可取（回调是私有成员）。
    static const ShotHostPluginListsV1 kService = {
        .struct_size = sizeof(ShotHostPluginListsV1),
        .version     = 1,
        .list_count  = &HostListCatalog::listCount,
        .list_id     = &HostListCatalog::listId,
        .list_json   = &HostListCatalog::listJson,
    };
    return activeCatalog.load() != nullptr ? &kService : nullptr;
}

void HostListCatalog::handleEvent(const std::string& topic, const std::string& payload) {
    // 列表变更只允许 main 线程。事件总线已经在 main 线程分发，这里是最后一道
    // 防线：以后若有人图省事在插件线程直接调进来，会立刻在日志里暴露。
    if (std::this_thread::get_id() != m_mainThread) {
        spdlog::warn("列表事件 {} 来自非宿主 main 线程，已丢弃（列表变更只允许 main 线程）", topic);
        return;
    }
    if (topic == kListsAddTopic) {
        if (!m_writeThreadLogged) {
            m_writeThreadLogged = true;
            spdlog::info("HostListCatalog: 列表写路径首次变更处理（thread={}，宿主 main 线程）",
                         currentThreadId());
        }
        handleAdd(payload);
        return;
    }
    if (topic == kListsRemoveTopic) {
        handleRemove(payload);
        return;
    }
    if (topic == kListsUpdateTopic) {
        handleUpdate(payload);
        return;
    }
    if (topic == kListsCommitTopic) {
        handleCommit(payload);
        return;
    }
    if (topic == kListsChangedTopic || topic == kListsUpdatedTopic) {
        // 宿主自己发布的通知主题。插件若重新 publish 回来会造成回环，直接忽略。
        return;
    }
    spdlog::warn("未知列表事件 {}，已忽略（已知：lists.add/remove/update/commit）", topic);
}

void HostListCatalog::handleAdd(const std::string& payload) {
    const nlohmann::json data = parseJsonObject(payload);
    if (!data.is_object()) {
        spdlog::warn("lists.add 负载不是合法 JSON 对象，已丢弃");
        return;
    }
    const std::string list = jsonFieldString(data, "list");
    const std::string owner = jsonFieldString(data, "owner");
    if (list.empty() || owner.empty()) {
        spdlog::warn("lists.add 缺少 list/owner 字段，已丢弃");
        return;
    }
    // config_list 是宿主生成的只读投影：插件只能通过 lists.update 写值，
    // 直接 add 会造出第二份真相，宁可拒绝也别让消费方看到两种数据来源。
    if (list == kConfigListId) {
        spdlog::warn("lists.add 试图写宿主生成的 {}，已丢弃（写值请用 lists.update）", kConfigListId);
        return;
    }
    if (owner == kCoreOwner) {
        spdlog::warn("lists.add 使用了宿主保留 owner \"core\"，已丢弃");
        return;
    }
    // 事件总线不携带发送者身份，宿主只能校验 owner 是当前已加载的插件 id；
    // 这防的是"插件没加载就声明条目"的手滑，不是防同进程恶意插件。
    if (!ownerLoaded(owner)) {
        spdlog::warn("lists.add 的 owner={} 不是已加载插件，已丢弃", owner);
        return;
    }
    const auto itemsIt = data.find("items");
    if (itemsIt == data.end() || !itemsIt->is_array()) {
        spdlog::warn("lists.add 的 items 不是数组，已丢弃（list={}, owner={}）", list, owner);
        return;
    }

    std::size_t accepted = 0;
    for (const nlohmann::json& item : *itemsIt) {
        if (!item.is_object()) {
            spdlog::warn("lists.add 的条目不是 JSON 对象，跳过（list={}, owner={}）", list, owner);
            continue;
        }
        const std::string id = jsonFieldString(item, "id");
        if (id.empty()) {
            spdlog::warn("lists.add 的条目缺少 id，跳过（list={}, owner={}）", list, owner);
            continue;
        }
        upsertEntry(list, owner, id, jsonOrder(item), dumpJson(item));
        ++accepted;
    }
    if (accepted > 0) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingChanged.insert(list);
        }
        spdlog::info("lists.add: list={} owner={} 接受 {} 条", list, owner, accepted);
    }
}

void HostListCatalog::handleRemove(const std::string& payload) {
    const nlohmann::json data = parseJsonObject(payload);
    if (!data.is_object()) {
        spdlog::warn("lists.remove 负载不是合法 JSON 对象，已丢弃");
        return;
    }
    const std::string list = jsonFieldString(data, "list");
    const std::string owner = jsonFieldString(data, "owner");
    if (list.empty() || owner.empty()) {
        spdlog::warn("lists.remove 缺少 list/owner 字段，已丢弃");
        return;
    }
    if (list == kConfigListId) {
        spdlog::warn("lists.remove 不能删宿主生成的 {}，已丢弃", kConfigListId);
        return;
    }
    if (owner == kCoreOwner) {
        spdlog::warn("lists.remove 不能删宿主预置的 core 条目，已丢弃");
        return;
    }
    if (!ownerLoaded(owner)) {
        spdlog::warn("lists.remove 的 owner={} 不是已加载插件，已丢弃", owner);
        return;
    }

    // ids 省略（或给空数组）= 撤回该 owner 在这个列表里的全部条目；
    // 非空数组 = 只撤回列出的 id。非数组是明显的负载错误，整条丢弃。
    std::set<std::string> ids;
    const auto idsIt = data.find("ids");
    if (idsIt != data.end() && !idsIt->is_null()) {
        if (!idsIt->is_array()) {
            spdlog::warn("lists.remove 的 ids 不是数组，已丢弃（list={}, owner={}）", list, owner);
            return;
        }
        for (const nlohmann::json& idValue : *idsIt) {
            if (idValue.is_string() && !idValue.get<std::string>().empty()) {
                ids.insert(idValue.get<std::string>());
            }
        }
    }
    const bool removeAll = ids.empty();

    std::size_t removed = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto listIt = m_lists.find(list);
        if (listIt == m_lists.end()) {
            return;  // 列表本来就不存在：删除是幂等的空操作
        }
        ListState& state = listIt->second;
        if (state.generated) {
            return;
        }
        const std::size_t before = state.entries.size();
        std::erase_if(state.entries, [&](const Entry& entry) {
            return entry.owner == owner && (removeAll || ids.count(entry.id) != 0);
        });
        removed = before - state.entries.size();
        if (removed > 0) {
            state.cacheValid = false;
            m_pendingChanged.insert(list);
        }
    }
    if (removed > 0) {
        spdlog::info("lists.remove: list={} owner={} 删除 {} 条", list, owner, removed);
    }
}

void HostListCatalog::handleUpdate(const std::string& payload) {
    const nlohmann::json data = parseJsonObject(payload);

    // 载荷里带了 tag 就原样回显，让消费方（设置页）能把请求和响应配对。
    nlohmann::json response = nlohmann::json::object();
    response["list"] = kConfigListId;
    if (data.is_object() && data.contains("tag")) {
        response["tag"] = data["tag"];
    }
    // 出错时也发 lists.updated：消费方可能在等 tag 匹配的响应，静默丢弃会让它
    // 一直等到超时；error 字段让"宿主不支持 / 参数错"可以被界面显示出来。
    const auto publishError = [&response](const std::string& error) {
        response["results"] = nlohmann::json::array();
        response["error"] = error;
        publishJson(kListsUpdatedTopic, response);
        spdlog::warn("lists.update 失败：{}", error);
    };
    if (!data.is_object()) {
        publishError("负载不是合法 JSON 对象");
        return;
    }
    if (jsonFieldString(data, "list") != kConfigListId) {
        publishError("lists.update 只支持 config_list");
        return;
    }
    const auto changesIt = data.find("changes");
    if (changesIt == data.end() || !changesIt->is_array()) {
        publishError("changes 必须是数组");
        return;
    }

    nlohmann::json results = nlohmann::json::array();
    std::size_t okCount = 0;
    std::size_t failCount = 0;
    {
        // 调用插件 set_config 必须在生命周期锁内：否则设置页线程写值的同时宿主
        // 可能正在热卸载这个插件（tick 拿同一把锁）。
        std::lock_guard<std::mutex> lock(*m_lifecycleMutex);
        const auto snapshot = snapshotProvidersLocked();
        for (const nlohmann::json& change : *changesIt) {
            nlohmann::json result = nlohmann::json::object();
            std::string owner;
            std::string id;
            std::string value;
            bool valueOk = false;
            if (change.is_object()) {
                owner = jsonFieldString(change, "owner");
                id = jsonFieldString(change, "id");
                const auto valueIt = change.find("value");
                if (valueIt != change.end() && valueIt->is_string()) {
                    value = valueIt->get<std::string>();
                    valueOk = true;
                }
            }
            result["owner"] = owner;
            result["id"] = id;
            if (owner.empty() || id.empty() || !valueOk) {
                result["ok"] = false;
                result["reason"] = "参数非法：owner/id/value 必须都是字符串";
                ++failCount;
                results.push_back(std::move(result));
                continue;
            }
            const Provider* provider = findProvider(*snapshot, owner.c_str());
            if (provider == nullptr || provider->config == nullptr ||
                provider->config->set_config == nullptr) {
                result["ok"] = false;
                result["reason"] = "提供者未加载或不支持 shot.configurable";
                ++failCount;
                results.push_back(std::move(result));
                continue;
            }
            const std::int32_t rc =
                provider->config->set_config(provider->self, id.c_str(), value.c_str());
            if (rc == 0) {
                // 值与插件内存已经同步；宿主配置只记内存，落盘由 lists.commit 触发。
                core::Config::getInstance().set(owner + "/" + id, core::ConfigItem(value));
                result["ok"] = true;
                result["reason"] = "";
                ++okCount;
            } else {
                result["ok"] = false;
                result["reason"] = "插件拒绝（返回码 " + std::to_string(rc) + "）";
                ++failCount;
            }
            results.push_back(std::move(result));
        }
        // 值已经推给插件，立刻重建 config_list：设置页收到 lists.updated 后重新
        // 读快照时，看到的就是新值，不会出现"响应说成功、列表还是旧值"的窗口。
        rebuildConfigListLocked();
    }
    response["results"] = std::move(results);
    publishJson(kListsUpdatedTopic, response);
    spdlog::info("lists.update: {} 项成功，{} 项失败（config_list 已重建）", okCount, failCount);
}

void HostListCatalog::handleCommit(const std::string& payload) {
    const nlohmann::json data = parseJsonObject(payload);
    nlohmann::json response = nlohmann::json::object();
    response["list"] = kConfigListId;
    if (data.is_object() && data.contains("tag")) {
        response["tag"] = data["tag"];
    }
    if (!data.is_object() || jsonFieldString(data, "list") != kConfigListId) {
        response["results"] = nlohmann::json::array();
        response["error"] = "lists.commit 只支持 config_list";
        publishJson(kListsUpdatedTopic, response);
        spdlog::warn("lists.commit 失败：只支持 config_list");
        return;
    }

    // 落盘不碰插件，不需要生命周期锁；saveConfigFile 内部自己串行化并返回真实结果。
    const bool saved = core::Config::getInstance().saveConfigFile();
    {
        std::lock_guard<std::mutex> lock(*m_lifecycleMutex);
        rebuildConfigListLocked();
    }
    if (saved) {
        // 成功走既有约定：发 config.applied（空负载），设置页用它结束"应用中…"。
        Host::HostServices::eventBusPublish(nullptr, kConfigAppliedTopic, nullptr, 0);
        spdlog::info("lists.commit: config_list 已落盘，发布 {}", kConfigAppliedTopic);
        return;
    }
    // 失败没有对应的"applied"，用 lists.updated 的 error 字段告诉消费方原因。
    response["results"] = nlohmann::json::array();
    response["error"] = "保存失败";
    publishJson(kListsUpdatedTopic, response);
    spdlog::warn("lists.commit: 配置落盘失败，已发布 lists.updated(error)");
}

bool HostListCatalog::ownerLoaded(const std::string& owner) const {
    Plugin::PluginManager* manager = managerPointer();
    if (manager == nullptr) {
        return false;
    }
    for (const auto& instance : manager->getAllInstances()) {
        if (instance != nullptr && instance->getId() == owner) {
            return true;
        }
    }
    return false;
}

Plugin::PluginManager* HostListCatalog::managerPointer() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_manager;
}

void HostListCatalog::dropSnapshots() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_current.reset();
    m_previous.reset();
}

std::shared_ptr<const std::vector<HostListCatalog::Provider>>
HostListCatalog::snapshotProvidersLocked() {
    Plugin::PluginManager* manager = managerPointer();

    std::vector<Provider> providers;
    providers.push_back(coreProvider());
    if (manager != nullptr) {
        // 实时快照：config_list 随插件启停立即变化，列表读取方收到 plugins.changed /
        // lists.changed 后重查即可，不需要宿主维护增量的配置项索引。
        for (const auto& instance : manager->getAllInstances()) {
            if (instance == nullptr) {
                continue;
            }
            void* iface = instance->queryInterface(SHOT_IFACE_CONFIGURABLE, 1);
            if (iface == nullptr) {
                continue;  // 没声明可配置：正常情况，不是错误
            }
            const auto* config = static_cast<const ShotConfigurableV1*>(iface);
            // 缺任何必备操作都当"不可配置"：老插件/坏插件不该把设置页带崩。
            if (!SHOT_HAS_FIELD(config, ShotConfigurableV1, set_config) ||
                config->get_config_count == nullptr || config->get_config_desc == nullptr ||
                config->get_config == nullptr || config->set_config == nullptr) {
                continue;
            }
            Provider provider;
            provider.id = instance->getId();
            if (provider.id.empty()) {
                continue;  // 没有 id 就无法按 id 定位，跳过
            }
            provider.name = instance->getName();
            provider.version = instance->getVersion();
            provider.instance = instance;
            provider.self = instance->nativeHandle();
            provider.config = config;
            providers.push_back(std::move(provider));
        }
    }

    auto snapshot = std::make_shared<const std::vector<Provider>>(std::move(providers));
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 保留上一份：服务函数刚返回的字符串指针可能还被调用方拿着，等下一次刷新
        // 才允许回收（契约要求字符串至少活到"下一次目录调用"）。
        m_previous = std::move(m_current);
        m_current = snapshot;
    }
    return snapshot;
}

const HostListCatalog::Provider* HostListCatalog::findProvider(
    const std::vector<Provider>& providers, const char* id) {
    if (id == nullptr) {
        return nullptr;
    }
    for (const Provider& provider : providers) {
        if (provider.id == id) {
            return &provider;
        }
    }
    return nullptr;
}

HostListCatalog::Provider HostListCatalog::coreProvider() {
    Provider provider;
    provider.id = HostConfigurable::id();
    provider.name = HostConfigurable::name();
    provider.version = HostConfigurable::version();
    provider.self = nullptr;  // 宿主设置没有实例，能力函数忽略 self
    provider.config = HostConfigurable::service();
    return provider;
}

std::int32_t HostListCatalog::applyStoredToProvider(const Provider& provider) {
    if (provider.config == nullptr || provider.config->get_config_count == nullptr) {
        return 0;
    }
    core::Config& config = core::Config::getInstance();
    std::int32_t applied = 0;
    const std::uint32_t count = provider.config->get_config_count(provider.self);
    for (std::uint32_t item = 0; item < count; ++item) {
        ShotConfigDesc desc{};
        desc.struct_size = sizeof(ShotConfigDesc);
        if (provider.config->get_config_desc(provider.self, item, &desc) != 0) {
            continue;
        }
        if (desc.key == nullptr || desc.key[0] == '\0') {
            continue;
        }
        // 存盘键带提供者前缀：core/logging.level、example.invert/keep_alpha。
        const std::string configKey = provider.id + "/" + desc.key;
        std::string value;
        if (config.contains(configKey)) {
            value = config.get(configKey).toString();
        } else if (desc.default_value != nullptr) {
            // 第一次运行没有旧值：用默认值并立刻记进配置，落盘后配置文件就是完整的。
            value = desc.default_value;
            config.set(configKey, core::ConfigItem(value));
        } else {
            continue;  // 没有已存值也没有默认值：这个键不落盘、不灌值
        }
        if (provider.config->set_config(provider.self, desc.key, value.c_str()) == 0) {
            ++applied;
        }
    }
    return applied;
}

std::int32_t HostListCatalog::applyStoredLocked() {
    // 未绑定管理器时只剩 core：core 的配置项也要回灌（日志级别/控制台开关），
    // 所以这里不因为 manager 为空就提前返回，只把插件循环跳过。
    const auto snapshot = snapshotProvidersLocked();
    std::int32_t applied = 0;
    for (const Provider& provider : *snapshot) {
        applied += applyStoredToProvider(provider);
    }
    return applied;
}

bool HostListCatalog::readConfigValue(const Provider& provider, const char* key,
                                      std::string& out) {
    out.clear();
    if (provider.config == nullptr || provider.config->get_config == nullptr || key == nullptr) {
        return false;
    }
    const std::int32_t length = provider.config->get_config(provider.self, key, nullptr, 0);
    if (length < 0) {
        return false;
    }
    out.assign(static_cast<std::size_t>(length), '\0');
    if (length == 0) {
        return true;  // 空串是合法值
    }
    out.resize(static_cast<std::size_t>(length) + 1);  // 给 NUL 留位
    const std::int32_t got = provider.config->get_config(
        provider.self, key, out.data(), static_cast<std::uint32_t>(out.size()));
    if (got < 0) {
        out.clear();
        return false;
    }
    out.resize(static_cast<std::size_t>(got));
    return true;
}

void HostListCatalog::rebuildConfigListLocked() {
    // 先在不持 m_mutex 的情况下把插件数据读完：调用插件能力期间绝不能拿着数据锁。
    const auto snapshot = snapshotProvidersLocked();
    std::vector<Entry> entries;
    for (const Provider& provider : *snapshot) {
        if (provider.config == nullptr) {
            continue;
        }
        const std::uint32_t count = provider.config->get_config_count(provider.self);
        for (std::uint32_t item = 0; item < count; ++item) {
            ShotConfigDesc desc{};
            desc.struct_size = sizeof(ShotConfigDesc);
            if (provider.config->get_config_desc(provider.self, item, &desc) != 0) {
                continue;
            }
            if (desc.key == nullptr || desc.key[0] == '\0') {
                spdlog::warn("Provider {} 声明了空 key 的配置项 #{}，已跳过", provider.id, item);
                continue;
            }
            if (!isKnownKind(desc.kind)) {
                spdlog::warn("Provider {} 的配置项 {} 使用了未知 kind={}，已跳过",
                             provider.id, desc.key, desc.kind);
                continue;
            }
            std::string value;
            if (!readConfigValue(provider, desc.key, value)) {
                // 声明了却读不出来：保留条目（让用户看见它）但值为空并记录警告，
                // 比直接丢掉更难被察觉的故障更容易排查。
                spdlog::warn("Provider {} 的配置项 {} 读取失败，值按空串处理", provider.id, desc.key);
                value.clear();
            }
            nlohmann::json entry = {
                {"owner", provider.id},
                {"owner_name", provider.name},
                {"id", desc.key},
                {"label", desc.label != nullptr ? desc.label : desc.key},
                {"kind", desc.kind},
                {"value", value},
                {"default", desc.default_value != nullptr ? desc.default_value : ""},
                {"options", desc.options != nullptr ? desc.options : ""},
                {"min", desc.min},
                {"max", desc.max},
                {"step", desc.step},
                {"tooltip", desc.tooltip != nullptr ? desc.tooltip : ""},
            };
            Entry entryState;
            entryState.owner = provider.id;
            entryState.id = desc.key;
            entryState.valueJson = dumpJson(entry);
            entries.push_back(std::move(entryState));
        }
    }
    installGeneratedList(kConfigListId, std::move(entries));
}

void HostListCatalog::upsertEntry(const std::string& list, const std::string& owner,
                                  const std::string& id, std::int32_t order,
                                  const std::string& valueJson) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ListState& state = m_lists[list];  // 任意 list id 第一次出现时自动注册
    if (state.generated) {
        return;  // 双保险：config_list 不接受插件条目（handleAdd 已经拒绝过）
    }
    const auto existing = std::find_if(
        state.entries.begin(), state.entries.end(),
        [&](const Entry& entry) { return entry.owner == owner && entry.id == id; });
    if (existing != state.entries.end()) {
        // 同 (list, owner, id) 覆盖：保留原插入位置，只换内容与权重。
        existing->order = order;
        existing->valueJson = valueJson;
    } else {
        state.entries.push_back(Entry{owner, id, order, valueJson});
    }
    // stable_sort 让同 order 的条目保持插入顺序：菜单项不会因为一次无关的 add
    // 而互相换位（消费方刷新时那种"闪一下"就是这样来的）。
    std::stable_sort(state.entries.begin(), state.entries.end(),
                     [](const Entry& left, const Entry& right) { return left.order < right.order; });
    state.cacheValid = false;
}

void HostListCatalog::installGeneratedList(const std::string& list, std::vector<Entry> entries) {
    const std::string text = serializeEntries(entries);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ListState& state = m_lists[list];
        if (!state.cacheValid) {
            state.cache = serializeEntries(state.entries);
            state.cacheValid = true;
        }
        changed = state.cache != text;
        state.entries = std::move(entries);
        state.cache = text;
        state.cacheValid = true;
        state.generated = true;
        if (changed) {
            m_pendingChanged.insert(list);
        }
    }
    if (changed) {
        spdlog::info("HostListCatalog: {} 内容变化，已排入 lists.changed", list);
    }
}

void HostListCatalog::removeOwner(const std::string& owner) {
    if (owner.empty() || owner == kCoreOwner) {
        return;
    }
    if (std::this_thread::get_id() != m_mainThread) {
        spdlog::warn("removeOwner({}) 在非宿主 main 线程调用，已忽略", owner);
        return;
    }
    std::vector<std::string> changedLists;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [listId, state] : m_lists) {
            if (state.generated) {
                continue;  // config_list 由 rebuildConfigListLocked 重建
            }
            const std::size_t before = state.entries.size();
            std::erase_if(state.entries,
                          [&](const Entry& entry) { return entry.owner == owner; });
            if (state.entries.size() != before) {
                state.cacheValid = false;
                m_pendingChanged.insert(listId);
                changedLists.push_back(listId);
            }
        }
    }
    for (const std::string& listId : changedLists) {
        spdlog::info("插件 {} 已卸载，清除其在 {} 的列表贡献", owner, listId);
    }
}

void HostListCatalog::flushChanged() {
    if (std::this_thread::get_id() != m_mainThread) {
        spdlog::warn("flushChanged 在非宿主 main 线程调用，已忽略");
        return;
    }
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pending.assign(m_pendingChanged.begin(), m_pendingChanged.end());
        m_pendingChanged.clear();
    }
    for (const std::string& listId : pending) {
        nlohmann::json payload = nlohmann::json::object();
        payload["list"] = listId;
        publishJson(kListsChangedTopic, payload);
        spdlog::info("发布 lists.changed: list={}", listId);
    }
}

std::string HostListCatalog::serializeEntries(const std::vector<Entry>& entries) {
    std::string text;
    text.reserve(entries.size() * 64 + 2);
    text.push_back('[');
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) {
            text.push_back(',');
        }
        text += entries[index].valueJson;
    }
    text.push_back(']');
    return text;
}

std::uint32_t HostListCatalog::listCount(void* /*ctx*/) {
    HostListCatalog* catalog = activeCatalog.load();
    if (catalog == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(catalog->m_mutex);
    return static_cast<std::uint32_t>(catalog->m_lists.size());
}

std::int32_t HostListCatalog::listId(void* /*ctx*/, std::uint32_t index, char* buf,
                                     std::uint32_t bufSize) {
    HostListCatalog* catalog = activeCatalog.load();
    if (catalog == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(catalog->m_mutex);
    if (index >= catalog->m_lists.size()) {
        return -1;
    }
    auto it = catalog->m_lists.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(index));
    return copyOut(it->first, buf, bufSize);
}

std::int32_t HostListCatalog::listJson(void* /*ctx*/, const char* listId, char* buf,
                                       std::uint32_t bufSize) {
    if (listId == nullptr) {
        return -1;
    }
    HostListCatalog* catalog = activeCatalog.load();
    if (catalog == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(catalog->m_mutex);
    const auto it = catalog->m_lists.find(listId);
    if (it == catalog->m_lists.end()) {
        return -1;  // 不认识的 list id：插件按"没有这个列表"降级
    }
    ListState& state = it->second;
    if (!state.cacheValid) {
        // 只在这个列表自上次变更后第一次读取时重建一次，之后直接返回缓存的
        // 字符串；消费方按事件刷新、别每帧轮询，序列化就不会成为热点。
        state.cache = serializeEntries(state.entries);
        state.cacheValid = true;
    }
    return copyOut(state.cache, buf, bufSize);
}

}  // namespace Host
