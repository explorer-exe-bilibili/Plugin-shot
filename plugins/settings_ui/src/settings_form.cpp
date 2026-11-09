// settings_form —— 设置页的"表单"：顶部操作条 + 按提供者分节的滚动内容 +
// 7 种 SHOT_CONFIG_KIND_* 对应的 ImGui 控件。
//
// 只负责"把模型画出来、把用户改动写回本地缓冲"；读取模型在 settings_model.cpp，
// 点「应用」之后的提交在 settings_apply.cpp，插件管理表在 settings_plugin_table.cpp。
//
// PATH 项的"浏览…"通过 settings_dialog.cpp 调 nativefiledialog-extended；本文件不
// 直接依赖任何平台 API，对话框不可用时把提示写在配置项旁边、用户可手输路径。

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_stdlib.h"
#include "settings_dialog.h"
#include "settings_panel_impl.h"
#include "shot_plugin_helpers.h"

namespace settings_ui {

void SettingsPanel::Impl::renderActionBar() {
    const bool busy = isBusy();
    const bool pending = hasPendingChanges();
    const bool canApply = pending && !busy;

    ImGui::BeginDisabled(!canApply);
    if (ImGui::Button("应用")) {
        apply();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canApply);
    if (ImGui::Button("取消")) {
        cancel();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("恢复全部默认")) {
        restoreDefaults();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    // 状态文本优先级：等待中 > 有未应用修改 > 上一次操作的结果。
    std::string text = statusText;
    Tone tone = statusTone;
    if (busy) {
        // 事件驱动的提交分几个等待阶段：显示当前阶段，用户才知道在等什么。
        text = busyText();
        tone = Tone::Warning;
    } else if (pending) {
        text = "有未应用的修改";
        tone = Tone::Warning;
    }
    if (text.empty()) {
        ImGui::TextDisabled("没有未应用的修改");
    } else {
        ImGui::TextColored(toneColor(tone), "%s", text.c_str());
    }
}

void SettingsPanel::Impl::renderConfigSections() {
    if (configUnavailable) {
        // 老宿主没有列表服务 / config_list 暂时读不到：显示具体原因，插件管理照常渲染。
        ImGui::TextDisabled("%s", configUnavailableReason.c_str());
        renderPluginTable();
        // 刚被热卸载的提供者：还显示一节灰色的"插件已卸载"，下一次刷新就消失。
        for (ProviderSection& section : removedSections) {
            renderProviderSection(section);
        }
        return;
    }
    // 宿主 core 固定排最前（config_list 一般也按这个顺序给，但这里不依赖它的顺序）。
    for (ProviderSection& section : sections) {
        if (section.id == "core") {
            renderProviderSection(section);
            break;
        }
    }
    renderPluginTable();
    if (sections.empty()) {
        ImGui::TextDisabled("宿主与插件都没有声明配置项。");
    }
    for (ProviderSection& section : sections) {
        if (section.id == "core") {
            continue;  // 已经在插件管理表之前画过
        }
        renderProviderSection(section);
    }
    // 刚被热卸载的提供者：还显示一节灰色的"插件已卸载"，下一次刷新就消失。
    for (ProviderSection& section : removedSections) {
        renderProviderSection(section);
    }
}

void SettingsPanel::Impl::renderProviderSection(ProviderSection& section) {
    const std::string header = (section.name == section.id)
                                   ? section.name
                                   : (section.name + "（" + section.id + "）");
    ImGui::PushID(section.id.c_str());
    if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (section.removed) {
            ImGui::TextColored(kToneWarning, "插件已卸载：本节只剩最后一次读到的值，不能编辑。");
        } else if (section.items.empty()) {
            ImGui::TextDisabled("该提供者没有声明任何配置项。");
        }
        if (section.removed) {
            ImGui::BeginDisabled();
        }
        if (!section.items.empty()) {
            const ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingStretchProp |
                                               ImGuiTableFlags_BordersInnerH |
                                               ImGuiTableFlags_PadOuterX;
            if (ImGui::BeginTable("##items", 2, tableFlags)) {
                ImGui::TableSetupColumn("配置项", ImGuiTableColumnFlags_WidthFixed, 200.0f);
                ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch);
                for (ConfigItem& item : section.items) {
                    renderConfigItem(item);
                }
                ImGui::EndTable();
            }
        }
        if (section.removed) {
            ImGui::EndDisabled();
        }
    }
    ImGui::PopID();
}

void SettingsPanel::Impl::renderConfigItem(ConfigItem& item) {
    const ConfigDesc& desc = item.desc;
    ImGui::PushID(desc.key.c_str());
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(desc.label.empty() ? desc.key.c_str() : desc.label.c_str());
    // 悬停提示：插件的 tooltip 是给用户看的；把 key 也带上，排查配置问题时不用翻代码。
    const std::string tooltip = desc.tooltip.empty() ? ("键：" + desc.key)
                                                     : (desc.tooltip + "\n键：" + desc.key);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }

    ImGui::TableNextColumn();
    // 控件自己把新值写回 item.value；这里只关心它画完，不需要"变没变"的返回值。
    renderItemControl(item);

    ImGui::SameLine();
    if (ImGui::SmallButton("恢复默认")) {
        item.value = defaultValueFor(desc);
        item.error.clear();
        syncColorHex(item);
    }

    if (!item.error.empty()) {
        ImGui::SameLine();
        // 失败原因来自 lists.updated 的 reason（宿主转述插件 set_config 的结果）。
        ImGui::TextColored(kToneError, "写入失败：%s", item.error.c_str());
    } else if (item.value != item.baseline) {
        ImGui::SameLine();
        ImGui::TextColored(kToneWarning, "已修改");
    }
    // 控件级提示（例如文件对话框不可用 → 请手输）：换一行显示，别把表格撑宽。
    if (!item.hint.empty()) {
        ImGui::TextColored(kToneWarning, "%s", item.hint.c_str());
    }
    ImGui::PopID();
}

