// settings_apply —— 设置页的"提交"：应用 / 取消 / 恢复默认，以及宿主事件
// （config.applied / plugins.changed）与启停等待超时。
//
// 应用流程严格按 document/SETTINGS.md §4：
//   1. 脏配置项逐条 set_value（失败项标红）；
//   2. 变化的启停逐条 set_enabled（宿主只登记，下一轮 main 线程执行）；
//   3. save() 落盘（宿主发布 config.applied）；
//   4. 有启停变更时进入"应用中…"，等 plugins.changed 到达后结束等待。
//
// 渲染在 settings_form.cpp / settings_plugin_table.cpp；这里不画任何控件。

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "settings_panel_impl.h"
#include "shot_plugin_helpers.h"

namespace settings_ui {
namespace {

// 启停是异步的：宿主处理完会发 plugins.changed。等不到时给个兜底，别让界面永远"应用中…"。
constexpr std::chrono::seconds kPluginChangeTimeout(5);

}  // namespace

void SettingsPanel::Impl::apply() {
    if (waitingForPlugins) {
        return;
    }
    if (host == nullptr) {
        setStatus(Tone::Error, "宿主服务不可用，无法应用");
        return;
    }

    shot::PluginConfig config(host);
    shot::PluginRegistry registry(host);

    // 1. 脏配置项逐条写回。set_value 同步转调插件的 set_config，失败项留在原地标红。
    int appliedConfigCount = 0;
    int rejectedConfigCount = 0;
    int unavailableConfigCount = 0;
    for (ProviderSection& section : sections) {
        if (section.removed) {
            continue;
        }
        for (ConfigItem& item : section.items) {
            if (item.value == item.baseline) {
                item.error.clear();
                continue;
            }
            if (!config.available()) {
                item.error = "宿主未提供设置服务";
                ++unavailableConfigCount;
                continue;
            }
            if (config.setString(section.id.c_str(), item.desc.key.c_str(), item.value)) {
                item.baseline = item.value;
                item.error.clear();
                ++appliedConfigCount;
            } else {
                item.error = "该值不被接受";
                ++rejectedConfigCount;
            }
        }
    }

    // 2. 启停变更逐条登记（宿主只记 pending，下一轮 main 线程才真正卸载/加载）。
    int changedPluginCount = 0;
    int rejectedPluginCount = 0;
    for (PluginRow& row : plugins) {
        if (row.enabledPending == row.enabledNow) {
            row.message.clear();
            continue;
        }
        if (!registry.available() ||
            !registry.setEnabled(row.status.id.c_str(), row.enabledPending)) {
            row.message = "宿主拒绝了这次启停请求";
            ++rejectedPluginCount;
            continue;
        }
        row.enabledNow = row.enabledPending;
        row.message.clear();
        ++changedPluginCount;
    }

    // 3. 落盘（宿主同时发布 config.applied）。配置服务不可用时跳过，启停仍然可能已经登记。
    const bool saved = config.available() ? config.save() : false;

    // 4. 状态：有启停变更就进入"应用中…"，等 plugins.changed；超时由 checkWaitTimeout 兜底。
    if (changedPluginCount > 0) {
        waitingForPlugins = true;
        waitDeadline = std::chrono::steady_clock::now() + kPluginChangeTimeout;
    }
    if (rejectedConfigCount > 0 || rejectedPluginCount > 0) {
        setStatus(Tone::Error, "应用完成，但有 " + std::to_string(rejectedConfigCount) +
                                   " 项配置和 " + std::to_string(rejectedPluginCount) +
                                   " 个插件启停被拒绝");
    } else if (unavailableConfigCount > 0) {
        setStatus(Tone::Error, "有 " + std::to_string(unavailableConfigCount) +
                                   " 项配置没能写入：宿主设置服务不可用");
    } else if (!saved && appliedConfigCount > 0) {
        setStatus(Tone::Error, "配置保存失败（值已生效，但没有落盘）");
    } else if (changedPluginCount > 0) {
        setStatus(Tone::Warning, "应用中…");
    } else if (appliedConfigCount > 0) {
        setStatus(Tone::Success, "已应用");
    } else {
        setStatus(Tone::Normal, "没有需要应用的修改");
    }

    shot::Logger(host).info("settings_ui：应用 " + std::to_string(appliedConfigCount) +
                            " 项配置、" + std::to_string(changedPluginCount) + " 个插件启停");
}

void SettingsPanel::Impl::cancel() {
    for (ProviderSection& section : sections) {
        for (ConfigItem& item : section.items) {
            item.value = item.baseline;
            item.error.clear();
            syncColorHex(item);
        }
    }
    for (PluginRow& row : plugins) {
        row.enabledPending = row.enabledNow;
    }
    waitingForPlugins = false;
    setStatus(Tone::Normal, "已丢弃未应用的修改");
}

void SettingsPanel::Impl::restoreDefaults() {
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
    // 只改本地缓冲：默认值也要用户点「应用」才写回插件（SETTINGS.md §4 的编辑模型）。
    if (changed > 0) {
        setStatus(Tone::Warning, "已恢复默认值，点「应用」后写回");
    } else {
        setStatus(Tone::Normal, "没有可恢复默认值的配置项");
    }
}

void SettingsPanel::Impl::onPluginsChanged(std::string_view payload) {
    const bool wasWaiting = waitingForPlugins;
    waitingForPlugins = false;
    refresh();  // 节可能增删：整表重读，别继续用旧 index / 旧声明

    bool rejected = false;
    if (!payload.empty()) {
        // 负载是宿主发的 JSON（document/SETTINGS.md §3.2）。解析失败就只刷新、不猜内容。
        const nlohmann::json doc = nlohmann::json::parse(payload.begin(), payload.end(),
                                                         nullptr, /*allow_exceptions=*/false);
        if (!doc.is_discarded() && doc.is_object()) {
            const std::string pluginId = doc.value("plugin", std::string());
            const std::string reason = doc.value("reason", std::string());
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
        setStatus(Tone::Success, "已应用");
    } else {
        setStatus(Tone::Normal, "插件列表已更新");
    }
}

void SettingsPanel::Impl::onConfigApplied() {
    // 应用刚成功：全量刷新，以插件当前值为新基线（简单做法，SETTINGS.md §4）。
    // 例外：如果有项被插件拒绝，不能刷新——否则会把用户刚输入、还没被接受的值冲掉，
    // 红色标记也就失去意义；保留本地模型让用户改完再应用。
    const bool hasRejected = std::any_of(
        sections.begin(), sections.end(), [](const ProviderSection& section) {
            return std::any_of(section.items.begin(), section.items.end(),
                               [](const ConfigItem& item) { return !item.error.empty(); });
        });
    if (!hasRejected) {
        refresh();
    }
}

void SettingsPanel::Impl::setStatus(Tone tone, std::string text) {
    statusTone = tone;
    statusText = std::move(text);
}

void SettingsPanel::Impl::checkWaitTimeout() {
    if (!waitingForPlugins) {
        return;
    }
    if (std::chrono::steady_clock::now() < waitDeadline) {
        return;
    }
    waitingForPlugins = false;
    setStatus(Tone::Error, "等待插件启停超时：宿主没有发来 plugins.changed");
}

}  // namespace settings_ui
