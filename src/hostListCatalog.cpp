#include "hostListCatalog.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "PluginInstances/base.h"
#include "config.h"
#include "hostConfigurable.h"
#include "hostI18n.h"
#include "hostServices.h"
#include "pluginManager.h"

// ============================================================================
//  hostListCatalog.cpp —— 通用列表目录的实现
//
//  四块职责都落在这里（一个类对应一个实现文件）：
//    1. 只读服务：list_count / list_id / list_json（两段式、按列表缓存序列化）；
//    2. 事件协议：lists.add / lists.remove / lists.update / lists.commit 的宿主端，
//       以及 lists.updated / lists.changed 的发布；
//    3. config_list 的生成：core + 所有 shot.configurable 实例的实时投影；
//    4. plugin_list 的生成：已安装插件记录 + metadata 的只读投影（设置页插件页）。
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
const char kI18nSourceListId[] = "i18n_source_list";    /**< 插件可贡献：语言表 */
const char kI18nListId[] = "i18n_list";                 /**< 宿主生成：当前语言快照 */
const char kHotkeyListId[] = "hotkey_list";             /**< 插件可贡献：全局热键目录 */
const char kPluginListId[] = "plugin_list";              /**< 宿主生成：已安装插件清单 */

// 入口插件 id：设置页就活在这个插件里，插件管理表据此把它的复选框置灰。
// 宿主只投影一个 entry 标记，不把"入口"做成通用能力/ABI 字段——它是设置页
// 这一层的约定（document/SETTINGS.md §3.1），换入口插件时只改这一处与设置页。
const char kEntryPluginId[] = "settings_ui.main";

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
    // 语言包是从 .json 文件直接拼出来的，文件头常带 UTF-8 BOM；BOM 不是合法的
    // JSON 前缀，不剥掉整条负载都会被判成坏 JSON（表现为"插件文案全都不生效"）。
    const char* begin = payload.data();
    std::size_t length = payload.size();
    if (length >= 3 && static_cast<unsigned char>(begin[0]) == 0xEF &&
        static_cast<unsigned char>(begin[1]) == 0xBB &&
        static_cast<unsigned char>(begin[2]) == 0xBF) {
        begin += 3;
        length -= 3;
    }
    nlohmann::json data = nlohmann::json::parse(begin, begin + length, nullptr, false);
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

// ---- i18n 源条目的解析与合并 ----------------------------------------------
// i18n_source_list 的条目协议（document/I18N.md §2.1）：
//   {"id":"<key>","texts":{"<tag>":"…"},"target":"<owner>","priority":300}
// target 缺省 = 贡献者自己；priority 缺省 100（内置表 100、官方包 200、
// 用户包 300）。旧插件只写 id+texts，所以下面这两个缺省值就是"旧数据行为
// 完全不变"的落点：等价于 target=自己、priority=100。
constexpr std::int32_t kCorePriority = 100;

// 语言声明条目的 id 前缀：`@language/<tag>` 只扩展语言目录，不作文本条目。
constexpr std::string_view kLanguageDeclarationPrefix = "@language/";

// i18n_source_list 的内部存储键分隔符：条目协议的身份是 (target, id)，但通用列表
// 条目的存储身份是 (owner, id)，其中 owner 是**贡献者插件 id**。同一个贡献者
// （典型是翻译插件）会替多个 target 发布同名 id（每个插件都有 plugin.name /
// plugin.description / config.enabled.label…），如果直接按 (owner, id) 存，这些
// 条目会互相覆盖，最后一条胜出、其余 target 的译文连同贡献者自己的内置表条目一起
// 从 i18n_list 消失（表现为插件页某些插件的名字/描述永远停在 metadata 原文，切
// 语言也不跟着变）。所以带 target 的条目存储时把 target 并进 id，投影时再按条目
// JSON 里的 target 还原成 (target, id)。
// 分隔符用 0x1F（ASCII Unit Separator）：不会出现在语言标签、插件 id 或文本 key 里。
constexpr char kI18nStorageKeySeparator = '\x1f';

/**
 * @brief 生成 i18n_source_list 条目的内部存储 id（其它列表原样返回）。
 *
 * 没有 target 的旧式条目（贡献者自己的内置表）保持裸 id：它们与带 target 的条目
 * 不会再撞键，合并投影时仍按 (contributor, id) 落到贡献者自己身上。
 *
 * @param[in] id     条目 JSON 里声明的文本 key。
 * @param[in] target 条目 JSON 里的 target；空串 = 贡献者自己。
 * @return 用于 upsertEntry 的存储 id。
 */
std::string i18nStorageId(const std::string& id, const std::string& target) {
    if (target.empty()) {
        return id;
    }
    std::string key = target;
    key.push_back(kI18nStorageKeySeparator);
    key += id;
    return key;
}

