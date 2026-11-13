// settings_plugin_table —— 「插件」页签的渲染：启用复选框 / 插件名(id) + 描述 /
// 版本 / 状态。
//
// 数据来自 settings_model.cpp 读出的注册表快照（PluginRow）。名字与描述先按
// i18n_list 的 plugin.name / plugin.description 解析（owner = 插件 id），解析
// 不到时分别退回 metadata 名与 plugin_list 的 description；两者都没有就只显示
// id / 不显示描述。勾选只改本地 enabledPending，真正 set_enabled 在
// settings_apply.cpp。入口插件（settings_ui.main 自己）的复选框在这里置灰：
// 禁用它会连设置页一起卸载。

#include <string>
#include <string_view>

#include "imgui.h"

#include "plugin_entry.h"
#include "settings_panel_impl.h"

namespace settings_ui {

void SettingsPanel::Impl::renderPluginPage() {
    if (registryUnavailable) {
        ImGui::TextDisabled(
            "%s", tr("settings_ui.registry_unavailable",
                     "宿主未提供插件注册表服务（shot.host.plugin_registry）："
                     "当前宿主不能管理插件启停。"));
        return;
    }
    if (plugins.empty()) {
        ImGui::TextDisabled("%s", tr("settings_ui.no_plugins", "没有已安装的插件。"));
        return;
    }
    renderPluginTable();
}

void SettingsPanel::Impl::renderPluginTable() {
    // 页签本身已经叫「插件」，这里只留一句操作提示，不再套 CollapsingHeader：
    // 再套一层折叠标题会让表格默认折起来，用户要点两次才能看到内容。
    ImGui::TextUnformatted(tr(
        "settings_ui.plugin_table_title", "插件管理（勾选后要点「应用」才生效）"));
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable("##plugins", 4, tableFlags)) {
        // 固定列宽要同时放得下中文"启用"和英文"Enabled"（约 68px + padding）。
        ImGui::TableSetupColumn(tr("settings_ui.column_enabled", "启用"),
                                ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn(tr("settings_ui.column_plugin", "插件"),
                                ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn(tr("settings_ui.column_version", "版本"),
                                ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn(tr("settings_ui.column_status", "状态"),
                                ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        for (PluginRow& row : plugins) {
            renderPluginRow(row);
        }
        ImGui::EndTable();
    }
}

void SettingsPanel::Impl::renderPluginRow(PluginRow& row) {
    ImGui::PushID(row.status.id.c_str());
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    // 入口插件不能禁用自己：设置页就活在这个插件里，禁用等于把正在用的入口卸载掉。
    // 宿主不为此加特判（SETTINGS.md §3.1），所以这里把复选框置灰。
    const bool isEntry = (row.status.id == kSettingsUi.id);
    bool checked = row.enabledPending;
    if (isEntry) {
        ImGui::BeginDisabled();
        ImGui::Checkbox("##enabled", &checked);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s", tr("settings_ui.entry_disabled_tooltip",
                         "入口插件不能禁用自己（禁用会连设置页一起卸载）"));
        }
    } else if (ImGui::Checkbox("##enabled", &checked)) {
        row.enabledPending = checked;
    }

    ImGui::TableNextColumn();
    // 名字的解析顺序：plugin.name 译文 → metadata name（注册表快照）→ id。
    // 另一个 agent 正在给各插件补 plugin.name 文案，不能假设它一定存在；
    // i18n 查不到时 resolve 原样返回 key，所以"结果 == key"就是没有译文。
    const char* const kPluginNameKey = "plugin.name";
    const char* const localizedName = resolve(row.status.id.c_str(), kPluginNameKey);
    const bool hasLocalizedName =
        (localizedName != nullptr) && (std::string_view(localizedName) != kPluginNameKey);
    std::string displayName = hasLocalizedName ? std::string(localizedName) : row.status.name;
    if (displayName.empty()) {
        displayName = row.status.id;  // metadata 名缺失时用 id 兜底（宿主已按此投影）
    }
    const std::string label =
        (displayName == row.status.id)
            ? displayName
            : trFormat("settings_ui.name_with_id", "%s（%s）", displayName.c_str(),
                       row.status.id.c_str());
    ImGui::TextUnformatted(label.c_str());
    if (isEntry) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("settings_ui.entry_marker", "（入口）"));
    }
    if (row.enabledPending != row.enabledNow) {
        ImGui::SameLine();
        ImGui::TextColored(kToneWarning, "%s",
                           tr("settings_ui.pending_marker", "（待应用）"));
    }

    // 描述：plugin.description 译文 → plugin_list.description（metadata 原文）→ 不显示。
    // TextWrapped 按插件列宽自动换行，ImGui 据此把行高撑到放得下名称 + 描述两行；
    // 这里不写死行高，否则长描述会被裁掉、短描述又留下大块空白。
    const char* const kPluginDescriptionKey = "plugin.description";
    const char* const localizedDescription =
        resolve(row.status.id.c_str(), kPluginDescriptionKey);
    const bool hasLocalizedDescription =
        (localizedDescription != nullptr) &&
        (std::string_view(localizedDescription) != kPluginDescriptionKey);
    const std::string description =
        hasLocalizedDescription ? std::string(localizedDescription) : row.description;
    if (!description.empty()) {
        // 描述是辅助信息：用禁用文字色压一级，避免和插件名抢注意力。
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", description.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(row.status.version.empty() ? "-" : row.status.version.c_str());

    ImGui::TableNextColumn();
    ImVec4 stateColor = kToneNormal;
    const char* stateText = tr("settings_ui.state_unknown", "未知状态");
    switch (row.status.state) {
        case SHOT_PLUGIN_STATE_LOADED:
            stateText = tr("settings_ui.state_loaded", "已加载");
            stateColor = kToneSuccess;
            break;
        case SHOT_PLUGIN_STATE_DISABLED:
            stateText = tr("settings_ui.state_disabled", "已禁用");
            stateColor = kToneWarning;
            break;
        case SHOT_PLUGIN_STATE_FAILED:
            stateText = tr("settings_ui.state_failed", "加载失败");
            stateColor = kToneError;
            break;
        default:
            break;
    }
    ImGui::TextColored(stateColor, "%s", stateText);
    // 拒绝/失败原因：plugins.changed 带回的优先，其次用注册表快照里的原因。
    const std::string& detail = row.message.empty() ? row.status.reason : row.message;
    if (!detail.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kToneError);
        ImGui::TextWrapped("%s", detail.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

}  // namespace settings_ui
