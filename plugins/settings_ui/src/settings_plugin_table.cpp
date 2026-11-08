// settings_plugin_table —— 插件管理表的渲染：启用复选框 / 插件名(id) / 版本 / 状态。
//
// 数据来自 settings_model.cpp 读出的注册表快照（PluginRow）；勾选只改本地
// enabledPending，真正 set_enabled 在 settings_apply.cpp。
// 入口插件（settings_ui.main 自己）的复选框在这里置灰：禁用它会连设置页一起卸载。

#include <string>

#include "imgui.h"

#include "plugin_entry.h"
#include "settings_panel_impl.h"

namespace settings_ui {

void SettingsPanel::Impl::renderPluginTable() {
    if (!ImGui::CollapsingHeader("插件管理（勾选后要点「应用」才生效）",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (registryUnavailable) {
        ImGui::TextDisabled("宿主未提供插件注册表服务（shot.host.plugin_registry）："
                            "当前宿主不能管理插件启停。");
        return;
    }
    if (plugins.empty()) {
        ImGui::TextDisabled("没有已安装的插件。");
        return;
    }
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable("##plugins", 4, tableFlags)) {
        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("插件", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("版本", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthStretch, 2.0f);
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
            ImGui::SetTooltip("入口插件不能禁用自己（禁用会连设置页一起卸载）");
        }
    } else if (ImGui::Checkbox("##enabled", &checked)) {
        row.enabledPending = checked;
    }

    ImGui::TableNextColumn();
    const std::string label = (row.status.name.empty() || row.status.name == row.status.id)
                                  ? row.status.id
                                  : (row.status.name + "（" + row.status.id + "）");
    ImGui::TextUnformatted(label.c_str());
    if (isEntry) {
        ImGui::SameLine();
        ImGui::TextDisabled("（入口）");
    }
    if (row.enabledPending != row.enabledNow) {
        ImGui::SameLine();
        ImGui::TextColored(kToneWarning, "（待应用）");
    }

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(row.status.version.empty() ? "-" : row.status.version.c_str());

    ImGui::TableNextColumn();
    ImVec4 stateColor = kToneNormal;
    const char* stateText = "未知状态";
    switch (row.status.state) {
        case SHOT_PLUGIN_STATE_LOADED:
            stateText = "已加载";
            stateColor = kToneSuccess;
            break;
        case SHOT_PLUGIN_STATE_DISABLED:
            stateText = "已禁用";
            stateColor = kToneWarning;
            break;
        case SHOT_PLUGIN_STATE_FAILED:
            stateText = "加载失败";
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
