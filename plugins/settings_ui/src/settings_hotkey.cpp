// settings_hotkey —— 设置页的"按组合键录入"控件：STRING 且 options == "hotkey"。
//
// 为什么单独一个文件：热键录入是一个独立关注点（键盘扫描 / 规范化 / 与 hotkey 插件的
// 暂停-恢复握手），塞进 settings_form.cpp 会让那个文件同时负责"7 种 kind 的表单"和
// "键盘采集状态机"（STYLE.md §6.8）。
//
// 交互（用户视角说明在 document/SETTINGS.md §6）：
//   · 行内显示当前组合键 + 「录入…」按钮；
//   · 点「录入…」后弹一个模态弹窗，由 ImGui 独占键盘，其它控件（含键盘导航）不响应；
//   · 按下 [Ctrl+][Alt+][Shift+][Win+]<A-Z|0-9|F1..F12|PrintScreen> 就把规范化文本写回 item.value，
//     仍然走"本地编辑 + 应用"的流程（不在这里写宿主）；
//   · Esc 取消（值不变），Backspace / Delete 清空（存空串 = 该动作不设热键）；
//   · 录入期间发布 hotkey.suspend / hotkey.resume，让 hotkey 插件撤掉全局注册——
//     否则按下"当前已注册的组合键"会先触发真实动作（比如立刻截一张图）。
//
// 值语法与合法性完全归 hotkey 插件（plugins/hotkey/include/hotkey_binding.h）：这里只
// 产出规范化文本，不重复实现解析器，避免两份语法各自漂移；旧配置里的非法值原样显示，
// 由 hotkey 插件在注册时 warn 跳过。
//
// 线程模型：只在设置页的 UI 线程上执行。事件总线回调不在这里（设置页的事件都先入队，
// 再在 UI 线程 drain），所以 hotkeyCapture 不需要锁。

#include <cstddef>
#include <iterator>
#include <string>

#include "imgui.h"

#include "settings_panel_impl.h"
#include "shot_plugin_helpers.h"

