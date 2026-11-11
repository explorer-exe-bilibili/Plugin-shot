// settings_color —— 设置页的"颜色"控件：STRING 且 options 以 '#' 开头。
//
// 为什么用 STRING + options 而不是新增 kind：数据形状不进 ABI，options 本来就是
// "给设置页的交互提示"（hotkey / directory 都这么用）。插件只要在描述符里写
// "#4C9AFF|#34C759|…"，设置页就渲染成预设色块；老宿主仍把同一份值当普通文本框
// 显示，配置本身不需要迁移，也不需要涨 ABI。目前的使用者是宿主设置里的
// core/theme_color（声明在 src/hostConfigurable.cpp）。
//
// 交互（用户视角说明在 plugins/settings_ui/README.md §4.2）：
//   · 一行预设色块（options 按 | 切，只保留合法的 #RRGGBB），点击即选中；
//     当前值与某个预设相同时，色块画外边框 + 勾选标记；
//   · 「自定义…」打开非模态取色窗（ColorPicker3），拖动时实时写回本地缓冲；
//     点窗外 / 「确定」保留实时预览，「取消」回滚到打开弹窗时的值；
//   · 旁边的十六进制输入框只认 #RRGGBB（大小写不敏感）：输入暂时非法时不提交、
//     不标红，用户能继续编辑到合法为止；
//   · 所有改动只写本地 ConfigItem，仍然要点「应用」才走 lists.update / commit
//     （提交路径与其它控件共用 settings_apply.cpp，不在这里绕过）。
//
// 尺寸：色块边长、边框粗细都按 ImGui::GetFrameHeight() 的比例取，行高已经随
// content scale 缩放（settings_graphics.cpp 的 ScaleAllSizes / FontScaleMain），
// 所以这里不写死像素，100% / 150% / 200% 下视觉比例一致。
//
// 线程模型：只在设置页 UI 线程上执行；取色会话与热键录入一样只用字符串定位
// 目标项（lists.changed 可能随时重建模型，ConfigItem* 会失效）。

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "imgui.h"
// ImGui::InputText(const char*, std::string*) 的 std::string 重载在 misc/cpp 里：
// 少了这个头就只能匹配 char* 版本，编译期报"函数不接受 2 个参数"。
#include "imgui_stdlib.h"

#include "settings_panel_impl.h"

namespace settings_ui {
namespace {

/** @brief 自定义取色弹窗的 ImGui id；在顶层窗口打开，不继承配置行的 id 栈。 */
constexpr char kColorCustomPopup[] = "##color_custom";

/** @brief 色块边长 / 行高的逻辑比例；行高本身已按 DPI 缩放。 */
constexpr float kSwatchSizeRatio = 1.15f;

/** @brief 选中边框与勾选线的粗细 / 行高的逻辑比例。 */
constexpr float kSelectionLineRatio = 0.09f;

/** @brief 颜色相等的容差：8 位分量换算成 0..1 后允许半个色阶。 */
constexpr float kColorEpsilon = 0.5f / 255.0f;

/**
 * @brief 判断两个 0..1 RGB 是否可视为同一种颜色。
 *
 * 预设色经过 8 位量化，取色器拖动后的浮点值可能与预设只差几个最低位；用半个
 * 色阶做容差，既能把"取色器恰好调成预设色"标成选中，又不会把相邻的预设认错。
 *
 * @param[in] left  第一个颜色。
 * @param[in] right 第二个颜色。
 * @return true = 三个分量都在容差内。
 */
bool sameColor(const float left[3], const float right[3]) {
    return std::fabs(left[0] - right[0]) <= kColorEpsilon &&
           std::fabs(left[1] - right[1]) <= kColorEpsilon &&
           std::fabs(left[2] - right[2]) <= kColorEpsilon;
}

/**
 * @brief 在色块上画选中标记：外边框 + 勾选线。
 *
 * 勾选线用黑/白两色按预设色亮度选，保证浅色（#34C759）与深色（#8E8E93）上都看得见；
 * 只画在 ColorButton 已经占用的矩形内，不改变布局。
 *
 * @param[in] rect         色块的屏幕矩形（GetItemRectMin/Max）。
 * @param[in] color        预设色（0..1 RGB），用来决定勾选线颜色。
 * @param[in] lineThickness 线宽（已按 DPI 缩放）。
 */
void drawSelectedSwatch(const ImVec2& rectMin, const ImVec2& rectMax, const float color[3],
                        float lineThickness) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float width = rectMax.x - rectMin.x;
    const float height = rectMax.y - rectMin.y;
    const ImU32 borderColor = ImGui::GetColorU32(ImGuiCol_Text);
    drawList->AddRect(rectMin, rectMax, borderColor, 0.0f, ImDrawFlags_None, lineThickness);
    const float luma = color[0] * 0.299f + color[1] * 0.587f + color[2] * 0.114f;
    const ImU32 checkColor =
        (luma > 0.55f) ? IM_COL32(0, 0, 0, 230) : IM_COL32(255, 255, 255, 245);
    const ImVec2 first(rectMin.x + width * 0.22f, rectMin.y + height * 0.55f);
    const ImVec2 middle(rectMin.x + width * 0.43f, rectMin.y + height * 0.76f);
    const ImVec2 last(rectMin.x + width * 0.82f, rectMin.y + height * 0.26f);
    drawList->AddLine(first, middle, checkColor, lineThickness);
    drawList->AddLine(middle, last, checkColor, lineThickness);
}

}  // namespace