/**
 * @brief lists.remove 的 id 匹配：去掉 i18n_source_list 存储 id 的 target 前缀再比。
 *
 * 贡献者撤回时只知道自己的文本 key（"plugin.name"），不知道宿主内部怎么存；
 * 同一个 key 可能对多个 target 各存了一份，撤回就是"我贡献的这些 key 全撤"。
 *
 * @param[in] storedId 条目内部的存储 id。
 * @param[in] ids      撤回负载里的 id 集合。
 * @return true = 该条目应被删除。
 */
bool i18nStoredIdMatches(const std::string& storedId, const std::set<std::string>& ids) {
    if (ids.count(storedId) != 0) {
        return true;
    }
    const std::size_t separator = storedId.find(kI18nStorageKeySeparator);
    return separator != std::string::npos && ids.count(storedId.substr(separator + 1)) != 0;
}

// config_list 的 owner_name_key：插件显示名固定用这个 key，core 沿用 core.owner_name
// （core 的 owner_name 本身就是 key，消费方按 display(owner,name) 解析）。
const char kPluginNameKey[] = "plugin.name";
const char kCoreOwnerNameKey[] = "core.owner_name";

/** @brief 一条解析后的 i18n_source_list 条目。 */
struct I18nSourceItem {
    std::string owner;                        /**< 供文本的 target owner（core 或插件 id） */
    std::string id;                           /**< 文本 key；语言声明条目里是声明的语言标签 */
    std::int32_t priority = kCorePriority;    /**< 合并优先级；大的覆盖小的 */
    std::map<std::string, std::string> texts; /**< 规范语言 -> 文本（空串已剔除） */
    bool declaration = false;                 /**< true = @language/ 语言声明 */
    std::string declaredName;                 /**< 声明里的显示名（texts[tag] 优先） */
    std::string declaredFallback;             /**< 声明里的 fallback */
};

/** @brief 合并后的一个 (target,id)：语言 -> 文本 + 每个语言胜出的优先级。 */
struct I18nMergedText {
    std::map<std::string, std::string> texts;       /**< 规范语言 -> 文本 */
    std::map<std::string, std::int32_t> priorities; /**< 规范语言 -> 胜出优先级 */
};

/** @brief (target, id)：i18n_list 里一条文本条目的身份。 */
using I18nKey = std::pair<std::string, std::string>;
/** @brief (target,id) -> 各语言候选。 */
using I18nTable = std::map<I18nKey, I18nMergedText>;

/**
 * @brief 去掉字符串开头的 UTF-8 BOM。
 *
 * 语言包文件常年带 BOM；插件把它原样拼进 JSON 字符串时，BOM 会变成第一个 key
 * （或 id）的开头，查询方永远匹配不上，而日志里又看不出区别。
 * @param[in,out] text 待处理的字符串。
 */
void stripBomInPlace(std::string& text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
}

/**
 * @brief 解析一条 i18n_source_list 条目（坏条目安静丢弃，调用方只跳过它）。
 *
 * 兼容两种历史写法：`texts` 是对象（正常）；语言声明条目的 `texts` 直接是字符串
 * （只给一个语言名）。`@meta` 既可能是字符串也可能是对象（语言表借用了这个键），
 * 声明里只取它的 name/fallback 作补充，不当作文本 key。
 *
 * @param[in]  contributor 贡献者 owner（target 缺省值）。
 * @param[in]  valueJson   条目 JSON 文本。
 * @param[out] out         解析结果；成功时填好。
 * @return true = 是一条可用的文本条目或语言声明；false = 丢弃。
 */
