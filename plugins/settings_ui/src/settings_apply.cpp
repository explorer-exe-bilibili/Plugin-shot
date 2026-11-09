// settings_apply —— 设置页的"提交"：应用 / 取消 / 恢复默认，以及宿主事件
// （lists.changed / lists.updated / config.applied / plugins.changed）与等待超时。
//
// ABI 2.0 起配置写入是**事件驱动**的，不再有同步 set_value / save：
//   1. 收集脏配置项，发布 `lists.update`
//      {"list":"config_list","tag":"<自增序号>","changes":[{owner,id,value}]}；
//   2. 等同 tag 的 `lists.updated`，逐项看 results：ok=false 的项标红并保持脏标记；
//   3. 再发布 `lists.commit`（同 tag），等宿主的 `config.applied`（保存失败时宿主
//      回落成同 tag 的 `lists.updated` + error）；
//   4. 配置阶段结束后再登记插件启停（shot.host.plugin_registry 不变），等 `plugins.changed`。
// 每一步都有 5 秒超时兜底，别让操作条永远卡在"应用中…"。
//
// 渲染在 settings_form.cpp / settings_plugin_table.cpp；这里不画任何控件。

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "settings_panel_impl.h"
#include "shot_plugin_helpers.h"

namespace settings_ui {
namespace {

constexpr char kTopicListsUpdate[] = "lists.update";
constexpr char kTopicListsCommit[] = "lists.commit";

// 事件驱动的写值与提交是异步的：等 lists.updated / config.applied 的兜底时间。
// 与旧的启停等待一样取 5 秒——宿主 main 线程正常一轮就处理完，5 秒已经是"明显卡住"。
constexpr std::chrono::seconds kListsResponseTimeout(5);

// 启停是异步的：宿主处理完会发 plugins.changed。等不到时的兜底，别让界面永远"应用中…"。
constexpr std::chrono::seconds kPluginChangeTimeout(5);

/**
 * @brief 宽容地读一个布尔字段（兼容 true/false、"1"/"0"、数字 0/1）。
 *
 * 宿主协议规定 ok 是 JSON 布尔；但这是跨版本边界，读断言失败会让整批结果丢失，
 * 不如把常见的等价写法都认下来。只服务本文件的 onListsUpdated，留在匿名命名空间。
 */
bool jsonBool(const nlohmann::json& object, const char* field, bool fallback) {
    const auto it = object.find(field);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_boolean()) {
        return it->get<bool>();
    }
    if (it->is_number_integer()) {
        return it->get<long long>() != 0;
    }
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        return text == "1" || text == "true" || text == "TRUE";
    }
    return fallback;
}

/** @brief 解析一段 JSON 文本；失败或不是对象时返回 discarded 值，由调用方安静忽略。 */
nlohmann::json parseObject(std::string_view payload) {
    if (payload.empty()) {
        return nlohmann::json();
    }
    return nlohmann::json::parse(payload.begin(), payload.end(), nullptr,
                                 /*allow_exceptions=*/false);
}

}  // namespace

// ---------------------------------------------------------------------------
// 一次提交的状态机
// ---------------------------------------------------------------------------
void SettingsPanel::Impl::apply() {
    if (isBusy()) {
        return;  // 按钮在等待期间置灰；这里再兜一层，避免重复发布同一批改动
    }
    if (host == nullptr) {
        setStatus(Tone::Error, "宿主服务不可用，无法应用");
        return;
    }
    const bool hasConfig = hasPendingConfigChanges();
    const bool hasPlugins = hasPendingPluginChanges();
    if (!hasConfig && !hasPlugins) {
        setStatus(Tone::Normal, "没有需要应用的修改");
        return;
    }

    // 清掉上一次提交的残留（计数器在本次提交里重新累计，用于最后写操作条）。
    resetApplyState();
    applyConfigTotal = 0;
    applyConfigApplied = 0;
    applyConfigRejected = 0;
    applySaveFailed = false;
    applyPluginChanged = 0;
    applyPluginRejected = 0;

    if (hasConfig) {
        beginApplyConfig();
    } else {
        beginApplyPlugins();  // 只有启停变更：配置服务不可用（老宿主）时也能用
    }
}