bool SettingsPanel::Impl::isColorStringDesc(const ConfigDesc& desc) {
    // options 的语义由设置页解释（document/LISTS.md §5.1）：以 '#' 开头的 STRING
    // 就是"颜色"——后面是 | 分隔的预设值。空 options / 其它值仍是普通文本框。
    return desc.kind == SHOT_CONFIG_KIND_STRING && !desc.options.empty() &&
           desc.options.front() == '#';
}

bool SettingsPanel::Impl::renderColorControl(ConfigItem& item, const std::string& owner) {
    const ConfigDesc& desc = item.desc;
    bool changed = false;
    float current[3] = {0.0f, 0.0f, 0.0f};
    const bool valueValid = parseRgbColorValue(item.value, current);

    // 色块尺寸不写死像素：行高由 ImGui 按当前 content scale 算好，这里只定比例。
    const float frameHeight = ImGui::GetFrameHeight();
    const float swatchSize = frameHeight * kSwatchSizeRatio;
    const float lineThickness = frameHeight * kSelectionLineRatio;
    const std::vector<std::string> presets = splitOptions(desc.options);
    for (std::size_t index = 0; index < presets.size(); ++index) {
        float preset[3] = {0.0f, 0.0f, 0.0f};
        if (!parseRgbColorValue(presets[index], preset)) {
            continue;  // options 里混进非颜色项时跳过，不影响其余预设
        }
        ImGui::PushID(static_cast<int>(index));
        const ImVec4 swatchColor(preset[0], preset[1], preset[2], 1.0f);
        const bool clicked = ImGui::ColorButton(
            "##swatch", swatchColor,
            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
            ImVec2(swatchSize, swatchSize));
        const ImVec2 rectMin = ImGui::GetItemRectMin();
        const ImVec2 rectMax = ImGui::GetItemRectMax();
        if (clicked) {
            item.value = formatRgbHex(preset);
            item.colorHex = item.value;
            changed = true;
        } else if (valueValid && sameColor(current, preset)) {
            drawSelectedSwatch(rectMin, rectMax, preset, lineThickness);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", presets[index].c_str());
        }
        ImGui::PopID();
        ImGui::SameLine();
    }

    if (ImGui::Button(tr("settings_ui.color_custom", "自定义…"))) {
        beginColorCustom(owner, desc.key);
    }
    ImGui::SameLine();

    // 十六进制输入框跟着字号走：宽度按 "#RRGGBB" 的文本宽度算，不写死像素。
    const ImGuiStyle& style = ImGui::GetStyle();
    const float hexWidth =
        ImGui::CalcTextSize("#RRGGBB").x + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(hexWidth);
    if (ImGui::InputText("##hex", &item.colorHex)) {
        float parsed[3] = {0.0f, 0.0f, 0.0f};
        if (parseRgbColorValue(item.colorHex, parsed)) {
            // 合法输入立刻同步到 item.value（预览 / 脏标记都靠它）；输入框里的
            // 原文保留，避免用户还没输完就被改写、光标跳动。非法输入只留在缓冲里。
            item.value = formatRgbHex(parsed);
            changed = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          tr("settings_ui.color_hex_hint", "#RRGGBB，例如 #4C9AFF"));
    }
    return changed;
}