namespace settings_ui {
namespace {

/** @brief 设置页发布的主题：开始录入时请求 hotkey 插件撤掉全部全局注册。 */
constexpr char kTopicHotkeySuspend[] = "hotkey.suspend";

/** @brief 设置页发布的主题：结束录入时请求 hotkey 插件按当前配置恢复注册。 */
constexpr char kTopicHotkeyResume[] = "hotkey.resume";

/** @brief 录入弹窗的 ImGui id；弹窗是独立窗口，不继承配置行的 id 栈。 */
constexpr char kHotkeyCapturePopup[] = "##hotkey_capture";

// 可录入的主键：与 hotkey_binding.h 的语法一一对应。
// ImGuiKey_A..Z / 0..9 / F1..F12 在 imgui.h 里是连续枚举，但这里仍然显式列出：
// 录的是"写进配置的键名"，和 ImGui 的枚举顺序脱钩，以后升级 ImGui 也不用重新推导。
// PrintScreen 不在这段连续区间里，单独列在 F12 后面。
constexpr ImGuiKey kPrimaryKeys[] = {
    ImGuiKey_A, ImGuiKey_B, ImGuiKey_C, ImGuiKey_D, ImGuiKey_E, ImGuiKey_F, ImGuiKey_G,
    ImGuiKey_H, ImGuiKey_I, ImGuiKey_J, ImGuiKey_K, ImGuiKey_L, ImGuiKey_M, ImGuiKey_N,
    ImGuiKey_O, ImGuiKey_P, ImGuiKey_Q, ImGuiKey_R, ImGuiKey_S, ImGuiKey_T, ImGuiKey_U,
    ImGuiKey_V, ImGuiKey_W, ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z,
    ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6,
    ImGuiKey_7, ImGuiKey_8, ImGuiKey_9,
    // 小键盘数字按同一个数字录入：hotkey 语法里没有"小键盘"这个概念，用户只是想按数字。
    ImGuiKey_Keypad0, ImGuiKey_Keypad1, ImGuiKey_Keypad2, ImGuiKey_Keypad3, ImGuiKey_Keypad4,
    ImGuiKey_Keypad5, ImGuiKey_Keypad6, ImGuiKey_Keypad7, ImGuiKey_Keypad8, ImGuiKey_Keypad9,
    ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6, ImGuiKey_F7,
    ImGuiKey_F8, ImGuiKey_F9, ImGuiKey_F10, ImGuiKey_F11, ImGuiKey_F12,
    // PrintScreen 是独立按键（Win32 VK_SNAPSHOT）；hotkey 插件的语法同样接受它，
    // 常见拼写（Print Screen / PrtSc …）由解析器统一规范化成 "PrintScreen"。
    ImGuiKey_PrintScreen,
};

/** @brief 与 kPrimaryKeys 同下标的配置文本（"A" / "7" / "F12"）。 */
constexpr const char* kPrimaryTexts[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R",
    "S", "T", "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "PrintScreen",
};

static_assert(std::size(kPrimaryKeys) == std::size(kPrimaryTexts),
              "主键表与文本表必须一一对应");

/**
 * @brief 判断 ImGui 的一个键是不是修饰键。
 *
 * 修饰键单独按下不算"录入了一个组合键"（要等主键），也不该被报成"不支持的按键"。
 * 只服务 scanHotkeyPress，与任何类状态无关，所以留在匿名命名空间。
 *
 * @param[in] key ImGui 键值。
 * @return true = Ctrl / Alt / Shift / Super 的左右键或其保留位。
 */
bool isModifierKey(ImGuiKey key) {
    return key == ImGuiKey_LeftCtrl || key == ImGuiKey_LeftShift ||
           key == ImGuiKey_LeftAlt || key == ImGuiKey_LeftSuper ||
           key == ImGuiKey_RightCtrl || key == ImGuiKey_RightShift ||
           key == ImGuiKey_RightAlt || key == ImGuiKey_RightSuper ||
           key == ImGuiKey_ReservedForModCtrl || key == ImGuiKey_ReservedForModShift ||
           key == ImGuiKey_ReservedForModAlt || key == ImGuiKey_ReservedForModSuper;
}

/**
 * @brief 按 hotkey_binding.h 的规范化顺序拼出组合键文本。
 *
 * 顺序固定为 Ctrl → Alt → Shift → Win；与 hotkey 插件的 buildLabel 一致，
 * 这样"设置页写进去的值"和"热键插件日志里的 label"是同一个字符串，排障时不用心算。
 *
 * @param[in] ctrl  Ctrl 是否按下。
 * @param[in] alt   Alt 是否按下。
 * @param[in] shift Shift 是否按下。
 * @param[in] super Win 是否按下。
 * @param[in] main  主键文本（"A" / "7" / "F12"）。
 * @return 如 "Ctrl+Alt+S"；没有修饰键时就是 main 本身。
 */
std::string buildHotkeyLabel(bool ctrl, bool alt, bool shift, bool super, const std::string& main) {
    std::string label;
    if (ctrl) {
        label += "Ctrl+";
    }
    if (alt) {
        label += "Alt+";
    }
    if (shift) {
        label += "Shift+";
    }
    if (super) {
        label += "Win+";
    }
    label += main;
    return label;
}

}  // namespace

bool SettingsPanel::Impl::isHotkeyDesc(const ConfigDesc& desc) {
    // 与 PATH 的 pathPickerFor 同一套约定：options 是设置页解释的字符串，
    // 大小写不敏感（插件写 "Hotkey" 也认）。为空串 / 别的值 = 普通 STRING。
    if (desc.kind != SHOT_CONFIG_KIND_STRING) {
        return false;
    }
    std::string lower;
    lower.reserve(desc.options.size());
    for (const char c : desc.options) {
        lower.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
    }
    return lower == "hotkey";
}

bool SettingsPanel::Impl::scanHotkeyPress(std::string& label, bool& hasModifier,
                                         bool& unsupported) {
    unsupported = false;
    const ImGuiIO& io = ImGui::GetIO();
    for (std::size_t index = 0; index < std::size(kPrimaryKeys); ++index) {
        // repeat=false：按住不放时只认第一次按下（与热键语义"按一下触发一次"一致）。
        if (!ImGui::IsKeyPressed(kPrimaryKeys[index], false)) {
            continue;
        }
        hasModifier = io.KeyCtrl || io.KeyAlt || io.KeyShift || io.KeySuper;
        label = buildHotkeyLabel(io.KeyCtrl, io.KeyAlt, io.KeyShift, io.KeySuper,
                                 kPrimaryTexts[index]);
        return true;
    }
    // 没有可录入的主键：如果按的是 Tab / 方向键 / 空格这类键，告诉用户"不支持"，
    // 否则弹窗看起来像没反应。Esc / Backspace / Delete 在调用方已经先处理掉了。
    unsupported = false;
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        const ImGuiKey candidate = static_cast<ImGuiKey>(key);
        if (candidate == ImGuiKey_Escape || candidate == ImGuiKey_Backspace ||
            candidate == ImGuiKey_Delete || isModifierKey(candidate)) {
            continue;
        }
        if (ImGui::IsKeyPressed(candidate, false)) {
            unsupported = true;
            break;
        }
    }
    return false;
}

void SettingsPanel::Impl::beginHotkeyCapture(const std::string& owner, const std::string& key) {
    if (hotkeyCapture.active) {
        endHotkeyCapture();  // 防御：弹窗是模态的，正常不会出现两个会话
    }
    hotkeyCapture.active = true;
    hotkeyCapture.owner = owner;
    hotkeyCapture.key = key;
    hotkeyCapture.hint.clear();

    // 先让 hotkey 插件撤掉全局注册，再等用户按键：否则按下"当前生效的组合键"会先
    // 触发真实动作。总线不可用的旧宿主上只能记录 WARN——录入本身仍然可用。
    if (!publishEvent(host, kTopicHotkeySuspend, {})) {
        shot::Logger(host).warn(
            "settings_ui：发布 hotkey.suspend 失败，录入期间按到已注册的热键可能仍会触发动作");
    }
}