void SettingsPanel::Impl::beginApplyConfig() {
    pendingChanges.clear();
    for (ProviderSection& section : sections) {
        if (section.removed) {
            continue;
        }
        for (ConfigItem& item : section.items) {
            if (item.value == item.baseline) {
                // 值已经回到基线：上一次的红色标记是陈旧的，顺手清掉（这次没有要提交的）。
                item.error.clear();
                continue;
            }
            // 重新提交：先清掉上一次的拒绝原因，结果回来再决定要不要重新标红。
            item.error.clear();
            pendingChanges.push_back(PendingConfigChange{section.id, item.desc.key, item.value});
        }
    }
    if (pendingChanges.empty()) {
        beginApplyPlugins();
        return;
    }

    pendingTag = std::to_string(++nextTag);
    nlohmann::json changes = nlohmann::json::array();
    for (const PendingConfigChange& change : pendingChanges) {
        changes.push_back({{"owner", change.owner}, {"id", change.key}, {"value", change.value}});
    }
    const nlohmann::json payload = {
        {"list", "config_list"}, {"tag", pendingTag}, {"changes", std::move(changes)}};
    if (!publishEvent(host, kTopicListsUpdate, payload.dump())) {
        // 事件没发出去：保持脏标记，用户可以重试；不要假装修过。
        setStatus(Tone::Error, "发布 lists.update 失败：宿主事件总线不可用或队列已满");
        return;
    }
    applyConfigTotal = static_cast<int>(pendingChanges.size());
    applyPhase = ApplyPhase::WaitingUpdate;
    waitDeadline = std::chrono::steady_clock::now() + kListsResponseTimeout;
}

void SettingsPanel::Impl::beginApplyCommit() {
    if (applyPhase != ApplyPhase::WaitingUpdate) {
        return;  // 只有 lists.updated 处理完才进入提交阶段
    }
    pendingChanges.clear();  // 基线已经回填完，不再需要提交快照
    const nlohmann::json payload = {{"list", "config_list"}, {"tag", pendingTag}};
    if (!publishEvent(host, kTopicListsCommit, payload.dump())) {
        applySaveFailed = true;
        setStatus(Tone::Error, "发布 lists.commit 失败：配置值已写入，但可能没有落盘");
        finishApplyConfig();
        return;
    }
    applyPhase = ApplyPhase::WaitingCommit;
    waitDeadline = std::chrono::steady_clock::now() + kListsResponseTimeout;
}

void SettingsPanel::Impl::finishApplyConfig() {
    // 配置阶段收尾：以宿主/插件当前值为新基线重建配置区。未应用的本地修改由
    // mergeDirtyValues 保住，失败项的红色标记也在那里保留下来。
    refreshConfig();
    if (hasPendingPluginChanges()) {
        beginApplyPlugins();
        return;
    }
    resetApplyState();
    reportApplyResult();
}

void SettingsPanel::Impl::beginApplyPlugins() {
    shot::PluginRegistry registry(host);
    int changed = 0;
    int rejected = 0;
    for (PluginRow& row : plugins) {
        if (row.enabledPending == row.enabledNow) {
            continue;
        }
        // 宿主只登记 pending，下一轮 main 线程才真正卸载/加载。
        if (!registry.available() ||
            !registry.setEnabled(row.status.id.c_str(), row.enabledPending)) {
            row.message = "宿主拒绝了这次启停请求";
            ++rejected;
            continue;
        }
        row.enabledNow = row.enabledPending;
        row.message.clear();
        ++changed;
    }
    applyPluginChanged = changed;
    applyPluginRejected = rejected;
    if (changed > 0) {
        applyPhase = ApplyPhase::WaitingPlugins;
        waitDeadline = std::chrono::steady_clock::now() + kPluginChangeTimeout;
        return;
    }
    resetApplyState();
    reportApplyResult();
}

void SettingsPanel::Impl::resetApplyState() {
    applyPhase = ApplyPhase::Idle;
    pendingTag.clear();
    pendingChanges.clear();
    waitDeadline = std::chrono::steady_clock::time_point{};
}

bool SettingsPanel::Impl::isBusy() const {
    return applyPhase != ApplyPhase::Idle;
}

std::string SettingsPanel::Impl::busyText() const {
    switch (applyPhase) {
        case ApplyPhase::WaitingUpdate:
            return "应用中…（写入配置）";
        case ApplyPhase::WaitingCommit:
            return "应用中…（保存配置）";
        case ApplyPhase::WaitingPlugins:
            return "应用中…（等待宿主完成插件启停）";
        case ApplyPhase::Idle:
        default:
            return "应用中…";
    }
}

