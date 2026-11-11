// 语言包插件的主体：配置回调、插件自身文案、重扫描编排。
//
// 这个文件只做"什么时候扫、扫完发布/撤回什么"，具体职责都在别的类里：
//   · 目录解析：pack_dirs.cpp（唯一的平台相关代码）
//   · 文件扫描：pack_scanner.cpp
//   · 单包解析：pack_loader.cpp
//   · 条目合并与分块发布：source_publisher.cpp
//
// 契约要点（include/plugin_api/configurable.h）：
//   · self 是 PluginInstance*，不是 TranslatorPlugin*：回调里先 state_of(self)；
//   · 不认识的 key 返回负值；结构太短返回 2；
//   · 异常绝不跨越 DLL 边界：start 与 set_config 内部收敛。
//
// 线程模型：lists.changed 回调在宿主 main 线程执行，扫描+发布都只在那里发生；
// set_config 可在任意线程调用，只置原子脏标记，不做 IO、不发事件。

#include "translator_plugin.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pack_dirs.h"
#include "pack_scanner.h"
#include "pack_types.h"
#include "plugin_api.h"
#include "plugin_entry.h"
#include "shot_plugin_helpers.h"
#include "source_publisher.h"

namespace translator {

namespace {

// 本插件的用户可见文案：两个配置项的 label/tooltip + 设置页里的插件分区名
// （config_list 的 owner_name_key = "plugin.name"，见 document/LISTS.md §2.4）。
// 配置描述符里写的是这些 key（见 translator_config.cpp），设置页按
// display("translator.main", key) = i18n_list[(owner, key)] ?? key 解析；
// key 拼写必须与 translator_config.cpp 完全一致，拼错会原样显示 key。
// 每语言一张 JSON 对象，@meta 必填，两张表的 key 集必须一致；JSON 文本里不能有
// 裸换行，而整行又可能超过 120 列，所以用相邻 raw 字面量拼接——拼出来仍是一行。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"语言包",)json"
    R"json("config.enabled.label":"启用外部语言包",)json"
    R"json("config.enabled.tooltip":)json"
    R"json("开启后扫描语言包目录，把包里的语言加进「语言」下拉，并用包里的译文覆盖内置文案；关闭即撤回",)json"
    R"json("config.pack_dirs.label":"语言包目录",)json"
    R"json("config.pack_dirs.tooltip":)json"
    R"json("留空使用官方 <exe>/files/lang 与用户目录；填了只用该目录（多个用分号分隔）"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"Language packs",)json"
    R"json("config.enabled.label":"Enable external language packs",)json"
    R"json("config.enabled.tooltip":)json"
    R"json("Scan the language pack folder, add its languages to the Language )json"
    R"json(list and override built-in texts; turning it off withdraws them",)json"
    R"json("config.pack_dirs.label":"Language pack folder",)json"
    R"json("config.pack_dirs.tooltip":)json"
    R"json("Empty = official <exe>/files/lang plus the user folder; )json"
    R"json(when set, only this folder is used (separate several with semicolons)"})json",
};

}  // namespace

TranslatorPlugin::TranslatorPlugin() = default;

TranslatorPlugin::~TranslatorPlugin() { stop(); }

bool TranslatorPlugin::start(const ShotHostServices* host) {
    m_host = host;
    // I18n 构造会订阅 lists.changed、loadTables 会发布 lists.add，两者都可能分配内存；
    // 本方法由 ABI 边界 create_instance 调用，异常绝不能穿过去，所以在这里收敛成 false。
    try {
        // owner 用插件身份证，避免与 metadata.json / kTranslator 里的字符串漂移。
        auto i18n = std::make_unique<shot::I18n>(host, kTranslator.id);
        if (!i18n->loadTables(kLangTables, std::size(kLangTables))) {
            shot::Logger(host).warn("[translator] 插件自身文案表解析失败，配置项文案回退为 key");
        }
        m_i18n = std::move(i18n);
    } catch (...) {
        m_i18n.reset();
        shot::Logger(host).warn("[translator] 插件自身文案初始化异常，配置项文案回退为 key");
    }

    // 订阅 lists.changed：EventBus 本身无状态（宿主侧只记住 user=this），临时对象
    // 析构不影响订阅；stop() 用同一个 this 退订。
    m_subscribed =
        shot::EventBus(host).subscribe("lists.changed", &TranslatorPlugin::onListsChanged, this);
    if (!m_subscribed) {
        shot::Logger(host).warn(
            "[translator] 订阅 lists.changed 失败：开启/修改语言包目录后要重启宿主才生效");
    }
    // 首次扫描不在这里做：create 阶段宿主还没 apply_stored 回灌配置（默认是关），
    // 现在扫等于按默认值白扫一次。等第一轮 lists.changed 再扫——启动时 config_list /
    // i18n_list 必然变化一次，apply_stored 也一定发生在第一轮 dispatch 之前。
    m_reloadPending.store(true);
    shot::Logger(host).info("[translator] 已就绪（默认关闭；在设置页开启后扫描语言包）");
    return true;
}