// 返回 true = 这一帧控件改了值（值已经写回 item.value）。
bool SettingsPanel::Impl::renderItemControl(ConfigItem& item) {
    const ConfigDesc& desc = item.desc;
    switch (desc.kind) {
        case SHOT_CONFIG_KIND_BOOL: {
            bool value = parseBoolValue(item.value);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Checkbox("##bool", &value)) {
                item.value = value ? "1" : "0";
                return true;
            }
            return false;
        }
        case SHOT_CONFIG_KIND_INT: {
            const bool hasRange = desc.max > desc.min;
            int value = parseIntValue(item.value, parseIntValue(desc.defaultValue,
                                                               static_cast<int>(desc.min)));
            bool changed = false;
            if (hasRange) {
                const int minimum = static_cast<int>(std::lround(desc.min));
                const int maximum = static_cast<int>(std::lround(desc.max));
                ImGui::SetNextItemWidth(160.0f);
                changed |= ImGui::SliderInt("##slider", &value, minimum, maximum);
                ImGui::SameLine();
            }
            const int step = (desc.step >= 1.0) ? static_cast<int>(std::lround(desc.step)) : 1;
            ImGui::SetNextItemWidth(80.0f);
            changed |= ImGui::InputInt("##input", &value, step, step * 10);
            if (changed) {
                if (hasRange) {
                    value = std::clamp(value, static_cast<int>(std::lround(desc.min)),
                                       static_cast<int>(std::lround(desc.max)));
                }
                item.value = formatIntValue(value);
            }
            return changed;
        }
        case SHOT_CONFIG_KIND_FLOAT: {
            const bool hasRange = desc.max > desc.min;
            double value =
                parseDoubleValue(item.value, parseDoubleValue(desc.defaultValue, desc.min));
            bool changed = false;
            if (hasRange) {
                const double minimum = desc.min;
                const double maximum = desc.max;
                ImGui::SetNextItemWidth(160.0f);
                changed |= ImGui::SliderScalar("##slider", ImGuiDataType_Double, &value,
                                               &minimum, &maximum, "%.6g");
                ImGui::SameLine();
            }
            const double step = (desc.step > 0.0) ? desc.step : 0.1;
            ImGui::SetNextItemWidth(80.0f);
            changed |= ImGui::InputDouble("##input", &value, step, step * 10.0, "%.6g");
            if (changed) {
                if (hasRange) {
                    value = std::clamp(value, desc.min, desc.max);
                }
                item.value = formatDoubleValue(value);
            }
            return changed;
        }
        case SHOT_CONFIG_KIND_ENUM: {
            if (item.enumOptions.empty()) {
                ImGui::TextDisabled("（枚举没有候选值）");
                return false;
            }
            int current = 0;
            for (std::size_t index = 0; index < item.enumOptions.size(); ++index) {
                if (item.enumOptions[index] == item.value) {
                    current = static_cast<int>(index);
                    break;
                }
            }
            std::vector<const char*> labels;
            labels.reserve(item.enumOptions.size());
            for (const std::string& option : item.enumOptions) {
                labels.push_back(option.c_str());
            }
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo("##enum", &current, labels.data(), static_cast<int>(labels.size()))) {
                item.value = item.enumOptions[static_cast<std::size_t>(current)];
                return true;
            }
            return false;
        }
        case SHOT_CONFIG_KIND_STRING: {
            ImGui::SetNextItemWidth(320.0f);
            return ImGui::InputText("##string", &item.value);
        }
        case SHOT_CONFIG_KIND_COLOR: {
            float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            parseColorValue(item.value, color);  // 解析失败就用黑色，不能因为坏值崩掉
            bool changed = ImGui::ColorEdit4(
                "##color", color,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar |
                    ImGuiColorEditFlags_AlphaPreviewHalf);
            if (changed) {
                item.value = formatColorHex(color);
                item.colorHex = item.value;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::InputText("##hex", &item.colorHex)) {
                float edited[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                if (parseColorValue(item.colorHex, edited)) {
                    item.value = formatColorHex(edited);
                    changed = true;
                }
            }
            return changed;
        }
        case SHOT_CONFIG_KIND_PATH: {
            ImGui::SetNextItemWidth(320.0f);
            bool changed = ImGui::InputText("##path", &item.value);
            ImGui::SameLine();
            if (ImGui::Button("浏览…")) {
                std::string chosen = item.value;
                std::string dialogError;
                // 描述符没有声明 PATH 是文件还是目录：当前值指向已存在目录时按目录选，
                // 否则按文件选（与旧版"只选文件"的行为一致，目录是额外支持）。
                std::error_code pathError;
                const bool pickFolder =
                    !chosen.empty() && std::filesystem::is_directory(chosen, pathError);
                if (pickFolder ? pickExistingFolder(chosen, dialogError)
                               : pickExistingFile(chosen, dialogError)) {
                    item.value = std::move(chosen);
                    item.hint.clear();
                    changed = true;
                } else if (!dialogError.empty()) {
                    item.hint = std::move(dialogError);
                    shot::Logger(host).warn("settings_ui：PATH 文件对话框不可用，已降级为手输路径");
                }
            }
            return changed;
        }
        default:
            ImGui::TextDisabled("不支持的配置类型（kind=%u）", desc.kind);
            return false;
    }
}

}  // namespace settings_ui