void SettingsPanel::Impl::reportApplyResult() {
    const int unconfirmed = applyConfigTotal - applyConfigApplied - applyConfigRejected;
    if (applyConfigRejected > 0 || applyPluginRejected > 0) {
        setStatus(Tone::Error, "应用完成，但有 " + std::to_string(applyConfigRejected) +
                                   " 项配置和 " + std::to_string(applyPluginRejected) +
                                   " 个插件启停被拒绝");
    } else if (unconfirmed > 0) {
        setStatus(Tone::Error, "有 " + std::to_string(unconfirmed) + " 项配置没有被宿主确认");
    } else if (applySaveFailed) {
        setStatus(Tone::Error, "配置保存失败（值可能已生效，但没有落盘）");
    } else if (applyConfigApplied > 0 || applyPluginChanged > 0) {
        setStatus(Tone::Success, "已应用");
    } else {
        setStatus(Tone::Normal, "没有需要应用的修改");
    }
    shot::Logger(host).info("settings_ui：应用 " + std::to_string(applyConfigApplied) + "/" +
                            std::to_string(applyConfigTotal) + " 项配置、" +
                            std::to_string(applyPluginChanged) + " 个插件启停");
}

// ---------------------------------------------------------------------------
// 事件回调（UI 线程 drain 队列后调用）
// ---------------------------------------------------------------------------
void SettingsPanel::Impl::onListsUpdated(std::string_view payload) {
    if (applyPhase != ApplyPhase::WaitingUpdate && applyPhase != ApplyPhase::WaitingCommit) {
        return;  // 没有进行中的提交：陈旧回显或别家的响应，忽略
    }
    const nlohmann::json document = parseObject(payload);
    if (document.is_discarded() || !document.is_object()) {
        return;  // 负载坏了就继续等超时兜底，不猜内容
    }
    const std::string listId = document.value("list", std::string());
    if (!listId.empty() && listId != "config_list") {
        return;
    }
    const std::string tag = document.value("tag", std::string());
    if (!tag.empty() && tag != pendingTag) {
        return;  // 不是本次提交的回显（例如上一次超时后迟到的结果）
    }
    const std::string error = document.value("error", std::string());

    if (applyPhase == ApplyPhase::WaitingCommit) {
        // lists.commit 失败时宿主用"同 tag 的 lists.updated + error"回落，不再发
        // config.applied；没有 error 的回显不是失败信号，继续等 config.applied。
        if (error.empty()) {
            return;
        }
        applySaveFailed = true;
        setStatus(Tone::Error, "保存配置失败：" + error);
        finishApplyConfig();
        return;
    }

    if (!error.empty()) {
        // 整批写值失败（宿主在 results 之外给的兜底错误）：不再进入提交阶段。
        setStatus(Tone::Error, "写入配置失败：" + error);
        resetApplyState();
        return;
    }

    // 逐项结果：成功项以"提交值"作为新基线，失败项标红并保持脏标记。
    const auto results = document.find("results");
    if (results != document.end() && results->is_array()) {
        for (const nlohmann::json& result : *results) {
            if (!result.is_object()) {
                continue;
            }
            const std::string owner = result.value("owner", std::string());
            const std::string id = result.value("id", std::string());
            ConfigItem* item = findConfigItem(owner, id);
            if (item == nullptr) {
                continue;  // 结果对应的项已经被刷新掉了（插件卸载/declaration 变化）
            }
            if (jsonBool(result, "ok", false)) {
                // 用提交值而不是 item->value：等待期间用户可能又改了控件，
                // 那样 value != baseline，仍然算脏，语义正确。
                const std::string* submitted = findPendingChangeValue(owner, id);
                if (submitted != nullptr) {
                    item->baseline = *submitted;
                }
                item->error.clear();
                ++applyConfigApplied;
            } else {
                const std::string reason = result.value("reason", std::string());
                item->error = reason.empty() ? "该值不被接受" : reason;
                ++applyConfigRejected;
            }
        }
    }
    beginApplyCommit();
}

void SettingsPanel::Impl::onListsChanged(std::string_view payload) {
    // 负载 {"list":"config_list"}；只关心配置列表，其它列表是别的消费方的事。
    if (host == nullptr) {
        return;
    }
    if (!payload.empty()) {
        const nlohmann::json document = parseObject(payload);
        if (!document.is_discarded() && document.is_object()) {
            const std::string listId = document.value("list", std::string());
            if (!listId.empty() && listId != "config_list") {
                return;
            }
        }
    }
    // 收到事件才刷新，不要每帧读 JSON；刷新会按 owner+id 合并回未应用的本地修改。
    refreshConfig();
}

void SettingsPanel::Impl::onConfigApplied() {
    if (applyPhase == ApplyPhase::WaitingCommit) {
        applySaveFailed = false;  // 收到 config.applied = 保存确认
        finishApplyConfig();
        return;
    }
    if (applyPhase != ApplyPhase::Idle) {
        return;  // 还在等写值结果 / 等启停：不要让一个旁路的 applied 打乱阶段
    }
    // 宿主从别的路径保存过配置（例如另一个设置页实例 / 命令行）：刷新配置区，
    // 但不动插件管理表的"待应用"勾选（refreshPlugins 会把它重置掉）。
    refreshConfig();
}

