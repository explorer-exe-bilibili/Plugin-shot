// settings_form —— 设置页的"表单"：顶部操作条 + 按提供者分节的滚动内容 +
// 7 种 ShotConfigDesc::kind 对应的 ImGui 控件。
//
// 只负责"把模型画出来、把用户改动写回本地缓冲"；读取模型在 settings_model.cpp，
// 点「应用」之后的提交在 settings_apply.cpp，插件管理表在 settings_plugin_table.cpp。
//
// PATH 项的"浏览…"要弹系统文件对话框（Win32 的 GetOpenFileNameW），这是本文件里
// 唯一出现 Win32 的地方；公共头不含任何 Win32 类型。

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX  // 防止 <windows.h> 的 min/max 宏把 std::min / std::max 顶掉
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commdlg.h>

#include "imgui.h"
#include "imgui_stdlib.h"
#include "settings_panel_impl.h"

namespace settings_ui {
namespace {

// UTF-8 <-> UTF-16 转换与文件对话框只服务 PATH 控件的"浏览…"：既不属于设置页
// 模型，也不属于窗口状态，所以留在本 TU 的匿名命名空间（STYLE.md §6.7）。
std::wstring utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::string wideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return std::string();
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

/**
 * @brief 弹系统"打开文件"对话框给 PATH 配置项选路径。
 *
 * 用 GetOpenFileNameW 而不是自己糊一个文件列表：PATH 是"选个文件"这个通用需求，
 * 系统对话框自带最近目录、网络位置和长路径处理。它只在本 .cpp 内出现，
 * 公共头仍然不含 Win32 类型。
 *
 * @param[in,out] path 进 = 当前值（作为对话框初始路径），出 = 用户选的路径（UTF-8）。
 * @return true = 用户选了文件；false = 取消或对话框失败（保持原值）。
 */
bool browseForFile(std::string& path) {
    wchar_t buffer[1024] = {};
    const std::wstring initial = utf8ToWide(path);
    const std::size_t copied = std::min(initial.size(), std::size(buffer) - 1);
    std::copy_n(initial.begin(), copied, buffer);

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    // 过滤串是"显示名\0通配符\0"序列，末尾再来一个 \0；直接写进宽字面量即可。
    dialog.lpstrFilter = L"所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrTitle = L"选择文件";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog) == FALSE) {
        return false;  // 用户取消不是错误
    }
    path = wideToUtf8(buffer);
    return true;
}

}  // namespace

void SettingsPanel::Impl::renderActionBar() {
    const bool pending = hasPendingChanges();
    const bool canApply = pending && !waitingForPlugins;

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
    ImGui::BeginDisabled(waitingForPlugins);
    if (ImGui::Button("恢复全部默认")) {
        restoreDefaults();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    // 状态文本优先级：等待中 > 有未应用修改 > 上一次操作的结果。
    std::string text = statusText;
    Tone tone = statusTone;
    if (waitingForPlugins) {
        text = "应用中…（等待宿主完成插件启停）";
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
        ImGui::TextDisabled("宿主未提供设置目录服务（shot.host.plugin_config）："
                            "当前宿主不能读插件配置，设置页只剩插件管理。");
    } else if (!sections.empty()) {
        // 目录约定索引 0 固定是宿主自己（id "core"），所以它永远排在最前面。
        renderProviderSection(sections[0]);
    }
    renderPluginTable();
    for (std::size_t index = 1; index < sections.size(); ++index) {
        renderProviderSection(sections[index]);
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
    const shot::ConfigDesc& desc = item.desc;
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
        ImGui::TextColored(kToneError, "插件拒绝：%s", item.error.c_str());
    } else if (item.value != item.baseline) {
        ImGui::SameLine();
        ImGui::TextColored(kToneWarning, "已修改");
    }
    ImGui::PopID();
}

// 返回 true = 这一帧控件改了值（值已经写回 item.value）。
bool SettingsPanel::Impl::renderItemControl(ConfigItem& item) {
    const shot::ConfigDesc& desc = item.desc;
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
                if (browseForFile(chosen)) {
                    item.value = std::move(chosen);
                    changed = true;
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