void SettingsPanel::Impl::beginColorCustom(const std::string& owner, const std::string& key) {
    // 同一条目已经打开：保留原会话（否则 originalValue 会被实时预览覆盖，取消回不去）。
    if (colorCustom.active && colorCustom.owner == owner && colorCustom.key == key) {
        return;
    }
    colorCustom.active = true;
    colorCustom.requestOpen = true;
    colorCustom.owner = owner;
    colorCustom.key = key;
    if (const ConfigItem* item = findConfigItem(owner, key)) {
        colorCustom.originalValue = item->value;
    } else {
        colorCustom.originalValue.clear();
    }
}

void SettingsPanel::Impl::endColorCustom(bool revert) {
    if (!colorCustom.active) {
        return;
    }
    if (revert) {
        // 取消：把实时预览写回的值恢复成打开弹窗时的值；模型刷新后目标项可能
        // 已经消失（插件被热卸载），那就什么都不用恢复。
        if (ConfigItem* item = findConfigItem(colorCustom.owner, colorCustom.key)) {
            float color[3] = {0.0f, 0.0f, 0.0f};
            item->value = colorCustom.originalValue;
            item->colorHex = parseRgbColorValue(item->value, color)
                                 ? formatRgbHex(color)
                                 : item->value;
        }
    }
    colorCustom = ColorCustomEdit{};
}

void SettingsPanel::Impl::renderColorCustomPopup() {
    if (colorCustom.requestOpen) {
        ImGui::OpenPopup(kColorCustomPopup);
        colorCustom.requestOpen = false;
    }
    if (!ImGui::BeginPopup(kColorCustomPopup)) {
        // 点窗外 / Esc 会让 ImGui 自己关掉弹窗：实时预览保留（等价于「确定」），
        // 只结束会话；要回滚请用弹窗里的「取消」。
        if (colorCustom.active) {
            endColorCustom(false);
        }
        return;
    }
    if (!colorCustom.active) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ConfigItem* item = findConfigItem(colorCustom.owner, colorCustom.key);
    if (item == nullptr) {
        // 目标项随插件卸载消失：没有可以继续编辑的值，关掉会话。
        endColorCustom(false);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextUnformatted(tr("settings_ui.color_pick_title", "自定义颜色"));
    float color[3] = {0.0f, 0.0f, 0.0f};
    if (!parseRgbColorValue(item->value, color) &&
        !parseRgbColorValue(item->desc.defaultValue, color)) {
        color[0] = 0.0f;
        color[1] = 0.0f;
        color[2] = 0.0f;  // 坏值且没有可用默认值：从黑色起手，用户自己调
    }
    if (ImGui::ColorPicker3("##picker", color,
                            ImGuiColorEditFlags_PickerHueWheel |
                                ImGuiColorEditFlags_NoAlpha)) {
        // 实时预览：只改本地编辑缓冲；右键「应用」才提交给宿主。
        item->value = formatRgbHex(color);
        item->colorHex = item->value;
    }
    ImGui::Separator();
    if (ImGui::Button(tr("settings_ui.color_custom_done", "确定"))) {
        endColorCustom(false);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("settings_ui.cancel", "取消"))) {
        endColorCustom(true);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::EndPopup();
}

}  // namespace settings_ui
