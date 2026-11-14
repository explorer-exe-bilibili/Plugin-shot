// 插件自检的宿主侧检查：加载状态、metadata 声明、配置项、列表贡献、事件试运行。
//
// 分成两个 TU（本文件 + pluginTestCapabilities.cpp）是因为它们关注点不同：
// 本文件回答"宿主眼里的这个插件对不对"，能力文件回答"它真的能干活吗"。
//
// 所有列表/配置检查都走真实宿主路径：
//   · 读 = HostListCatalog::service() 的 list_json（宿主自己的服务表）；
//   · 写 = 发布 lists.update / lists.commit 事件，再 dispatchEvents 让 HostRuntime
//     的列表协议处理器执行——和设置页点击「应用」走的是同一条链路。
// 唯一"降级"的细节：如果宿主列表服务不可用（理论上不该发生），检查记 FAIL 并给出
// 原因，而不是偷偷改成直连插件配置。

#include "pluginTestChecks.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "hostListCatalog.h"
#include "hostRuntime.h"
#include "hostServices.h"
#include "pluginManager.h"
#include "pluginTest.h"
#include "pluginTestChecks.h"

namespace Host {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr char kConfigListId[] = "config_list";               /**< 宿主生成：配置项。 */
constexpr char kTrayCommandListId[] = "tray_command_list";    /**< 插件贡献：托盘命令。 */
constexpr char kHotkeyListId[] = "hotkey_list";               /**< 插件贡献：全局热键。 */
constexpr char kI18nSourceListId[] = "i18n_source_list";      /**< 插件贡献：语言表。 */
constexpr char kI18nListId[] = "i18n_list";                   /**< 宿主生成：当前语言快照。 */
constexpr char kListsUpdateTopic[] = "lists.update";          /**< 写 config_list 值。 */
constexpr char kListsCommitTopic[] = "lists.commit";          /**< config_list 落盘。 */
constexpr char kListsUpdatedTopic[] = "lists.updated";        /**< 宿主回执。 */
constexpr char kConfigAppliedTopic[] = "config.applied";      /**< 提交成功的既有通知。 */
constexpr char kCaptureDoneTopic[] = "capture.done";          /**< 采集成功事件。 */
constexpr char kCaptureFailedTopic[] = "capture.failed";      /**< 采集失败事件。 */
constexpr char kTestTag[] = "plugin-test";                    /**< 自检事件的 tag，便于过滤。 */
constexpr char kUnknownConfigKey[] = "__plugin_test_unknown_key__"; /**< 未知 key 检查用。 */
constexpr int kPumpRounds = 4;                                /**< 启动后先跑几轮主循环。 */

/**
 * @brief 事件试运行用的真实 64x48 PNG（纯色，160 字节）。
 *
 * 为什么不用随便一个文本文件：capture.done 的订阅者（clipboard / notify）会真的去
 * 解码 path 指向的文件；放文本会得到"unknown image type"这类干扰日志，看起来像
 * 自检把插件弄坏了。用真实 PNG 才能验证"订阅者收到事件后能正常走完自己的处理"。
 */
const std::uint8_t kDummyPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x30, 0x08, 0x02, 0x00, 0x00, 0x00, 0x2E, 0x29, 0xEB,
    0x48, 0x00, 0x00, 0x00, 0x67, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0xD5, 0xCE, 0x41, 0x11, 0x00,
    0x20, 0x0C, 0xC0, 0xB0, 0x52, 0x7D, 0x28, 0xC4, 0x2C, 0x88, 0xD8, 0x83, 0x6B, 0x14, 0x64, 0xED,
    0x73, 0x29, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38,
    0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89,
    0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93,
    0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0x93, 0x38,
    0x89, 0x93, 0x38, 0x89, 0x93, 0x38, 0x89, 0xF3, 0x77, 0x60, 0xEA, 0x01, 0x7C, 0x08, 0x02, 0x45,
    0x39, 0x3C, 0xBF, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

/**
 * @brief 内部：读一个列表的 JSON 快照（宿主真实服务表，两段式读取）。
 * @param[in]  listId 列表 id。
 * @param[out] out    快照文本。
 * @return true = 列表存在且可读；false = 列表未注册或服务不可用。
 */
bool readListSnapshot(const char* listId, std::string& out) {
    const ShotHostPluginListsV1* lists = HostListCatalog::service();
    if (lists == nullptr || lists->list_json == nullptr) {
        return false;
    }
    const std::int32_t size = lists->list_json(nullptr, listId, nullptr, 0);
    if (size < 0) {
        return false;
    }
    std::vector<char> buffer(static_cast<std::size_t>(size) + 1, '\0');
    const std::int32_t written =
        lists->list_json(nullptr, listId, buffer.data(), static_cast<std::uint32_t>(buffer.size()));
    if (written < 0) {
        return false;
    }
    out.assign(buffer.data(), static_cast<std::size_t>(written));
    return true;
}

/** @brief 取 JSON 对象里的字符串字段；缺失/类型不对时返回空串。 */
std::string stringField(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

/** @brief 取 JSON 对象里的 bool 字段；缺失/类型不对时返回 false。 */
bool boolField(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

/**
 * @brief 把一个列表快照解析成 JSON 数组。
 * @param[in]  text   快照文本。
 * @param[out] array  解析结果。
 * @return true = 是合法 JSON 数组。
 */
bool parseListArray(const std::string& text, json& array) {
    array = json::parse(text, nullptr, false);
    return !array.is_discarded() && array.is_array();
}

/**
 * @brief 过滤出 owner 等于指定插件 id 的条目。
 * @param[in] array 列表快照数组。
 * @param[in] owner 插件 id。
 * @return 条目指针列表（生命周期与 array 相同）。
 */
std::vector<const json*> filterOwner(const json& array, const std::string& owner) {
    std::vector<const json*> items;
    for (const json& item : array) {
        if (item.is_object() && stringField(item, "owner") == owner) {
            items.push_back(&item);
        }
    }
    return items;
}

/** @brief 判断两个路径是否指向同一个文件（与 pluginTest.cpp 的同名辅助一致，见其注释）。 */
bool sameFile(const std::string& left, const std::string& right) {
    std::error_code ec;
    const bool equivalent = fs::equivalent(left, right, ec);
    if (!ec) {
        return equivalent;
    }
    std::error_code leftEc;
    std::error_code rightEc;
    const fs::path leftPath = fs::weakly_canonical(fs::path(left), leftEc);
    const fs::path rightPath = fs::weakly_canonical(fs::path(right), rightEc);
    if (leftEc || rightEc) {
        return false;
    }
    return leftPath == rightPath;
}

/** @brief 插件状态常量转 JSON 字符串（loaded / disabled / failed）。 */
const char* stateName(std::uint32_t state) {
    switch (state) {
        case SHOT_PLUGIN_STATE_LOADED: return "loaded";
        case SHOT_PLUGIN_STATE_DISABLED: return "disabled";
        case SHOT_PLUGIN_STATE_FAILED: return "failed";
        default: return "unknown";
    }
}

/** @brief PluginType 转 JSON 字符串（与接口 id 的后缀一致，方便对照 metadata）。 */
const char* typeName(Plugin::PluginType type) {
    switch (type) {
        case Plugin::PluginType::Lib: return "lib_provider";
        case Plugin::PluginType::UIProvider: return "ui_provider";
        case Plugin::PluginType::ImageProcessor: return "image_processor";
        case Plugin::PluginType::ScreenCapture: return "screen_capture";
        case Plugin::PluginType::ImageSaver: return "image_saver";
        case Plugin::PluginType::Generic: return "generic";
        case Plugin::PluginType::None: return "none";
    }
    return "none";
}

/** @brief 接口 id 是否是契约里已登记的能力（含 configurable）。 */
bool isKnownInterface(const std::string& iface) {
    return iface == SHOT_IFACE_IMAGE_PROCESSOR || iface == SHOT_IFACE_IMAGE_SAVER ||
           iface == SHOT_IFACE_SCREEN_CAPTURE || iface == SHOT_IFACE_LIB_PROVIDER ||
           iface == SHOT_IFACE_UI_PROVIDER || iface == SHOT_IFACE_CONFIGURABLE;
}

/** @brief 接口 id 对应的宿主包装类型；configurable / 未知接口返回 None。 */
Plugin::PluginType declaredTypeFor(const std::string& iface) {
    if (iface == SHOT_IFACE_IMAGE_PROCESSOR) {
        return Plugin::PluginType::ImageProcessor;
    }
    if (iface == SHOT_IFACE_IMAGE_SAVER) {
        return Plugin::PluginType::ImageSaver;
    }
    if (iface == SHOT_IFACE_SCREEN_CAPTURE) {
        return Plugin::PluginType::ScreenCapture;
    }
    if (iface == SHOT_IFACE_LIB_PROVIDER) {
        return Plugin::PluginType::Lib;
    }
    if (iface == SHOT_IFACE_UI_PROVIDER) {
        return Plugin::PluginType::UIProvider;
    }
    return Plugin::PluginType::None;
}

/**
 * @brief 按 PluginManager 的包装优先级，从 metadata 声明推断宿主应该给的类型。
 *
 * 顺序必须与 PluginManager::createPlugin 的 if 链一致（image_processor -> lib ->
 * ui_provider -> screen_capture -> image_saver -> Generic），否则"声明与包装不符"
 * 会误报。
 * @param[in] interfaces metadata 里声明的能力 id。
 * @return 宿主预期包装类型；没有具名能力时为 Generic。
 */
Plugin::PluginType expectedTypeFor(const std::vector<std::string>& interfaces) {
    const Plugin::PluginType priority[] = {
        Plugin::PluginType::ImageProcessor, Plugin::PluginType::Lib,
        Plugin::PluginType::UIProvider,     Plugin::PluginType::ScreenCapture,
        Plugin::PluginType::ImageSaver,
    };
    for (const Plugin::PluginType candidate : priority) {
        for (const std::string& iface : interfaces) {
            if (declaredTypeFor(iface) == candidate) {
                return candidate;
            }
        }
    }
    return Plugin::PluginType::Generic;
}

/** @brief 在 metadata 里按插件 id 找声明；找不到返回 nullptr。 */
const Plugin::PluginMetaEntry* findMetaEntry(const Plugin::PluginPackageMeta& metadata,
                                             const std::string& id) {
    for (const Plugin::PluginMetaEntry& entry : metadata.getMetaData()) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

// ---- 事件探针：自检自己订阅主题，用来观察"发布出去的事件是否真的被派发" ----

/** @brief 探针抓到的一条事件。 */
struct CapturedEvent {
    std::string topic;   /**< 主题。 */
    std::string payload; /**< 负载拷贝。 */
};

/** @brief 探针的可变状态；回调由事件总线在 main 线程调用。 */
struct EventProbe {
    std::mutex mutex;                 /**< 保护 events。 */
    std::vector<CapturedEvent> events;/**< 已抓到的事件。 */
};

/**
 * @brief 事件总线回调：把主题与负载拷进探针。
 * @param[in] user  探针指针。
 * @param[in] topic 主题。
 * @param[in] data  负载；可为空。
 * @param[in] len   负载字节数。
 */
void eventProbeCallback(void* user, const char* topic, const void* data, std::uint32_t len) {
    auto* probe = static_cast<EventProbe*>(user);
    if (probe == nullptr || topic == nullptr) {
        return;
    }
    CapturedEvent captured;
    captured.topic = topic;
    if (data != nullptr && len != 0) {
        captured.payload.assign(static_cast<const char*>(data), len);
    }
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->events.push_back(std::move(captured));
}

/** @brief 订阅一个主题模式；失败只记日志（检查结果里会体现"没抓到事件"）。 */
void subscribeProbe(EventProbe& probe, const char* pattern) {
    if (HostServices::eventBusSubscribe(nullptr, pattern, &eventProbeCallback, &probe) != 0) {
        spdlog::warn("自检探针订阅 {} 失败", pattern);
    }
}

/** @brief 退订该探针的全部主题。 */
void unsubscribeProbe(EventProbe& probe) {
    HostServices::eventBusUnsubscribe(nullptr, &probe);
}

/** @brief 取走探针已抓到的事件并清空。 */
std::vector<CapturedEvent> takeProbeEvents(EventProbe& probe) {
    std::lock_guard<std::mutex> lock(probe.mutex);
    std::vector<CapturedEvent> events = std::move(probe.events);
    probe.events.clear();
    return events;
}

/**
 * @brief 发布一个 JSON 负载的事件。
 * @param[in] topic   主题。
 * @param[in] payload 负载对象。
 * @return true = 已入队。
 */
bool publishJsonEvent(const char* topic, const json& payload) {
    const std::string text = payload.dump(-1, ' ', false, json::error_handler_t::replace);
    return HostServices::eventBusPublish(nullptr, topic, text.data(),
                                         static_cast<std::uint32_t>(text.size())) == 0;
}

/**
 * @brief 像宿主主循环那样跑几轮：dispatchEvents -> tick -> 清空遗留队列。
 *
 * 为什么要跑：插件在 create() 里发布的 lists.add 只是入队，宿主 main 循环下一轮
 * 才会处理；不跑完就看不到托盘/热键/i18n 贡献，也看不到 config_list 的最终形状。
 * @param[in,out] runtime 宿主运行期。
 * @param[in]     rounds  轮数（少量事件通常 1 轮就清空，留余量给级联）。
 */
void pumpMainLoop(HostRuntime& runtime, int rounds) {
    for (int round = 0; round < rounds; ++round) {
        HostServices::dispatchEvents();
        runtime.tick();
        while (!Host::g_eventQueue.empty()) {
            Host::g_eventQueue.pop();
        }
    }
}

/**
 * @brief 派发两轮事件：第一轮处理我们发布的事件，第二轮收它产生的回执。
 *
 * 事件总线在分发时换出队列，回调里 publish 的新事件进入下一轮，所以写 config_list
 * 至少要两轮才能拿到 lists.updated / config.applied。
 */
void dispatchTwoRounds() {
    HostServices::dispatchEvents();
    HostServices::dispatchEvents();
}

/** @brief lists.update 的逐项回执。 */
struct UpdateResult {
    bool ok = false;        /**< 该项是否成功。 */
    std::string reason;     /**< 失败原因。 */
};

/** @brief 从 lists.updated 事件里收集逐项回执（key = owner + "/" + id）。 */
std::map<std::string, UpdateResult> collectUpdateResults(
    const std::vector<CapturedEvent>& events) {
    std::map<std::string, UpdateResult> results;
    for (const CapturedEvent& event : events) {
        if (event.topic != kListsUpdatedTopic) {
            continue;
        }
        const json payload = json::parse(event.payload, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) {
            continue;
        }
        const auto resultsIt = payload.find("results");
        if (resultsIt == payload.end() || !resultsIt->is_array()) {
            continue;
        }
        for (const json& result : *resultsIt) {
            if (!result.is_object()) {
                continue;
            }
            UpdateResult entry;
            const auto okIt = result.find("ok");
            entry.ok = okIt != result.end() && okIt->is_boolean() && okIt->get<bool>();
            entry.reason = stringField(result, "reason");
            results[stringField(result, "owner") + "/" + stringField(result, "id")] = entry;
        }
    }
    return results;
}

/** @brief 从 config.applied / lists.updated(error) 事件里找提交结果。 */
struct CommitObservation {
    bool applied = false;       /**< 收到 config.applied。 */
    bool failed = false;        /**< 收到带 error 的 lists.updated。 */
    std::string errorText;      /**< 失败原因。 */
};

/** @brief 观察提交结果相关事件。 */
CommitObservation observeCommit(const std::vector<CapturedEvent>& events) {
    CommitObservation observation;
    for (const CapturedEvent& event : events) {
        if (event.topic == kConfigAppliedTopic) {
            observation.applied = true;
            continue;
        }
        if (event.topic != kListsUpdatedTopic) {
            continue;
        }
        const json payload = json::parse(event.payload, nullptr, false);
        if (!payload.is_discarded() && payload.is_object() && payload.contains("error")) {
            observation.failed = true;
            observation.errorText = stringField(payload, "error");
        }
    }
    return observation;
}

/**
 * @brief 读 config.json，取某个配置键落盘的值。
 * @param[in]  configPath config.json 路径。
 * @param[in]  key        存盘键（owner/id）。
 * @param[out] value      落盘值。
 * @return true = 找到了该键。
 */
bool readStoredConfigValue(const fs::path& configPath, const std::string& key,
                           std::string& value) {
    std::ifstream stream(configPath, std::ios::binary);
    if (!stream) {
        return false;
    }
    const json document = json::parse(stream, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return false;
    }
    const auto itemsIt = document.find("configItems");
    if (itemsIt == document.end() || !itemsIt->is_array()) {
        return false;
    }
    for (const json& item : *itemsIt) {
        if (!item.is_object()) {
            continue;
        }
        if (stringField(item, "name") == key) {
            value = stringField(item, "value");
            return true;
        }
    }
    return false;
}

// ---- 配置检查 ----------------------------------------------------------------

/** @brief 一个待回写的配置项。 */
struct PendingWriteback {
    std::string key;    /**< 配置键（owner 内唯一）。 */
    std::string target; /**< 要写入再回读的值。 */
};

/**
 * @brief 检查一个插件的配置项：字段完整性 + 默认值回写回读 + 未知 key + 落盘。
 *
 * 走真实宿主路径：`lists.update` → HostRuntime 处理 → 插件 set_config → 重建
 * config_list → 我们回读快照比对；`lists.commit` 触发宿主把 config.json 落盘，
 * 最后直接读隔离目录里的 config.json 验证真的写进去了。
 *
 * @param[in]     instance 已加载插件实例。
 * @param[in]     owner    插件 id（config_list 的 owner）。
 * @param[in]     dataDir  隔离数据目录（config.json 在这里）。
 * @param[in,out] checks   追加检查结果。
 */
void checkConfiguration(const std::shared_ptr<Plugin::PluginBase>& instance,
                        const std::string& owner, const fs::path& dataDir,
                        std::vector<PluginTestCheck>& checks) {
    const void* configOps = nullptr;
    bool queryThrew = false;
    try {
        configOps = instance->queryInterface(SHOT_IFACE_CONFIGURABLE, 1);
    } catch (...) {
        queryThrew = true;
    }
    if (queryThrew) {
        addPluginTestCheck(checks, "config.query", PluginTestStatus::Fail,
                           "query_interface(shot.configurable) 抛出 C++ 异常（契约要求异常不得跨 "
                           "DLL 边界）");
        return;
    }
    if (configOps == nullptr) {
        addPluginTestCheck(checks, "config", PluginTestStatus::Skip,
                           "没有实现 shot.configurable，跳过配置检查");
        return;
    }

    std::string snapshotText;
    if (!readListSnapshot(kConfigListId, snapshotText)) {
        addPluginTestCheck(checks, "config.list", PluginTestStatus::Fail,
                           "读取 config_list 快照失败（宿主列表服务不可用）");
        return;
    }
    json configList;
    if (!parseListArray(snapshotText, configList)) {
        addPluginTestCheck(checks, "config.list", PluginTestStatus::Fail,
                           "config_list 不是合法 JSON 数组");
        return;
    }
    const std::vector<const json*> entries = filterOwner(configList, owner);
    if (entries.empty()) {
        addPluginTestCheck(checks, "config.list", PluginTestStatus::Skip,
                           "config_list 中没有该插件的条目（get_config_count 可能为 0）");
        return;
    }
    addPluginTestCheck(checks, "config.list", PluginTestStatus::Pass,
                       "config_list 中有 " + std::to_string(entries.size()) + " 项配置");

    std::vector<std::string> fieldProblems;
    std::vector<std::string> defaultWarnings;
    std::vector<PendingWriteback> writebacks;
    for (const json* item : entries) {
        const std::string key = stringField(*item, "id");
        if (key.empty()) {
            fieldProblems.push_back("有条目缺少 id（宿主本应跳过它）");
            continue;
        }
        std::string itemProblem;
        if (stringField(*item, "label").empty()) {
            itemProblem += "label 为空；";
        }
        const auto kindIt = item->find("kind");
        const bool kindOk = kindIt != item->end() && kindIt->is_number_integer() &&
                            kindIt->get<long long>() >= SHOT_CONFIG_KIND_BOOL &&
                            kindIt->get<long long>() <= SHOT_CONFIG_KIND_PATH;
        if (!kindOk) {
            itemProblem += "kind 缺失或不在 1..7；";
        }
        const auto defaultIt = item->find("default");
        std::string defaultValue;
        if (defaultIt == item->end() || !defaultIt->is_string()) {
            itemProblem += "default 字段缺失（宿主应至少写空串）；";
        } else {
            defaultValue = defaultIt->get<std::string>();
        }
        if (!itemProblem.empty()) {
            fieldProblems.push_back(key + ": " + itemProblem);
        }
        if (kindOk && defaultValue.empty()) {
            const std::string currentValue = stringField(*item, "value");
            if (!currentValue.empty()) {
                defaultWarnings.push_back(key + "：default 为空（契约允许无默认值），回写用当前值");
            }
        }
        // 回写目标：优先默认值；没有默认值就用当前值（等价于"写回原值"）；都空则跳过。
        std::string target = !defaultValue.empty() ? defaultValue : stringField(*item, "value");
        if (target.empty()) {
            addPluginTestCheck(checks, "config.writeback." + key, PluginTestStatus::Skip,
                               "默认值与当前值都为空，跳过回写");
            continue;
        }
        writebacks.push_back(PendingWriteback{key, std::move(target)});
    }
    if (fieldProblems.empty()) {
        addPluginTestCheck(checks, "config.fields", PluginTestStatus::Pass,
                           "每项都有非空 label、合法 kind 与 default 字段");
    } else {
        addPluginTestCheck(checks, "config.fields", PluginTestStatus::Fail,
                           "配置项快照字段不完整：" + [&fieldProblems] {
                               std::string text;
                               for (const std::string& problem : fieldProblems) {
                                   if (!text.empty()) {
                                       text += "；";
                                   }
                                   text += problem;
                               }
                               return text;
                           }());
    }
    if (!defaultWarnings.empty()) {
        std::string text;
        for (const std::string& warning : defaultWarnings) {
            if (!text.empty()) {
                text += "；";
            }
            text += warning;
        }
        addPluginTestCheck(checks, "config.default", PluginTestStatus::Warn, text);
    }

    // ---- 默认值回写回读 ----
    if (writebacks.empty()) {
        addPluginTestCheck(checks, "config.writeback", PluginTestStatus::Skip,
                           "没有可回写的配置项");
    } else {
        EventProbe probe;
        subscribeProbe(probe, kListsUpdatedTopic);
        json payload = json{{"list", kConfigListId}, {"tag", "plugin-test-write"},
                            {"changes", json::array()}};
        for (const PendingWriteback& writeback : writebacks) {
            payload["changes"].push_back(json{
                {"owner", owner}, {"id", writeback.key}, {"value", writeback.target}});
        }
        const bool published = publishJsonEvent(kListsUpdateTopic, payload);
        dispatchTwoRounds();
        const std::vector<CapturedEvent> events = takeProbeEvents(probe);
        unsubscribeProbe(probe);
        const std::map<std::string, UpdateResult> results = collectUpdateResults(events);
        if (!published) {
            addPluginTestCheck(checks, "config.writeback.publish", PluginTestStatus::Fail,
                               "发布 lists.update 事件失败（事件队列满？）");
        }

        // 回读快照：宿主处理完 lists.update 后会同步重建 config_list，所以这里直接读即可。
        json afterList;
        std::vector<const json*> afterEntries;
        bool afterReadOk = false;
        std::string afterText;
        if (readListSnapshot(kConfigListId, afterText) && parseListArray(afterText, afterList)) {
            afterEntries = filterOwner(afterList, owner);
            afterReadOk = true;
        }
        for (const PendingWriteback& writeback : writebacks) {
            const std::string checkId = "config.writeback." + writeback.key;
            if (!afterReadOk) {
                addPluginTestCheck(checks, checkId, PluginTestStatus::Fail,
                                   "回写后重新读取 config_list 失败");
                continue;
            }
            const json* matchedEntry = nullptr;
            for (const json* item : afterEntries) {
                if (stringField(*item, "id") == writeback.key) {
                    matchedEntry = item;
                    break;
                }
            }
            const std::string actual = matchedEntry != nullptr
                                           ? stringField(*matchedEntry, "value")
                                           : std::string();
            const auto resultIt = results.find(owner + "/" + writeback.key);
            std::string reasonText;
            if (resultIt != results.end() && !resultIt->second.ok) {
                reasonText = "；宿主回执失败：" +
                             (resultIt->second.reason.empty() ? std::string("(无原因)")
                                                              : resultIt->second.reason);
            }
            if (matchedEntry == nullptr) {
                addPluginTestCheck(checks, checkId, PluginTestStatus::Fail,
                                   "回写后 config_list 里找不到该配置项");
            } else if (actual == writeback.target) {
                addPluginTestCheck(checks, checkId, PluginTestStatus::Pass,
                                   "写入默认值 \"" + writeback.target + "\" 后回读一致");
            } else {
                addPluginTestCheck(checks, checkId, PluginTestStatus::Fail,
                                   "写入默认值 \"" + writeback.target + "\" 后回读为 \"" + actual +
                                       "\"" + reasonText);
            }
        }

        // ---- 未知 key：契约要求 set_config 返回负值（宿主回执 ok=false）----
        EventProbe unknownProbe;
        subscribeProbe(unknownProbe, kListsUpdatedTopic);
        json unknownPayload = json{{"list", kConfigListId}, {"tag", "plugin-test-unknown"},
                                   {"changes", json::array()}};
        unknownPayload["changes"].push_back(
            json{{"owner", owner}, {"id", kUnknownConfigKey}, {"value", "x"}});
        publishJsonEvent(kListsUpdateTopic, unknownPayload);
        dispatchTwoRounds();
        const std::map<std::string, UpdateResult> unknownResults =
            collectUpdateResults(takeProbeEvents(unknownProbe));
        unsubscribeProbe(unknownProbe);
        const auto unknownIt = unknownResults.find(owner + "/" + kUnknownConfigKey);
        if (unknownIt == unknownResults.end()) {
            addPluginTestCheck(checks, "config.unknown_key", PluginTestStatus::Warn,
                               "没有收到未知 key 的 lists.updated 回执（宿主列表链路异常？）");
        } else if (!unknownIt->second.ok) {
            addPluginTestCheck(checks, "config.unknown_key", PluginTestStatus::Pass,
                               "对未知 key 返回负值（宿主回执 ok=false），降级行为符合契约");
        } else {
            addPluginTestCheck(checks, "config.unknown_key", PluginTestStatus::Warn,
                               "未知 key 被插件接受（契约要求返回负值，老宿主多传配置项时可能"
                               "误改插件状态）");
        }
    }

    // ---- 落盘（lists.commit）----
    EventProbe commitProbe;
    subscribeProbe(commitProbe, kListsUpdatedTopic);
    subscribeProbe(commitProbe, kConfigAppliedTopic);
    const bool commitPublished = publishJsonEvent(
        kListsCommitTopic, json{{"list", kConfigListId}, {"tag", kTestTag}});
    dispatchTwoRounds();
    const std::vector<CapturedEvent> commitEvents = takeProbeEvents(commitProbe);
    unsubscribeProbe(commitProbe);
    const CommitObservation observation = observeCommit(commitEvents);
    const fs::path configPath = dataDir / "config.json";
    std::error_code sizeEc;
    const std::uintmax_t configSize = fs::file_size(configPath, sizeEc);
    std::vector<std::string> missingKeys;
    for (const PendingWriteback& writeback : writebacks) {
        std::string stored;
        if (!readStoredConfigValue(configPath, owner + "/" + writeback.key, stored) ||
            stored != writeback.target) {
            missingKeys.push_back(writeback.key);
        }
    }
    const bool commitOk = !commitPublished ? false
                          : observation.failed ? false
                          : !sizeEc && configSize > 0 && missingKeys.empty();
    if (commitOk) {
        addPluginTestCheck(checks, "config.commit", PluginTestStatus::Pass,
                           std::string("config.json 已落盘（") + std::to_string(configSize) +
                               " 字节" +
                               (observation.applied ? "，收到 config.applied" : "") + "）" +
                               (writebacks.empty() ? "" : "，回写项均已写入"));
        return;
    }
    if (!commitPublished) {
        addPluginTestCheck(checks, "config.commit", PluginTestStatus::Fail,
                           "发布 lists.commit 事件失败（事件队列满？）");
    } else if (observation.failed) {
        addPluginTestCheck(checks, "config.commit", PluginTestStatus::Fail,
                           "lists.commit 失败：" + observation.errorText);
    } else if (sizeEc || configSize == 0) {
        addPluginTestCheck(checks, "config.commit", PluginTestStatus::Fail,
                           "config.json 没有落盘或为空：" + configPath.string());
    } else {
        std::string text;
        for (const std::string& missing : missingKeys) {
            if (!text.empty()) {
                text += ", ";
            }
            text += missing;
        }
        addPluginTestCheck(checks, "config.commit", PluginTestStatus::Fail,
                           "config.json 里没有这些回写项的最终值：" + text);
    }
}

// ---- 列表贡献检查 ------------------------------------------------------------

/**
 * @brief 检查一个插件在 tray_command_list 的贡献（缺字段只 WARN，不 FAIL）。
 * @param[in]     owner  插件 id。
 * @param[in,out] checks 追加检查结果。
 */
void checkTrayContributions(const std::string& owner, std::vector<PluginTestCheck>& checks) {
    std::string text;
    if (!readListSnapshot(kTrayCommandListId, text)) {
        addPluginTestCheck(checks, "list.tray_command_list", PluginTestStatus::Skip,
                           "tray_command_list 未注册（当前进程没有托盘/命令贡献者）");
        return;
    }
    json items;
    if (!parseListArray(text, items)) {
        addPluginTestCheck(checks, "list.tray_command_list", PluginTestStatus::Warn,
                           "tray_command_list 不是合法 JSON 数组");
        return;
    }
    const std::vector<const json*> owned = filterOwner(items, owner);
    if (owned.empty()) {
        addPluginTestCheck(checks, "list.tray_command_list", PluginTestStatus::Skip,
                           "该插件没有贡献托盘命令条目");
        return;
    }
    std::vector<std::string> problems;
    for (const json* item : owned) {
        const std::string id = stringField(*item, "id");
        const std::string label = stringField(*item, "label");
        const std::string event = stringField(*item, "event");
        if (id.empty()) {
            problems.push_back("有条目缺少 id");
        }
        if (boolField(*item, "separator")) {
            continue;  // 分隔线条目没有 label/event 是正常的。
        }
        if (label.empty()) {
            problems.push_back("条目 " + (id.empty() ? std::string("?") : id) + " 缺少 label");
        }
        if (event.empty()) {
            problems.push_back("条目 " + (id.empty() ? std::string("?") : id) + " 缺少 event");
        }
    }
    if (problems.empty()) {
        addPluginTestCheck(checks, "list.tray_command_list", PluginTestStatus::Pass,
                           "贡献 " + std::to_string(owned.size()) + " 条，字段齐全");
        return;
    }
    std::string detail;
    for (const std::string& problem : problems) {
        if (!detail.empty()) {
            detail += "；";
        }
        detail += problem;
    }
    addPluginTestCheck(checks, "list.tray_command_list", PluginTestStatus::Warn, detail);
}

/**
 * @brief 检查一个插件在 hotkey_list 的贡献（缺字段只 WARN，不 FAIL）。
 * @param[in]     owner  插件 id。
 * @param[in,out] checks 追加检查结果。
 */
void checkHotkeyContributions(const std::string& owner, std::vector<PluginTestCheck>& checks) {
    std::string text;
    if (!readListSnapshot(kHotkeyListId, text)) {
        addPluginTestCheck(checks, "list.hotkey_list", PluginTestStatus::Skip,
                           "hotkey_list 未注册（当前进程没有热键 provider/贡献者）");
        return;
    }
    json items;
    if (!parseListArray(text, items)) {
        addPluginTestCheck(checks, "list.hotkey_list", PluginTestStatus::Warn,
                           "hotkey_list 不是合法 JSON 数组");
        return;
    }
    const std::vector<const json*> owned = filterOwner(items, owner);
    if (owned.empty()) {
        addPluginTestCheck(checks, "list.hotkey_list", PluginTestStatus::Skip,
                           "该插件没有贡献热键条目");
        return;
    }
    std::vector<std::string> problems;
    for (const json* item : owned) {
        const std::string id = stringField(*item, "id");
        const std::string combo = stringField(*item, "combo");
        if (id.empty()) {
            problems.push_back("有条目缺少 id");
        }
        if (combo.empty()) {
            problems.push_back("条目 " + (id.empty() ? std::string("?") : id) + " 缺少 combo");
        }
        if (stringField(*item, "event").empty()) {
            problems.push_back("条目 " + (id.empty() ? std::string("?") : id) + " 缺少 event");
        }
    }
    if (problems.empty()) {
        addPluginTestCheck(checks, "list.hotkey_list", PluginTestStatus::Pass,
                           "贡献 " + std::to_string(owned.size()) + " 条，字段齐全");
        return;
    }
    std::string detail;
    for (const std::string& problem : problems) {
        if (!detail.empty()) {
            detail += "；";
        }
        detail += problem;
    }
    addPluginTestCheck(checks, "list.hotkey_list", PluginTestStatus::Warn, detail);
}

/**
 * @brief 检查一个插件在 i18n_source_list 的贡献（缺字段只 WARN，不 FAIL）。
 * @param[in]     owner  插件 id。
 * @param[in,out] checks 追加检查结果。
 */
void checkI18nContributions(const std::string& owner, std::vector<PluginTestCheck>& checks) {
    std::string text;
    if (!readListSnapshot(kI18nSourceListId, text)) {
        addPluginTestCheck(checks, "list.i18n_source_list", PluginTestStatus::Skip,
                           "i18n_source_list 未注册（当前进程没有语言表贡献者）");
        return;
    }
    json items;
    if (!parseListArray(text, items)) {
        addPluginTestCheck(checks, "list.i18n_source_list", PluginTestStatus::Warn,
                           "i18n_source_list 不是合法 JSON 数组");
        return;
    }
    const std::vector<const json*> owned = filterOwner(items, owner);
    if (owned.empty()) {
        addPluginTestCheck(checks, "list.i18n_source_list", PluginTestStatus::Skip,
                           "该插件没有贡献语言表条目");
        return;
    }
    std::vector<std::string> problems;
    for (const json* item : owned) {
        const std::string id = stringField(*item, "id");
        if (id.empty()) {
            problems.push_back("有条目缺少 id");
            continue;
        }
        const auto textsIt = item->find("texts");
        const bool textsOk = textsIt != item->end() &&
                             (textsIt->is_object() || textsIt->is_string());
        if (!textsOk && id.rfind("@language/", 0) != 0) {
            problems.push_back("条目 " + id + " 缺少 texts（应为 语言标签→文本 的对象）");
        }
    }
    if (problems.empty()) {
        addPluginTestCheck(checks, "list.i18n_source_list", PluginTestStatus::Pass,
                           "贡献 " + std::to_string(owned.size()) + " 条，字段齐全");
        return;
    }
    std::string detail;
    for (const std::string& problem : problems) {
        if (!detail.empty()) {
            detail += "；";
        }
        detail += problem;
    }
    addPluginTestCheck(checks, "list.i18n_source_list", PluginTestStatus::Warn, detail);
}

// ---- 全局检查：i18n 快照 / 事件试运行 ----------------------------------------

/**
 * @brief 检查 i18n_list 快照：@meta 存在、language 与 --language 一致。
 * @param[in]     requestedLanguage 请求的语言标签。
 * @param[in,out] checks            追加检查结果。
 */
void checkI18nSnapshot(const std::string& requestedLanguage,
                       std::vector<PluginTestCheck>& checks) {
    std::string text;
    if (!readListSnapshot(kI18nListId, text)) {
        addPluginTestCheck(checks, "i18n.snapshot", PluginTestStatus::Fail,
                           "读取 i18n_list 快照失败（宿主列表服务不可用）");
        return;
    }
    json items;
    if (!parseListArray(text, items)) {
        addPluginTestCheck(checks, "i18n.snapshot", PluginTestStatus::Fail,
                           "i18n_list 不是合法 JSON 数组");
        return;
    }
    const json* meta = nullptr;
    for (const json& item : items) {
        if (item.is_object() && stringField(item, "id") == "@meta") {
            meta = &item;
            break;
        }
    }
    if (meta == nullptr) {
        addPluginTestCheck(checks, "i18n.snapshot", PluginTestStatus::Fail,
                           "i18n_list 里没有 @meta 条目");
        return;
    }
    const std::string language = stringField(*meta, "language");
    const std::string fallback = stringField(*meta, "fallback");
    addPluginTestCheck(checks, "i18n.snapshot", PluginTestStatus::Pass,
                       "@meta: language=" + language + "，fallback=" + fallback + "，条目 " +
                           std::to_string(items.size()) + " 条");
    if (language == requestedLanguage) {
        addPluginTestCheck(checks, "i18n.language", PluginTestStatus::Pass,
                           "--language " + requestedLanguage + " 已生效");
    } else {
        addPluginTestCheck(checks, "i18n.language", PluginTestStatus::Warn,
                           "--language " + requestedLanguage + " 不在语言目录时按回退链生效为 " +
                               language + "（固定语言快照请用内置/语言包标签）");
    }
}

/**
 * @brief 事件试运行：发布 capture.done / capture.failed，派发两轮，观察不崩不挂。
 *
 * 负载按 document/CAPTURE.md：capture.done 只需 path/width/height/processors，
 * 这里用一个真实存在的小文件充数（订阅者可能会去读它）；capture.failed 用合法的
 * stage/reason。自检探针同时订阅 capture.* 计数，顺便证明"事件真的派发到了订阅者"。
 * @param[in]     dataDir 隔离数据目录（放假文件）。
 * @param[in,out] checks  追加检查结果。
 */
void checkEventDispatch(const fs::path& dataDir, std::vector<PluginTestCheck>& checks) {
    const fs::path eventDir = dataDir / "events";
    const fs::path dummyFile = eventDir / "plugin_test_capture.png";
    std::error_code ec;
    fs::create_directories(eventDir, ec);
    if (ec) {
        addPluginTestCheck(checks, "events.fixture", PluginTestStatus::Fail,
                           "无法创建事件试运行目录 " + eventDir.string() + "：" + ec.message());
        return;
    }
    {
        std::ofstream stream(dummyFile, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(kDummyPng),
                     static_cast<std::streamsize>(sizeof(kDummyPng)));
        stream.flush();
        if (!stream) {
            addPluginTestCheck(checks, "events.fixture", PluginTestStatus::Fail,
                               "无法写入事件试运行的假图片 " + dummyFile.string());
            return;
        }
    }
    EventProbe probe;
    subscribeProbe(probe, "capture.*");
    const json donePayload = json{
        {"path", dummyFile.string()},
        {"width", 64},
        {"height", 48},
        {"processors", 0},
    };
    const json failedPayload = json{
        {"stage", "capture"},
        {"reason", "plugin-shot-test dry run"},
    };
    const bool donePublished = publishJsonEvent(kCaptureDoneTopic, donePayload);
    const bool failedPublished = publishJsonEvent(kCaptureFailedTopic, failedPayload);
    const auto begin = std::chrono::steady_clock::now();
    dispatchTwoRounds();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    const std::vector<CapturedEvent> events = takeProbeEvents(probe);
    unsubscribeProbe(probe);
    std::size_t doneCount = 0;
    std::size_t failedCount = 0;
    for (const CapturedEvent& event : events) {
        if (event.topic == kCaptureDoneTopic) {
            ++doneCount;
        } else if (event.topic == kCaptureFailedTopic) {
            ++failedCount;
        }
    }
    const bool published = donePublished && failedPublished;
    const bool delivered = doneCount > 0 && failedCount > 0;
    addPluginTestCheck(checks, "events.dispatch", delivered ? PluginTestStatus::Pass
                                                            : PluginTestStatus::Warn,
                       std::string("capture.done/capture.failed ") +
                           (published ? "已发布" : "发布失败") + "；自检订阅者收到 done=" +
                           std::to_string(doneCount) + " failed=" + std::to_string(failedCount) +
                           (delivered ? "" : "（没有订阅者时收不到属正常，但探针自己就是订阅者）"));
    addPluginTestCheck(checks, "events.stability", PluginTestStatus::Pass,
                       "两轮 dispatchEvents 完成，未崩溃/未挂起（" + std::to_string(elapsed) +
                           " ms）");
}

}  // namespace

void runPluginChecks(const PluginTestOptions& options, HostRuntime& runtime,
                     const std::string& copiedDllPath,
                     const Plugin::PluginPackageMeta& metadata, bool metadataAvailable,
                     PluginTestReport& report) {
    const fs::path dataDir(report.dataDir);

    // 先跑几轮主循环：插件 create 阶段发布的 lists.add / 配置项还没被宿主处理。
    pumpMainLoop(runtime, kPumpRounds);

    checkI18nSnapshot(options.language, report.checks);

    // 找"被测 DLL"的已安装记录：一个包可以有多条（一个 DLL 多个插件）。
    const std::vector<Plugin::InstalledPlugin> installed = runtime.manager().getInstalledPlugins();
    std::vector<Plugin::InstalledPlugin> matched;
    for (const Plugin::InstalledPlugin& record : installed) {
        if (sameFile(record.dllPath, copiedDllPath)) {
            matched.push_back(record);
        }
    }
    if (matched.empty()) {
        addPluginTestCheck(report.checks, "records", PluginTestStatus::Fail,
                           "已安装插件记录里没有 dllPath == 被测 DLL 的条目（DLL 不是宿主能识别"
                           "的插件包；已安装记录共 " +
                               std::to_string(installed.size()) + " 条）");
        checkEventDispatch(dataDir, report.checks);
        return;
    }
    addPluginTestCheck(report.checks, "records", PluginTestStatus::Pass,
                       "被测 DLL 对应 " + std::to_string(matched.size()) +
                           " 条插件记录（共 " + std::to_string(installed.size()) + " 条已安装）");

    const std::vector<std::shared_ptr<Plugin::PluginBase>> instances =
        runtime.manager().getAllInstances();

    for (const Plugin::InstalledPlugin& record : matched) {
        PluginTestPluginReport plugin;
        plugin.id = record.id;
        plugin.name = record.name;
        plugin.version = record.version;
        plugin.state = stateName(record.state);
        if (!record.reason.empty()) {
            plugin.stateReason = record.reason.chinese();
        }

        std::shared_ptr<Plugin::PluginBase> instance;
        for (const std::shared_ptr<Plugin::PluginBase>& candidate : instances) {
            if (candidate != nullptr && candidate->getId() == record.id) {
                instance = candidate;
                break;
            }
        }
        plugin.type = instance != nullptr ? typeName(instance->getType()) : "none";

        // ---- 1. 加载状态 ----
        if (record.state != SHOT_PLUGIN_STATE_LOADED) {
            addPluginTestCheck(plugin.checks, "state", PluginTestStatus::Fail,
                               "state=" + plugin.state + "，reason=" +
                                   (plugin.stateReason.empty() ? "(无原因)" : plugin.stateReason));
        } else if (instance == nullptr) {
            addPluginTestCheck(plugin.checks, "state", PluginTestStatus::Fail,
                               "记录是 LOADED，但管理器里找不到对应实例");
        } else {
            addPluginTestCheck(plugin.checks, "state", PluginTestStatus::Pass,
                               "LOADED，实例类型 " + plugin.type);
        }

        // ---- 2. metadata 声明 vs 运行期事实 ----
        if (metadataAvailable) {
            const Plugin::PluginMetaEntry* declared = findMetaEntry(metadata, record.id);
            if (declared == nullptr) {
                addPluginTestCheck(plugin.checks, "metadata.entry", PluginTestStatus::Warn,
                                   "metadata 里没有 id=" + record.id +
                                       " 的声明（get_info 与 metadata 不一致？宿主会以 get_info "
                                       "为准）");
            } else {
                plugin.declaredInterfaces = declared->interfaces;
                for (const std::string& iface : declared->interfaces) {
                    if (!isKnownInterface(iface)) {
                        addPluginTestCheck(plugin.checks, "metadata.interface." + iface,
                                           PluginTestStatus::Warn,
                                           "metadata 声明了契约里不存在的接口 id：" + iface);
                    }
                }
                if (instance != nullptr) {
                    const Plugin::PluginType expected = expectedTypeFor(declared->interfaces);
                    const Plugin::PluginType actual = instance->getType();
                    if (expected == actual) {
                        addPluginTestCheck(plugin.checks, "metadata.type", PluginTestStatus::Pass,
                                           "metadata 声明与宿主包装类型一致（" + plugin.type + "）");
                    } else {
                        addPluginTestCheck(
                            plugin.checks, "metadata.type", PluginTestStatus::Fail,
                            "metadata 声明应包装成 " + std::string(typeName(expected)) +
                                "，宿主实际包装成 " + plugin.type +
                                "（运行时能力探测优先于声明，声明写错会让依赖/预筛失效）");
                    }
                    const bool declaresConfigurable =
                        std::find(declared->interfaces.begin(), declared->interfaces.end(),
                                  SHOT_IFACE_CONFIGURABLE) != declared->interfaces.end();
                    const void* configOps = nullptr;
                    try {
                        configOps = instance->queryInterface(SHOT_IFACE_CONFIGURABLE, 1);
                    } catch (...) {
                        addPluginTestCheck(plugin.checks, "metadata.configurable",
                                           PluginTestStatus::Fail,
                                           "query_interface(shot.configurable) 抛出 C++ 异常");
                    }
                    if (declaresConfigurable && configOps == nullptr) {
                        addPluginTestCheck(plugin.checks, "metadata.configurable",
                                           PluginTestStatus::Fail,
                                           "metadata 声明了 shot.configurable，但实例查询不到该能力");
                    } else if (!declaresConfigurable && configOps != nullptr) {
                        addPluginTestCheck(plugin.checks, "metadata.configurable",
                                           PluginTestStatus::Warn,
                                           "实例实现了 shot.configurable，但 metadata 未声明（配置"
                                           "仍可用，但依赖预筛看不到）");
                    }
                }
            }
        }

        // ---- 3. 能力检查（真实调用）----
        if (instance != nullptr) {
            runCapabilityChecks(instance, report.dataDir, plugin.checks);
        } else {
            addPluginTestCheck(plugin.checks, "capability", PluginTestStatus::Skip,
                               "插件未加载（没有实例），跳过能力检查");
        }

        // ---- 4. 配置项 ----
        if (instance != nullptr) {
            checkConfiguration(instance, record.id, dataDir, plugin.checks);
        } else {
            addPluginTestCheck(plugin.checks, "config", PluginTestStatus::Skip,
                               "插件未加载（没有实例），跳过配置检查");
        }

        // ---- 5. 列表贡献 ----
        if (instance != nullptr) {
            checkTrayContributions(record.id, plugin.checks);
            checkHotkeyContributions(record.id, plugin.checks);
            checkI18nContributions(record.id, plugin.checks);
        } else {
            addPluginTestCheck(plugin.checks, "list", PluginTestStatus::Skip,
                               "插件未加载（没有实例），跳过列表贡献检查");
        }

        report.plugins.push_back(std::move(plugin));
    }

    // ---- 6. 事件试运行（全局一次：它不是"某个插件"的能力）----
    checkEventDispatch(dataDir, report.checks);
}

}  // namespace Host