bool parseI18nSourceItem(const std::string& contributor, const std::string& valueJson,
                         I18nSourceItem& out) {
    const nlohmann::json item = parseJsonObject(valueJson);
    if (!item.is_object()) {
        return false;
    }
    std::string id = jsonFieldString(item, "id");
    stripBomInPlace(id);
    if (id.empty() || id == "@meta") {
        return false;  // @meta 是 i18n_list 自己的保留 id（handleAdd 也拒绝过）
    }
    out.declaration = id.starts_with(kLanguageDeclarationPrefix);
    if (out.declaration) {
        out.id = Host::I18n::normalizeTag(id.substr(kLanguageDeclarationPrefix.size()));
        if (out.id.empty()) {
            return false;
        }
    } else {
        out.id = std::move(id);
    }

    std::string target = jsonFieldString(item, "target");
    stripBomInPlace(target);
    out.owner = target.empty() ? contributor : target;

    if (const auto priorityIt = item.find("priority");
        priorityIt != item.end() && priorityIt->is_number_integer()) {
        const std::int64_t value = priorityIt->get<std::int64_t>();
        out.priority = static_cast<std::int32_t>(
            std::clamp<std::int64_t>(value, std::numeric_limits<std::int32_t>::min(),
                                     std::numeric_limits<std::int32_t>::max()));
    }

    const auto textsIt = item.find("texts");
    if (textsIt != item.end()) {
        if (textsIt->is_object()) {
            for (auto it = textsIt->begin(); it != textsIt->end(); ++it) {
                if (!it->is_string()) {
                    continue;  // 表元数据 / 坏值：只认字符串
                }
                std::string tag = Host::I18n::normalizeTag(it.key());
                stripBomInPlace(tag);
                std::string text = it->get<std::string>();
                stripBomInPlace(text);
                if (tag.empty() || tag == "@meta" || text.empty()) {
                    continue;  // 空串视为缺失：作者拿它占位，不能当译文
                }
                out.texts[tag] = std::move(text);
            }
        } else if (textsIt->is_string() && out.declaration) {
            std::string text = textsIt->get<std::string>();
            stripBomInPlace(text);
            if (!text.empty()) {
                out.texts[out.id] = std::move(text);
            }
        }
    }

    if (!out.declaration) {
        return !out.texts.empty();
    }
    // 显示名：声明自己的语言文本 > "@meta"（字符串/对象）的 name > 标签本身。
    if (const auto nameIt = out.texts.find(out.id); nameIt != out.texts.end()) {
        out.declaredName = nameIt->second;
    }
    out.declaredFallback = Host::I18n::normalizeTag(jsonFieldString(item, "fallback"));
    if (const auto metaIt = item.find("@meta"); metaIt != item.end()) {
        if (metaIt->is_string()) {
            std::string meta = metaIt->get<std::string>();
            stripBomInPlace(meta);
            if (out.declaredName.empty() && !meta.empty()) {
                out.declaredName = std::move(meta);
            }
        } else if (metaIt->is_object()) {
            std::string metaName = jsonFieldString(*metaIt, "name");
            stripBomInPlace(metaName);
            if (out.declaredName.empty() && !metaName.empty()) {
                out.declaredName = std::move(metaName);
            }
            if (out.declaredFallback.empty()) {
                out.declaredFallback =
                    Host::I18n::normalizeTag(jsonFieldString(*metaIt, "fallback"));
            }
        }
    }
    return true;
}

/**
 * @brief 按"priority 大者胜、同 priority 后到者胜"把一条文本并进表。
 * @param[in,out] table    合并表。
 * @param[in]     key      (target,id)。
 * @param[in]     language 规范语言标签。
 * @param[in]     priority 这条候选的优先级。
 * @param[in]     text     文本；空串在调用方已剔除。
 */
void mergeText(I18nTable& table, const I18nKey& key, const std::string& language,
               std::int32_t priority, std::string text) {
    I18nMergedText& entry = table[key];
    const auto existing = entry.priorities.find(language);
    if (existing != entry.priorities.end() && priority < existing->second) {
        return;  // 低优先级不覆盖已有译文
    }
    entry.priorities[language] = priority;
    entry.texts[language] = std::move(text);
}

/**
 * @brief 取 (target,id) 在指定语言下的文本（统一回退链，空串 = 没有可用文本）。
 * @param[in] table    合并表。
 * @param[in] key      (target,id)。
 * @param[in] language 目标语言。
 * @param[in] fallback 该语言的 fallback。
 * @return 选中的文本；没有时返回空串。
 */
std::string selectMergedText(const I18nTable& table, const I18nKey& key,
                             const std::string& language, const std::string& fallback) {
    const auto it = table.find(key);
    if (it == table.end()) {
        return {};
    }
    return Host::I18n::selectText(it->second.texts, language, fallback);
}

/**
 * @brief 这个语言在合并表里有没有任何一条非空文本（决定是否标"未翻译"）。
 *
 * 只看精确匹配：zh-TW 能回退到 zh-CN 的简体，但它自己没有译文，仍然要标
 * "未翻译"——那正是提醒用户"选了会看到回退语言"的地方。
 *
 * @param[in] table    合并表。
 * @param[in] language 规范语言标签。
 * @return true = 至少一条该语言的文本。
 */