void TranslatorPlugin::stop() {
    if (m_subscribed) {
        shot::EventBus(m_host).unsubscribe(this);
        m_subscribed = false;
    }
    m_reloadPending.store(false);
    // 显式撤回自己贡献的条目（宿主在实例消失后也会自动清）。注意：宿主的
    // disableOne/unloadAll 是先摘实例再 destroy，所以这条 lists.remove 到宿主时
    // 可能被判成"owner 不是已加载插件"丢弃——那是预期行为，自动清理会兜住。
    withdrawContribution();
    m_i18n.reset();  // 析构会退订它自己的 lists.changed
    m_host = nullptr;
}

std::uint32_t TranslatorPlugin::configCount() const noexcept { return m_config.configCount(); }

std::int32_t TranslatorPlugin::configDesc(std::uint32_t index, ShotConfigDesc* out) const noexcept {
    return m_config.configDesc(index, out);
}

std::int32_t TranslatorPlugin::getConfig(const char* key, char* buf,
                                         std::uint32_t bufSize) const {
    return m_config.getValue(key, buf, bufSize);
}

std::int32_t TranslatorPlugin::setConfig(const char* key, const char* value) {
    const std::int32_t result = m_config.setValue(key, value);
    if (result == 0) {
        // 只登记脏标记：set_config 可能来自任意线程，而 lists.add 只允许宿主 main
        // 线程处理。下一轮 lists.changed 回调里再真正扫描（配置变化本身就会让
        // config_list 变化，所以这轮事件一定会来）。
        m_reloadPending.store(true);
    }
    return result;
}

void TranslatorPlugin::onListsChanged(void* user, const char* /*topic*/, const void* /*data*/,
                                      std::uint32_t /*len*/) {
    auto* self = static_cast<TranslatorPlugin*>(user);
    if (self == nullptr) {
        return;
    }
    // 负载里是哪个 list 与重扫描无关：只要"main 线程到了"且配置脏了就扫一次。
    self->applyPendingReload();
}

void TranslatorPlugin::applyPendingReload() {
    if (!m_reloadPending.exchange(false)) {
        return;
    }
    try {
        reload();
    } catch (...) {
        // 扫描读的是用户可改的文件，异常（分配失败等）不能穿回宿主的 dispatch 循环。
        shot::Logger(m_host).warn("[translator] 重新扫描语言包时发生异常，已忽略本轮改动");
    }
}

void TranslatorPlugin::reload() {
    if (m_host == nullptr) {
        return;
    }
    const shot::Logger log(m_host);
    std::set<std::string> nextIds;
    if (m_config.enabled()) {
        const std::vector<PackLocation> locations = PackDirs::resolve(m_config.packDirs());
        const std::vector<ParsedPack> packs = PackScanner(m_host).scan(locations);
        if (!packs.empty()) {
            nextIds = SourcePublisher(m_host, kTranslator.id).publish(packs);
            log.info("[translator] 语言包目录扫描完成，共 " + std::to_string(packs.size()) + " 个包");
        } else {
            log.info("[translator] 没有可用语言包，未贡献任何文本");
        }
    } else if (!m_publishedIds.empty()) {
        // 只有"这一轮之前确实贡献过"才值得报撤回：重复的关闭不刷日志。
        // 关闭 = 从没加载过：包里的译文不能继续覆盖内置文案，否则开关看起来关不掉。
        log.info("[translator] 已关闭，撤回本插件贡献的语言包条目");
    }
    // 先撤旧、再记新：宿主按 (target,id) 存条目，新一批里没有的 key 不会自动清，
    // 不撤就会留下"包删了、界面还是旧语言"的幽灵译文。撤回与发布在同一轮事件里
    // 排队，宿主处理完两者才有一次 i18n_list 重建，所以界面不会闪。
    removeObsolete(nextIds);
    m_publishedIds = std::move(nextIds);
    if (!m_publishedIds.empty()) {
        m_contributed = true;
    }
}