void SettingsPanel::Impl::endHotkeyCapture() {
    if (!hotkeyCapture.active) {
        return;
    }
    hotkeyCapture.active = false;
    hotkeyCapture.owner.clear();
    hotkeyCapture.key.clear();
    hotkeyCapture.hint.clear();
    // 无论上次 suspend 有没有真的送到 hotkey 插件，resume 都是幂等的"按当前配置重注册"；
    // 漏发会让热键永久停在暂停状态，所以这里一定要发。
    if (!publishEvent(host, kTopicHotkeyResume, {})) {
        shot::Logger(host).warn(
            "settings_ui：发布 hotkey.resume 失败，热键插件可能在下一次配置应用/重启前保持暂停");
    }
}

void SettingsPanel::Impl::renderHotkeyControl(ConfigItem& item, const std::string& owner) {
    const bool capturing = hotkeyCapture.active && hotkeyCapture.owner == owner &&
                           hotkeyCapture.key == item.desc.key;
    // 空值不是错误：hotkey 插件解析空串时按"热键为空"warn 跳过，语义是"这个动作没有热键"。
    if (item.value.empty()) {
        ImGui::TextDisabled("（未设置）");
    } else {
        ImGui::TextUnformatted(item.value.c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(hotkeyCapture.active);
    if (ImGui::Button("录入…")) {
        beginHotkeyCapture(owner, item.desc.key);
    }
    ImGui::EndDisabled();
    if (capturing) {
        ImGui::SameLine();
        ImGui::TextDisabled("等待按键…");
    }
}

void SettingsPanel::Impl::renderHotkeyCapturePopup() {
    if (hotkeyCapture.active && !ImGui::IsPopupOpen(kHotkeyCapturePopup)) {
        // 每帧检查：弹窗被 ImGui 意外关闭（例如同一帧里窗口尺寸为 0 不渲染）时也能恢复。
        ImGui::OpenPopup(kHotkeyCapturePopup);
    }
    // NoNav：不让键盘导航把焦点放到"取消"按钮上——录入时按空格/回车是要被扫描的按键，
    // 不能被当成"激活焦点按钮"（那样会误取消录入）。鼠标点按钮仍然有效。
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoNav;
    if (!ImGui::BeginPopupModal(kHotkeyCapturePopup, nullptr, flags)) {
        return;
    }
    if (!hotkeyCapture.active) {
        // 会话在窗口隐藏 / 宿主退出时被取消，但弹窗栈里还留着 id：先关掉，下次才能重新打开。
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    bool closed = false;
    ImGui::TextUnformatted("按下新的组合键");
    ImGui::TextDisabled("Ctrl / Alt / Shift / Win + 字母、数字、F1-F12 或 PrintScreen");
    ImGui::Spacing();
    if (hotkeyCapture.hint.empty()) {
        ImGui::TextDisabled("Esc 取消；Backspace / Delete 清空（该动作不再有热键）");
    } else {
        ImGui::TextColored(kToneWarning, "%s", hotkeyCapture.hint.c_str());
    }
    ImGui::Separator();
    ConfigItem* item = findConfigItem(hotkeyCapture.owner, hotkeyCapture.key);
    const std::string current = (item != nullptr) ? item->value : std::string();
    ImGui::Text("当前组合键：%s", current.empty() ? "（未设置）" : current.c_str());
    ImGui::Separator();

    if (ImGui::Button("取消")) {
        closed = true;
    }
    // Esc 只取消录入，不隐藏窗口（窗口层的 Esc 处理会先看 isCapturingHotkey）。
    if (!closed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        closed = true;
    }
    // 清空 = 存空串：hotkey 插件解析时按"热键为空"warn 跳过，等于给这个动作去掉热键。
    if (!closed && (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
        if (item != nullptr) {
            item->value.clear();
            item->error.clear();
            item->hint.clear();
        }
        shot::Logger(host).info("settings_ui：清空热键 " + hotkeyCapture.owner + "/" +
                                hotkeyCapture.key + "（空值 = 不注册该热键）");
        closed = true;
    }
    if (!closed) {
        std::string label;
        bool hasModifier = false;
        bool unsupported = false;
        if (scanHotkeyPress(label, hasModifier, unsupported)) {
            if (item != nullptr) {
                item->value = label;
                item->error.clear();
                // 裸键（没有修饰键）语法合法，但会拦截全局输入：不擅自改用户的选择，只提示。
                item->hint = hasModifier ? std::string()
                                         : std::string("提示：没有修饰键的热键会拦截正常输入");
            }
            shot::Logger(host).info("settings_ui：录入热键 " + hotkeyCapture.owner + "/" +
                                    hotkeyCapture.key + " = " + label);
            closed = true;
        } else if (unsupported) {
            hotkeyCapture.hint =
                "不支持的按键：只支持 A-Z / 0-9 / F1-F12 / PrintScreen（可加 Ctrl/Alt/Shift/Win）";
        }
    }
    if (closed) {
        endHotkeyCapture();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace settings_ui