bool hasTranslation(const I18nTable& table, const std::string& language) {
    for (const auto& item : table) {
        const auto it = item.second.texts.find(language);
        if (it != item.second.texts.end() && !it->second.empty()) {
            return true;
        }
    }
    return false;
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

/**
 * @brief 把注册表状态值投影成稳定字符串（plugin_list 的 state 字段）。
 *
 * 数值枚举是 ABI（SHOT_PLUGIN_STATE_*），但列表 schema 用字符串表达，原因是
 * 消费方（设置页/第三方 UI）不该为了显示状态而硬编码宿主枚举值。字符串就是
 * 注册表状态的规范名，未知值统一给 "unknown"——比让整个列表解析失败好。
 *
 * @param[in] state SHOT_PLUGIN_STATE_* 的值。
 * @return "loaded" / "disabled" / "failed" / "unknown" 之一。
 */
const char* pluginStateName(std::uint32_t state) {
    switch (state) {
        case SHOT_PLUGIN_STATE_LOADED:
            return "loaded";
        case SHOT_PLUGIN_STATE_DISABLED:
            return "disabled";
        case SHOT_PLUGIN_STATE_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

/**
 * @brief 把 metadata 里的 provides 名字 / requires 声明转成 JSON 数组。
 * @param[in] meta        metadata 条目；nullptr 表示插件没有 metadata，返回空数组。
 * @param[in] forRequires true = 取 requires 原文；false = 取 provides 名字。
 * @return provides 是名字数组；requires 保留 metadata 原文（如 "core>=0.1.0"），
 *         让消费方能拿到版本约束，而不是只有提供者名字。
 * @note 参数不叫 requires：那是 C++20 关键字（与 metadata 字段同名也不能当标识符）。
 */
nlohmann::json pluginMetaArray(const Plugin::PluginMetaEntry* meta, bool forRequires) {
    nlohmann::json out = nlohmann::json::array();
    if (meta == nullptr) {
        return out;
    }
    if (forRequires) {
        for (const Plugin::PluginRequirement& requirement : meta->requireList) {
            out.push_back(requirement.getRaw());
        }
        return out;
    }
    for (const std::string& name : meta->provides) {
        out.push_back(name);
    }
    return out;
}

}  // namespace

namespace Host {

HostListCatalog::HostListCatalog(std::mutex& lifecycleMutex)
    : m_lifecycleMutex(&lifecycleMutex), m_mainThread(std::this_thread::get_id()) {
    activeCatalog.store(this);

    // 预置 core 的托盘命令。托盘只是渲染器，菜单里必须永远有这三项：插件被禁用 /
    // 加载失败时，用户仍然要能截屏、打开设置、退出。owner = "core" 的条目在
    // lists.remove 里被拒绝，所以插件删不掉它们。
    // label 是文案 key：托盘按 display(core, label) 用 i18n_list 解析（document/I18N.md）。
    nlohmann::json capture = {
        {"id", "capture.request"},
        {"owner", kCoreOwner},  // 消费方按 display(owner,label) 解析文本 key
        {"label", "capture.request"},
        {"event", "capture.request"},
        {"data", ""},
        {"order", 10},
        {"separator", false},
    };
    nlohmann::json regionCapture = {
        {"id", "region.capture"},
        {"owner", kCoreOwner},
        {"label", "region.capture"},
        {"event", "capture.request"},
        // 这条只是"请开始框选"的入口：mode=region 且不带 region，采集流水线会忽略它
        // （不抓屏、不落盘），由 plugins/region_selector 弹出覆盖层；框选确认后它会再发
        // 一条带 region 坐标的 capture.request，那才是真正要裁剪采集的请求。
        {"data", R"json({"mode":"region"})json"},
        {"order", 15},
        {"separator", false},
    };
    nlohmann::json settings = {
        {"id", "settings.open"},
        {"owner", kCoreOwner},
        {"label", "settings.open"},
        {"event", "settings.open"},
        {"data", ""},
        {"order", 20},
        {"separator", false},
    };
    nlohmann::json quit = {
        {"id", "app.quit"},
        {"owner", kCoreOwner},
        {"label", "app.quit"},
        {"event", "app.quit"},
        {"data", ""},
        {"order", 100},
        {"separator", false},
    };
    ListState& tray = m_lists[kTrayCommandListId];
    tray.entries.push_back(Entry{kCoreOwner, "capture.request", 10, dumpJson(capture)});
    tray.entries.push_back(Entry{kCoreOwner, "region.capture", 15, dumpJson(regionCapture)});
    tray.entries.push_back(Entry{kCoreOwner, "settings.open", 20, dumpJson(settings)});
    tray.entries.push_back(Entry{kCoreOwner, "app.quit", 100, dumpJson(quit)});

    // config_list 先注册成空列表：消费方在 init 阶段读不会拿到"列表不存在"，
    // 真正的条目由 HostRuntime::init 里的 rebuildConfigListLocked 填。
    ListState& config = m_lists[kConfigListId];
    config.generated = true;
    config.cache = "[]";
    config.cacheValid = true;

    // i18n_list 同理：它是宿主生成的当前语言快照，启动时由 HostRuntime::init 填；
    // 先注册成空列表，设置页/插件在 init 阶段读到的是 [] 而不是"列表不存在"。
    ListState& i18n = m_lists[kI18nListId];
    i18n.generated = true;
    i18n.cache = "[]";
    i18n.cacheValid = true;

    // plugin_list 同理：设置页打开时可能早于第一次 rebuildConfigListLocked，
    // 先注册成空列表让消费方读到 []（"还没有插件清单"），而不是未知 list id。
    ListState& pluginList = m_lists[kPluginListId];
    pluginList.generated = true;
    pluginList.cache = "[]";
    pluginList.cacheValid = true;

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
    // 宿主生成的只读投影（config_list / i18n_list / plugin_list）：插件只能通过
    // lists.update 写 config_list 的值，直接 add 会造出第二份真相，宁可拒绝也别让
    // 消费方看到两种数据来源。注意这个判断必须对"当前已注册为 generated 的任意
    // 列表"生效。
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto stateIt = m_lists.find(list);
        if (stateIt != m_lists.end() && stateIt->second.generated) {
            spdlog::warn("lists.add 试图写宿主生成的 {}，已丢弃（写值请用 lists.update）", list);
            return;
        }
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
        // 列表内部的去重键：i18n_source_list 带 target 的条目要把 target 并进去，
        // 否则同一贡献者为不同 target 发布的同名 id 会互相覆盖（见 i18nStorageId）。
        std::string storageId = id;
        // i18n_source_list 的额外校验（协议见 document/I18N.md §2.1）：@meta 是
        // i18n_list 的保留 id；target 只能是 core 或当前已加载的插件 id，否则这段
        // 文本永远没人查得到，不如在入口拒掉并留 WARN。
        if (list == kI18nSourceListId) {
            if (id == "@meta") {
                spdlog::warn("lists.add: i18n_source_list 不允许 id=\"@meta\"（保留），跳过"
                             "（owner={}）", owner);
                continue;
            }
            const auto targetIt = item.find("target");
            const std::string target = jsonFieldString(item, "target");
            if (targetIt != item.end() && !targetIt->is_string()) {
                spdlog::warn("lists.add: i18n_source_list 条目 {} 的 target 不是字符串，跳过"
                             "（owner={}）", id, owner);
                continue;
            }
            if (!target.empty() && target != kCoreOwner && !ownerLoaded(target)) {
                spdlog::warn("lists.add: i18n_source_list 条目 {} 的 target={} 不是已加载插件，"
                             "跳过（owner={}）", id, target, owner);
                continue;
            }
            const auto priorityIt = item.find("priority");
            if (priorityIt != item.end() && !priorityIt->is_number_integer()) {
                spdlog::warn("lists.add: i18n_source_list 条目 {} 的 priority 不是整数，"
                             "按缺省 100 处理（owner={}）", id, owner);
            }
            storageId = i18nStorageId(id, target);
        }
        // 宿主为每条注入贡献者 id：tray_command_list 这类消费方要按
        // display(owner,label) 用 i18n_list 解析文本 key。条目自带 owner 时以列表级
        // owner 为准（插件不能借别人的 owner 解析文案）。
        nlohmann::json stored = item;
        stored["owner"] = owner;
        upsertEntry(list, owner, storageId, jsonOrder(item), dumpJson(stored));
        ++accepted;
    }
    if (accepted > 0) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingChanged.insert(list);
            if (list == kI18nSourceListId) {
                m_i18nDirty = true;  // 新文案要先投影进 i18n_list 才对消费方可见
            }
            if (list == kHotkeyListId) {
                // hotkey.main 会按 hotkey_list 动态暴露配置项；目录变了必须同一轮重建
                // config_list，否则新贡献的热键在设置页里看不到（tick 在 flush 前处理）。
                m_configListStale = true;
            }
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
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto stateIt = m_lists.find(list);
        if (stateIt != m_lists.end() && stateIt->second.generated) {
            spdlog::warn("lists.remove 不能删宿主生成的 {}，已丢弃", list);
            return;
        }
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
            // 双保险：正常路径在上面已经按"任意 generated 列表"拒绝过。
            spdlog::warn("lists.remove 不能删宿主生成的 {}，已丢弃", list);
            return;
        }
        const std::size_t before = state.entries.size();
        std::erase_if(state.entries, [&](const Entry& entry) {
            if (entry.owner != owner) {
                return false;
            }
            if (removeAll) {
                return true;  // ids 省略 = 撤回该 owner 在这个列表里的全部条目
            }
            if (ids.count(entry.id) != 0) {
                return true;
            }
            // i18n_source_list 带 target 的条目存储 id 里含 target 前缀：按文本 key 撤回。
            return list == kI18nSourceListId && i18nStoredIdMatches(entry.id, ids);
        });
        removed = before - state.entries.size();
        if (removed > 0) {
            state.cacheValid = false;
            m_pendingChanged.insert(list);
            if (list == kI18nSourceListId) {
                m_i18nDirty = true;
            }
            if (list == kHotkeyListId) {
                // 条目被撤回（插件卸载/改贡献）后，provider 暴露的配置项也要跟着消失。
                m_configListStale = true;
            }
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
    const auto publishError = [&response](const Host::I18n::Message& error) {
        response["results"] = nlohmann::json::array();
        response["error"] = error.localized();
        publishJson(kListsUpdatedTopic, response);
        spdlog::warn("lists.update 失败：{}", error.chinese());  // 日志固定中文
    };
    if (!data.is_object()) {
        publishError(Host::I18n::Message("list.error.bad_payload"));
        return;
    }
    if (jsonFieldString(data, "list") != kConfigListId) {
        publishError(Host::I18n::Message("list.error.update_only_config"));
        return;
    }
    const auto changesIt = data.find("changes");
    if (changesIt == data.end() || !changesIt->is_array()) {
        publishError(Host::I18n::Message("list.error.changes_array"));
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
                result["reason"] =
                    Host::I18n::Message("list.error.bad_param").localized();
                ++failCount;
                results.push_back(std::move(result));
                continue;
            }
            const Provider* provider = findProvider(*snapshot, owner.c_str());
            if (provider == nullptr || provider->config == nullptr ||
                provider->config->set_config == nullptr) {
                result["ok"] = false;
                result["reason"] = Host::I18n::Message("list.error.provider_unavailable").localized();
                ++failCount;
                results.push_back(std::move(result));
                continue;
            }
            const std::int32_t rc =
                provider->config->set_config(provider->self, id.c_str(), value.c_str());
            if (rc == 0) {
                // 值与插件内存已经同步；宿主配置只记内存，落盘由 lists.commit 触发。
                core::Config::getInstance().set(owner + "/" + id, core::ConfigItem(value));
                if (owner == HostConfigurable::id() && id == Host::I18n::preferenceKey()) {
                    // 语言偏好立即生效（set_config 里已经切了 I18n），列表快照由下一轮
                    // tick 重建并统一发 lists.changed —— 一轮只发一次。
                    m_i18nDirty = true;
                }
                result["ok"] = true;
                result["reason"] = "";
                ++okCount;
            } else {
                result["ok"] = false;
                result["reason"] = Host::I18n::Message("list.error.plugin_rejected",
                                                      std::to_string(rc)).localized();
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
        response["error"] = Host::I18n::Message("list.error.commit_only_config").localized();
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
    response["error"] = Host::I18n::Message("list.error.save_failed").localized();
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
                // 插件显示名的文本 key 固定为 "plugin.name"：插件可以在自己的语言表里
                // 给 target=自己、id=plugin.name 提供译文，设置页按
                // resolve(owner, owner_name_key) ?? owner_name 显示。core 继续用
                // core.owner_name（core 的 owner_name 本身就是 key，旧消费方不受影响）。
                {"owner_name_key",
                 provider.id == HostConfigurable::id() ? kCoreOwnerNameKey : kPluginNameKey},
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
    // 语言目录变化引起的重建到此完成：core/language 的 options 已经与目录一致，
    // 再置着标记会让下一轮 tick 白重建一次。
    m_configListStale = false;

    // 插件清单与配置清单共享同一个刷新时机（插件加载完成、启停、配置提交），
    // 在这里顺带重建 plugin_list：调用方不必记两个入口，也不会出现"配置刷新了、
    // 插件描述还是上一次"的半旧状态。metadata 是声明、不随语言变化，所以内容
    // 没变时 installGeneratedList 不会发 lists.changed。
    rebuildPluginListLocked();
}

void HostListCatalog::rebuildPluginListLocked() {
    // 管理器未绑定（启动早期）时安装空清单：消费方读到 []，走注册表快照兜底。
    Plugin::PluginManager* manager = managerPointer();
    std::vector<Plugin::InstalledPlugin> installed;
    if (manager != nullptr) {
        installed = manager->getInstalledPlugins();
    }

    // 同一个 DLL 可能声明多个插件：一次重建里按 dllPath 只读一次 metadata 尾部。
    // 不做跨重建缓存：metadata 可能在宿主运行期间被重装/替换，重建频率又低
    // （插件加载/启停、配置提交），读文件尾比维护失效规则更简单可靠。
    struct PackageMeta {
        bool attempted = false;          /**< 是否已尝试读过这个 DLL 的 metadata */
        bool available = false;          /**< metadata 是否读取成功 */
        Plugin::PluginPackageMeta meta;  /**< 解析结果；available 为 false 时无意义 */
    };
    std::map<std::string, PackageMeta> packages;

    std::vector<Entry> entries;
    entries.reserve(installed.size());
    for (const Plugin::InstalledPlugin& info : installed) {
        if (info.id.empty()) {
            continue;  // 记录缺少 id 说明管理器内部数据异常，跳过而不是投影坏条目
        }
        PackageMeta& package = packages[info.dllPath];
        if (!package.attempted) {
            package.attempted = true;
            std::string error;
            // 读失败是正常情况（第三方包可能没有 metadata），不逐条 WARN。
            package.available = package.meta.readFromFile(info.dllPath, error);
        }
        const Plugin::PluginMetaEntry* meta = nullptr;
        if (package.available) {
            for (const Plugin::PluginMetaEntry& candidate : package.meta.getMetaData()) {
                if (candidate.id == info.id) {
                    meta = &candidate;
                    break;
                }
            }
        }

        // name/version 优先取 metadata 的声明值（注册表里的 name 是 metadata 名，
        // 但 get_info 运行期可能覆盖成别的文本）；没有 metadata 时退回已安装记录，
        // 记录本身也没有可用名字时用 id 兜底。
        std::string name;
        if (meta != nullptr && !meta->name.empty()) {
            name = meta->name;
        } else if (!info.name.empty()) {
            name = info.name;
        } else {
            name = info.id;
        }
        std::string version;
        if (meta != nullptr && !meta->version.empty()) {
            version = meta->version;
        } else {
            version = info.version;
        }

        nlohmann::json entry = {
            {"id", info.id},
            {"name", name},
            {"description", (meta != nullptr) ? meta->description : std::string()},
            {"version", version},
            {"state", pluginStateName(info.state)},
            // enabled 与注册表 is_enabled 语义一致：加载失败但用户没关它 → true。
            {"enabled", manager != nullptr && manager->isPluginEnabled(info.id)},
            {"entry", info.id == kEntryPluginId},
            {"provides", pluginMetaArray(meta, /*forRequires=*/false)},
            {"requires", pluginMetaArray(meta, /*forRequires=*/true)},
        };
        entries.push_back(Entry{info.id, info.id, 0, dumpJson(entry)});
    }
    installGeneratedList(kPluginListId, std::move(entries));
}

bool HostListCatalog::configListStale() const { return m_configListStale; }

void HostListCatalog::requestI18nRebuild() { m_i18nDirty.store(true); }

void HostListCatalog::rebuildI18nListIfDirty() {
    if (!m_i18nDirty.load()) {
        return;
    }
    rebuildI18nList();
}

void HostListCatalog::rebuildI18nList() {
    Host::I18n& i18n = Host::I18n::instance();

    // 先把 i18n_source_list 的条目拷出来（不持 m_mutex 时解析 JSON，避免长时间锁）。
    std::vector<Entry> sources;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto sourceIt = m_lists.find(kI18nSourceListId);
        if (sourceIt != m_lists.end()) {
            sources = sourceIt->second.entries;
        }
    }

    // 第一步：解析源条目并收集语言声明。目录必须在选文本之前就定下来：同一个
    // 标签的 fallback 会影响本次重建的每一条文本，边投影边扩目录会让同一个快照
    // 里不同条目按不同的目录规则选文本。
    std::vector<I18nSourceItem> items;
    items.reserve(sources.size());
    std::map<std::string, Host::I18n::ExternalLanguage> declarations;
    for (const Entry& source : sources) {
        if (source.owner.empty() || source.owner == kCoreOwner) {
            continue;  // core 的文本由内置表提供，插件不可能以 core 身份贡献条目
        }
        I18nSourceItem item;
        if (!parseI18nSourceItem(source.owner, source.valueJson, item)) {
            continue;
        }
        if (!item.declaration) {
            items.push_back(std::move(item));
            continue;
        }
        // 多个包声明同一个语言时按先到者补字段：显示名/fallback 谁给了用谁的，
        // 都不给就按目录规则回退（注册表 > 标签本身 / 默认语言）。
        Host::I18n::ExternalLanguage& declared = declarations[item.id];
        declared.tag = item.id;
        if (declared.name.empty()) {
            declared.name = std::move(item.declaredName);
        }
        if (declared.fallback.empty()) {
            declared.fallback = std::move(item.declaredFallback);
        }
    }
    std::vector<Host::I18n::ExternalLanguage> declaredLanguages;
    declaredLanguages.reserve(declarations.size());
    for (auto& [tag, language] : declarations) {
        declaredLanguages.push_back(std::move(language));
    }
    const bool catalogChanged = i18n.setExternalLanguages(std::move(declaredLanguages));
    if (catalogChanged) {
        // core/language 的 ENUM 候选里多了/少了标签，options 只能整表重序列化。
        // 这里只置标记：本函数承诺不调用插件能力，真正的重建由 HostRuntime::tick
        // 在持生命周期锁时调 rebuildConfigListLocked() 完成。
        m_configListStale = true;
        spdlog::info("i18n: 语言目录变化，共 {} 个标签，config_list 待重建",
                     i18n.catalog().size());
    }

    // 第二步：一次取定当前语言。@meta 与所有条目共用这一份快照——语言切换的瞬间
    // 若每条各自重取，会读出"@meta 说 en-US、正文还是中文"的撕裂快照。
    const std::string language = i18n.language();
    const std::string fallback = i18n.fallbackTag(language);

    // 第三步：合并候选。内置表按 priority=100 垫底；允许 target=core 的条目录入
    // 是因为语言包可能比宿主更懂某种语言（官方包 200、用户包 300）。覆盖按语言
    // 逐个进行：包里没提供当前语言时，内置的 zh-CN/en-US 仍然生效——这样"core
    // 文案被声明覆盖"不会把未命中语言的回退一起改掉。
    I18nTable table;
    for (const Host::I18n::CoreText& core : Host::I18n::displayTexts()) {
        for (const Host::I18n::LanguageText& text : core.texts) {
            if (text.tag == nullptr || text.text == nullptr || text.text[0] == '\0') {
                continue;
            }
            mergeText(table, {kCoreOwner, core.id}, Host::I18n::normalizeTag(text.tag),
                      kCorePriority, text.text);
        }
    }
    for (const I18nSourceItem& item : items) {
        for (const auto& [tag, text] : item.texts) {
            mergeText(table, {item.owner, item.id}, tag, item.priority, text);
        }
    }

    std::vector<Entry> entries;
    entries.reserve(2 + Host::I18n::displayTexts().size() + i18n.catalog().size() +
                   table.size());

    // 首条 @meta：消费方（插件 helper 与设置页/托盘）用它判断当前语言与回退语言。
    nlohmann::json meta = {
        {"id", "@meta"},
        {"language", language},
        {"fallback", fallback},
    };
    entries.push_back(Entry{"", "@meta", 0, dumpJson(meta)});

    // core 文案：按内置表顺序输出，文本来自合并结果（包只覆盖它提供了译文的语言）。
    std::set<std::string> emittedCore;
    for (const Host::I18n::CoreText& core : Host::I18n::displayTexts()) {
        emittedCore.insert(core.id);
        const std::string text =
            selectMergedText(table, {kCoreOwner, core.id}, language, fallback);
        if (text.empty()) {
            continue;  // 兜底也选不出文本：不发布，消费方会显示 key 原文
        }
        nlohmann::json entry = {
            {"owner", kCoreOwner},
            {"id", core.id},
            {"text", text},
        };
        entries.push_back(Entry{kCoreOwner, core.id, 0, dumpJson(entry)});
    }

    // 语言下拉的显示名：原生名不随界面语言变化（声明 > 注册表 > 标签本身）。
    // 没有任何译文的语言追加"（未翻译）"后缀——不隐藏候选，但要让人知道选了会
    // 看到回退语言。语言包也可以用 target=core、id=<tag> 给本地化显示名（优先级更高）。
    const std::string untranslatedSuffix = i18n.textIn(language, "language.untranslated");
    for (const Host::I18n::CatalogLanguage& catalogLanguage : i18n.catalog()) {
        emittedCore.insert(catalogLanguage.tag);
        std::string name =
            selectMergedText(table, {kCoreOwner, catalogLanguage.tag}, language, fallback);
        if (name.empty()) {
            name = catalogLanguage.name.empty() ? catalogLanguage.tag : catalogLanguage.name;
        }
        if (!hasTranslation(table, catalogLanguage.tag)) {
            name += untranslatedSuffix;
        }
        nlohmann::json entry = {
            {"owner", kCoreOwner},
            {"id", catalogLanguage.tag},
            {"text", name},
        };
        entries.push_back(Entry{kCoreOwner, catalogLanguage.tag, 0, dumpJson(entry)});
    }

    // 其余 (target,id)：消费方按 display(owner,id) 查。core 已经输出过的 id 不再
    // 重复——同一个 (owner,id) 在 i18n_list 里只能有一条，否则解析结果取决于
    // 消费方扫到哪一条。
    for (const auto& [key, merged] : table) {
        if (key.first == kCoreOwner && emittedCore.count(key.second) != 0) {
            continue;
        }
        const std::string text = Host::I18n::selectText(merged.texts, language, fallback);
        if (text.empty()) {
            continue;
        }
        nlohmann::json entry = {
            {"owner", key.first},
            {"id", key.second},
            {"text", text},
        };
        entries.push_back(Entry{key.first, key.second, 0, dumpJson(entry)});
    }

    installGeneratedList(kI18nListId, std::move(entries));
    m_i18nDirty.store(false);
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
                continue;  // config_list / i18n_list / plugin_list 由宿主按当前状态重建
            }
            const std::size_t before = state.entries.size();
            std::erase_if(state.entries,
                          [&](const Entry& entry) { return entry.owner == owner; });
            if (state.entries.size() != before) {
                state.cacheValid = false;
                m_pendingChanged.insert(listId);
                if (listId == kI18nSourceListId) {
                    m_i18nDirty = true;  // 插件走了，它的文案要从 i18n_list 里消失
                }
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
