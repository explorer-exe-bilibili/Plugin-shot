// 按 hotkey_list 快照注册 / 撤销全局热键（只在热键线程上执行）。
//
// 注册顺序 = 目录快照顺序（HotkeyCatalog 已按 hotkey_list 的顺序整理）。同一个
// 组合键在进程内只能被 RegisterHotKey 一次，所以这里先在自己这批注册里查重：
// 先到者注册，后者 WARN 跳过——不依赖系统错误码 1409，日志里也不会把"自己人
// 冲突"误报成"被其它程序占用"。
//
// 非法值处理：配置里存了非法组合键时回落到列表的默认组合键（combo），默认值也
// 非法才跳过；空串是设置页"清空"的语义，按"这项不注册"处理，不回落默认值。

#include "hotkey_window_impl.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "hotkey_catalog.h"
#include "hotkey_config.h"
#include "shot_plugin_helpers.h"

namespace hotkey {
namespace {

/** @brief Win32 VK_SNAPSHOT（PrintScreen）；裸按在 Win11 上会被系统截图工具先接管。 */
constexpr std::uint32_t kVkSnapshot = 0x2Cu;

/**
 * @brief 把 hotkey::kMod* 位集合转成 Win32 的 MOD_* 组合；额外加 MOD_NOREPEAT。
 * @param[in] modifiers hotkey_binding.h 里的修饰键位。
 * @return RegisterHotKey 的 fsModifiers。
 */
DWORD toWin32Modifiers(std::uint32_t modifiers) {
    DWORD result = 0;
    if ((modifiers & kModCtrl) != 0) {
        result |= MOD_CONTROL;
    }
    if ((modifiers & kModAlt) != 0) {
        result |= MOD_ALT;
    }
    if ((modifiers & kModShift) != 0) {
        result |= MOD_SHIFT;
    }
    if ((modifiers & kModWin) != 0) {
        result |= MOD_WIN;
    }
    // 按住不放只触发一次：全局热键最常见的用法是"按一下执行一个动作"，
    // 自动重复会在长按时连续截屏/连续打开设置。
    result |= MOD_NOREPEAT;
    return result;
}

/**
 * @brief 注册失败的简短中文提示（错误码数值留给排障用）。
 * @param[in] error RegisterHotKey 失败后 GetLastError() 的值。
 * @return 附加说明；没有匹配时为空串。
 */
std::string registerFailureHint(DWORD error) {
    if (error == ERROR_HOTKEY_ALREADY_REGISTERED) {
        return "，该组合键已被其它程序占用";
    }
    if (error == ERROR_ACCESS_DENIED) {
        return "，可能被更高权限的程序占用";
    }
    return "";
}

}  // namespace

void HotkeyWindow::Impl::applyHotkeys() {
    unregisterAllHotkeys();
    if (suspended) {
        // 录入期间配置可能变化（set_config / config.applied 都会走到这里）：只撤销、不注册。
        // 恢复时 setSuspended(false) 会重新读配置注册，所以这里不用缓存"待注册"状态。
        shot::Logger(host).info("[hotkey] 热键处于暂停状态（设置页录入中），跳过注册");
        return;
    }
    if (config == nullptr) {
        shot::Logger(host).warn("[hotkey] 没有配置对象，跳过热键注册");
        return;
    }
    if (!config->enabled()) {
        shot::Logger(host).info("[hotkey] 全局热键已禁用（enabled=0），不注册任何热键");
        return;
    }
    if (catalog == nullptr) {
        shot::Logger(host).warn("[hotkey] 没有热键目录，跳过热键注册（列表驱动未初始化）");
        return;
    }
    const std::vector<HotkeyEntry> entries = catalog->entries();
    if (entries.empty()) {
        shot::Logger(host).warn("[hotkey] hotkey_list 为空，没有可注册的热键");
        return;
    }

    int nextId = 1;
    for (const HotkeyEntry& entry : entries) {
        std::string text = entry.value;
        if (entry.hasValue && text.empty()) {
            // 空串 = 设置页的"清空"语义（这项不设热键），不是错误，也不回落默认值。
            shot::Logger(host).info("[hotkey] 热键 " + entry.id + " 已清空（空串 = 不注册），跳过");
            continue;
        }
        Binding binding;
        std::string error;
        if (!parse_binding(text.c_str(), binding, error)) {
            if (!entry.hasValue) {
                shot::Logger(host).warn("[hotkey] 列表给 " + entry.id + " 的默认组合键 \"" +
                                        text + "\" 不合法（" + error + "），已跳过这一项");
                continue;
            }
            // 配置值非法：回落到列表默认组合键，并保留一条 WARN 让用户知道配置没生效。
            shot::Logger(host).warn("[hotkey] 热键 " + entry.id + " 的配置值 \"" + text +
                                    "\" 不合法（" + error + "），回落到列表默认组合键 \"" +
                                    entry.defaultCombo + "\"");
            text = entry.defaultCombo;
            if (text.empty()) {
                shot::Logger(host).warn("[hotkey] 热键 " + entry.id +
                                        " 没有可用的默认组合键，已跳过这一项");
                continue;
            }
            if (!parse_binding(text.c_str(), binding, error)) {
                shot::Logger(host).warn("[hotkey] 热键 " + entry.id + " 的默认组合键 \"" + text +
                                        "\" 也不合法（" + error + "），已跳过这一项");
                continue;
            }
        }
        // 同一批注册里先到者生效：全局热键是进程级资源，重复注册只会得到 1409，
        // 在这里提前判掉，日志才能区分"列表自己冲突"和"被其它程序占用"。
        const auto duplicate = std::find_if(
            hotkeys.begin(), hotkeys.end(), [&binding](const RegisteredHotkey& slot) {
                return slot.registered && slot.modifiers == binding.modifiers &&
                       slot.virtualKey == binding.virtualKey;
            });
        if (duplicate != hotkeys.end()) {
            shot::Logger(host).warn("[hotkey] 热键 " + entry.id + "（" + binding.label +
                                    "）与先注册的 " + duplicate->action +
                                    " 冲突，按列表顺序先到者生效，跳过这一项");
            continue;
        }
        registerOne(nextId, binding, entry.id, entry.event, entry.data);
        ++nextId;
    }
}

void HotkeyWindow::Impl::unregisterAllHotkeys() {
    for (RegisteredHotkey& slot : hotkeys) {
        if (!slot.registered) {
            continue;
        }
        if (!UnregisterHotKey(hwnd, slot.id)) {
            shot::Logger(host).warn("[hotkey] 撤销热键失败（id=" + std::to_string(slot.id) +
                                    "，错误码 " + std::to_string(GetLastError()) + "）");
        }
    }
    hotkeys.clear();
}

bool HotkeyWindow::Impl::registerOne(int id, const Binding& binding, const std::string& action,
                                     const std::string& topic, const std::string& payload) {
    const DWORD modifiers = toWin32Modifiers(binding.modifiers);
    if (!RegisterHotKey(hwnd, id, modifiers, binding.virtualKey)) {
        const DWORD registerError = GetLastError();
        shot::Logger(host).warn("[hotkey] 热键 " + action + "（" + binding.label +
                                "）注册失败（错误码 " + std::to_string(registerError) +
                                registerFailureHint(registerError) + "），已跳过这一项");
        return false;
    }

    RegisteredHotkey slot;
    slot.id = id;
    slot.registered = true;
    slot.modifiers = binding.modifiers;
    slot.virtualKey = binding.virtualKey;
    slot.label = binding.label;
    slot.action = action;
    slot.topic = topic;
    slot.payload = payload;
    hotkeys.push_back(std::move(slot));

    std::string message = "[hotkey] 已注册热键 " + action + "（" + binding.label + "） → " + topic;
    if (!payload.empty()) {
        message += "（负载 " + payload + "）";
    }
    shot::Logger(host).info(message);

    // Win11 默认把「裸按 PrintScreen」交给系统截图工具（设置→辅助功能→键盘→
    // "使用 Print Screen 键打开截图工具"）。这种情况下 RegisterHotKey 会返回成功，
    // 但按键先被系统消费、WM_HOTKEY 不会到达本插件；带修饰键（Ctrl+PrintScreen 等）
    // 不受影响（本机实测两种行为都复现过）。注册成功后再提示一句，避免用户以为
    // "配了却不生效"是插件的 bug。
    if (binding.virtualKey == kVkSnapshot && binding.modifiers == 0) {
        shot::Logger(host).warn(
            "[hotkey] 注意：裸按 PrintScreen 在 Windows 11 上默认会被系统截图工具接管，"
            "可能收不到 WM_HOTKEY；建议改用 Ctrl+PrintScreen 等组合，或在 Windows 设置 → "
            "辅助功能 → 键盘 关闭「使用 Print Screen 键打开截图工具」");
    }
    return true;
}

void HotkeyWindow::Impl::publishHotkey(int id) {
    const auto it = std::find_if(hotkeys.begin(), hotkeys.end(), [id](const RegisteredHotkey& slot) {
        return slot.id == id && slot.registered;
    });
    if (it == hotkeys.end() || it->topic.empty()) {
        // 注册被跳过/撤销之后才到达的 WM_HOTKEY：忽略，不当作错误。
        return;
    }
    shot::EventBus bus(host);
    if (!bus.publish(it->topic.c_str(), it->payload)) {
        shot::Logger(host).warn("[hotkey] 发布 " + it->topic +
                                " 失败（宿主没有事件总线或队列已满）");
        return;
    }
    // 这条日志是验收锚点："发布 capture.request" 出现即说明热键通路打通。
    std::string message =
        "[hotkey] 发布 " + it->topic + "（" + it->action + " 热键 " + it->label + "）";
    if (!it->payload.empty()) {
        message += "，负载 " + it->payload;
    }
    shot::Logger(host).info(message);
}

}  // namespace hotkey