void TranslatorPlugin::removeObsolete(const std::set<std::string>& nextIds) {
    if (m_publishedIds.empty()) {
        return;
    }
    // 自身文案（shot::I18n 贡献的配置项 label/tooltip 等）不参与撤回，见头文件。
    const std::set<std::string>& own = ownTextKeys();
    std::set<std::string> stale;
    for (const std::string& id : m_publishedIds) {
        if (nextIds.count(id) == 0 && own.count(id) == 0) {
            stale.insert(id);
        }
    }
    if (stale.empty()) {
        return;
    }
    SourcePublisher(m_host, kTranslator.id).removeIds(stale);
}

const std::set<std::string>& TranslatorPlugin::ownTextKeys() {
    // 解析一次 kLangTables 得到键集：不另抄一份 key 列表，避免与语言表漂移。
    static const std::set<std::string> keys = [] {
        std::set<std::string> collected;
        for (const char* table : kLangTables) {
            const nlohmann::json doc = nlohmann::json::parse(table, nullptr, false);
            if (!doc.is_object()) {
                continue;
            }
            for (auto it = doc.begin(); it != doc.end(); ++it) {
                if (it.key() != "@meta") {
                    collected.insert(it.key());
                }
            }
        }
        return collected;
    }();
    return keys;
}

void TranslatorPlugin::withdrawContribution() {
    if (!m_contributed) {
        return;
    }
    m_contributed = false;
    m_publishedIds.clear();
    // 卸载路径用"按 owner 整段撤回"：本插件的自身文案也在同一 owner 名下，反正
    // 马上要一起消失；宿主在实例消失后还会自动清一遍。
    SourcePublisher(m_host, kTranslator.id).withdraw();
}

namespace {

// ---- shot.configurable 的 C 回调 -------------------------------------------
// 签名由契约固定，只能是自由函数（STYLE §6.7 的合法例外）；每个回调先 state_of
// 把 self（PluginInstance*）还原成插件状态，再转给类方法。

std::uint32_t config_count(void* self) {
    const auto* plugin = static_cast<const TranslatorPlugin*>(state_of(self));
    return (plugin == nullptr) ? 0 : plugin->configCount();
}

std::int32_t config_desc(void* self, std::uint32_t index, ShotConfigDesc* out) {
    const auto* plugin = static_cast<const TranslatorPlugin*>(state_of(self));
    return (plugin == nullptr) ? 1 : plugin->configDesc(index, out);
}

std::int32_t get_config(void* self, const char* key, char* buf, std::uint32_t bufSize) {
    const auto* plugin = static_cast<const TranslatorPlugin*>(state_of(self));
    return (plugin == nullptr) ? -1 : plugin->getConfig(key, buf, bufSize);
}

std::int32_t set_config(void* self, const char* key, const char* value) {
    auto* plugin = static_cast<TranslatorPlugin*>(state_of(self));
    if (plugin == nullptr) {
        return -1;
    }
    // 写目录字符串可能分配内存：边界上兜住，失败按"值非法"返回，不影响其它配置项。
    try {
        return plugin->setConfig(key, value);
    } catch (...) {
        return -1;
    }
}

const ShotConfigurableV1 kConfigurable = SHOT_OPS(
    ShotConfigurableV1,
    .get_config_count = &config_count,
    .get_config_desc  = &config_desc,
    .get_config       = &get_config,
    .set_config       = &set_config);

const ShotIfaceEntry kIfaces[] = {
    {SHOT_IFACE_CONFIGURABLE, 1, &kConfigurable},
    {nullptr, 0, nullptr},
};

/** @brief 创建插件状态；失败返回 nullptr（宿主会跳过这个插件）。 */
void* create_state() { return new (std::nothrow) TranslatorPlugin(); }

/** @brief 释放插件状态：析构里的 stop() 会退订并撤回贡献。 */
void destroy_state(void* self) { delete static_cast<TranslatorPlugin*>(self); }

}  // namespace

// 插件身份证：id 必须与 metadata.json 的 plugins[].id 完全一致（kEntries 也靠它索引），
// 名称与版本只是展示信息。
const PluginEntry kTranslator = {
    .id      = "translator.main",
    .name    = "Translator",
    .version = "0.1.0",
    .create  = &create_state,
    .destroy = &destroy_state,
    .ifaces  = kIfaces,
};

}  // namespace translator