void SettingsPanel::Impl::onPluginsChanged(std::string_view payload) {
    if (applyPhase == ApplyPhase::WaitingUpdate || applyPhase == ApplyPhase::WaitingCommit) {
        // 配置提交正等着 lists.updated / config.applied：不要让一条旁路的插件通知
        // 打断状态机；只刷新配置区（config_list 可能因插件状态变化而增删节）。
        refreshConfig();
        return;
    }
    const bool wasWaiting = (applyPhase == ApplyPhase::WaitingPlugins);
    resetApplyState();  // 第一条通知就结束"应用中…"（一次 set_enabled 可能发多条）
    refresh();          // 节可能增删：整表重读，别继续用旧 index / 旧声明

    bool rejected = false;
    if (!payload.empty()) {
        // 负载是宿主发的 JSON（document/SETTINGS.md §3.2）。解析失败就只刷新、不猜内容。
        const nlohmann::json document = parseObject(payload);
        if (!document.is_discarded() && document.is_object()) {
            const std::string pluginId = document.value("plugin", std::string());
            const std::string reason = document.value("reason", std::string());
            if (!pluginId.empty() && !reason.empty()) {
                rejected = true;
                bool shown = false;
                for (PluginRow& row : plugins) {
                    if (row.status.id == pluginId) {
                        row.message = reason;
                        shown = true;
                    }
                }
                if (!shown) {
                    setStatus(Tone::Error, "插件 " + pluginId + "：" + reason);
                }
            }
        }
    }
    if (rejected) {
        setStatus(Tone::Error, "有插件启停被拒绝，原因见插件管理表");
    } else if (wasWaiting) {
        reportApplyResult();  // 配置阶段的结果 + 启停成功 = 一次完整提交结束
    } else {
        setStatus(Tone::Normal, "插件列表已更新");
    }
}

void SettingsPanel::Impl::checkWaitTimeout() {
    if (applyPhase == ApplyPhase::Idle) {
        return;
    }
    if (std::chrono::steady_clock::now() < waitDeadline) {
        return;
    }
    switch (applyPhase) {
        case ApplyPhase::WaitingUpdate:
            setStatus(Tone::Error, "等待 lists.updated 超时：宿主没有确认配置写入（修改仍保留，可重试）");
            resetApplyState();
            break;
        case ApplyPhase::WaitingCommit:
            applySaveFailed = true;
            setStatus(Tone::Error, "等待 config.applied 超时：配置可能已生效，但落盘结果未知");
            finishApplyConfig();
            break;
        case ApplyPhase::WaitingPlugins:
            setStatus(Tone::Error, "等待插件启停超时：宿主没有发来 plugins.changed");
            resetApplyState();
            break;
        case ApplyPhase::Idle:
        default:
            resetApplyState();
            break;
    }
}

// ---------------------------------------------------------------------------
// 本地操作（不碰宿主）
// ---------------------------------------------------------------------------
void SettingsPanel::Impl::cancel() {
    if (isBusy()) {
        return;  // 提交进行中不允许取消：事件回来会覆盖本地状态
    }
    for (ProviderSection& section : sections) {
        for (ConfigItem& item : section.items) {
            item.value = item.baseline;
            item.error.clear();
            item.hint.clear();
            syncColorHex(item);
        }
    }
    for (PluginRow& row : plugins) {
        row.enabledPending = row.enabledNow;
    }
    resetApplyState();
    setStatus(Tone::Normal, "已丢弃未应用的修改");
}

void SettingsPanel::Impl::restoreDefaults() {
    if (isBusy()) {
        return;
    }
    int changed = 0;
    for (ProviderSection& section : sections) {
        if (section.removed) {
            continue;
        }
        for (ConfigItem& item : section.items) {
            item.value = defaultValueFor(item.desc);
            item.error.clear();
            syncColorHex(item);
            ++changed;
        }
    }
    // 只改本地缓冲：默认值也要用户点「应用」才写回（SETTINGS.md §4 的编辑模型）。
    if (changed > 0) {
        setStatus(Tone::Warning, "已恢复默认值，点「应用」后写回");
    } else {
        setStatus(Tone::Normal, "没有可恢复默认值的配置项");
    }
}

void SettingsPanel::Impl::setStatus(Tone tone, std::string text) {
    statusTone = tone;
    statusText = std::move(text);
}

bool SettingsPanel::Impl::publishEvent(const ShotHostServices* host, const char* topic,
                                       std::string_view payload) {
    shot::EventBus bus(host);
    return bus.publish(topic, payload);
}

}  // namespace settings_ui
