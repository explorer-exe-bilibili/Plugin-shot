// 按 hotkey_list 快照抓取 / 撤销 X11 全局热键，并把按键翻译成事件（只在热键线程上执行）。
//
// 策略与 Windows 实现（hotkey_window_register.cpp）保持一致：注册顺序 = 目录快照
// 顺序；配置值非法回落到条目的默认组合键，默认值也非法才跳过；空串 = 设置页
// "清空"语义（这项不注册，不回落默认值）；同一批里先到者生效、后者 WARN 跳过。
// 平台差异只有"怎么抓"与"怎么匹配"：
//
//   · 修饰符映射：Ctrl→ControlMask、Alt→Mod1Mask、Shift→ShiftMask、Win→Mod4Mask；
//   · X 服务器按**精确的修饰位组合**匹配被动抓取。CapsLock/NumLock/ScrollLock 开着
//     时事件 state 会多出锁位，只抓 base 组合就会"按了没反应"，所以每个组合额外
//     抓 2^n 个锁变体（LockMask / NumLock 位 / ScrollLock 位的全部组合）；
//   · 匹配时只比较本插件支持的四个修饰位（state 里可能还有锁位与鼠标键位），
//     锁位在抓取阶段已经用变体覆盖，语义上不该参与匹配；
//   · 冲突（BadAccess）由注册期临时安装的 X 错误处理器捕获：只 WARN 跳过这一项，
//     不崩溃、也不吞掉其它 X 错误（其它错误原样转发给原来的处理器）。
//
// 主键：hotkey_binding 解析出的 Win32 VK → keysym → XKeysymToKeycode（见 lookupKeycode）。

#include "hotkey_window_x11_impl.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include "hotkey_catalog.h"
#include "hotkey_config.h"
#include "shot_plugin_helpers.h"

namespace hotkey {
namespace {

/** @brief Win32 VK 数值：F1 = VK_F1 = 0x70，F12 = 0x7B（hotkey_binding 的解析结果）。 */
constexpr std::uint32_t kVkF1 = 0x70u;
/** @brief Win32 VK_SNAPSHOT（PrintScreen）= 0x2C。 */
constexpr std::uint32_t kVkSnapshot = 0x2Cu;

/** @brief 事件匹配时只比较本插件支持的四个修饰位（Ctrl / Alt / Shift / Win）。 */
constexpr unsigned int kSignificantModifiers = ControlMask | Mod1Mask | ShiftMask | Mod4Mask;

/**
 * @brief 把 hotkey::kMod* 位集合换成 X11 修饰位。
 * @param[in] modifiers hotkey_binding.h 里的修饰键位。
 * @return XGrabKey 用的基准修饰位（不含锁变体）。
 */
unsigned int toX11Modifiers(std::uint32_t modifiers) {
    unsigned int result = 0;
    if ((modifiers & kModCtrl) != 0) {
        result |= ControlMask;
    }
    if ((modifiers & kModAlt) != 0) {
        result |= Mod1Mask;
    }
    if ((modifiers & kModShift) != 0) {
        result |= ShiftMask;
    }
    if ((modifiers & kModWin) != 0) {
        result |= Mod4Mask;
    }
    return result;
}

/**
 * @brief 把解析结果的 Win32 VK 换成 X11 keysym（换不了时返回 0）。
 *
 * 纯映射、与类状态无关，所以留在匿名命名空间（STYLE.md §6.7 的合法例外）。
 *
 * @param[in] virtualKey hotkey_binding 解析出的 Win32 VK 数值。
 * @return 对应 keysym；本平台不支持的主键返回 0。
 */
KeySym toKeysym(std::uint32_t virtualKey) {
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        // X11 的字母 keysym 小写连续排列；XKeysymToKeycode 对大小写返回同一个 keycode。
        return static_cast<KeySym>(XK_a + (virtualKey - 'A'));
    }
    if (virtualKey >= '0' && virtualKey <= '9') {
        return static_cast<KeySym>(XK_0 + (virtualKey - '0'));
    }
    if (virtualKey >= kVkF1 && virtualKey < kVkF1 + 12) {
        return static_cast<KeySym>(XK_F1 + (virtualKey - kVkF1));
    }
    if (virtualKey == kVkSnapshot) {
        return XK_Print;
    }
    return 0;
}

/**
 * @brief VK → keycode：先按 keysym 换，找不到时再试一次大写字母 keysym。
 * @param[in] display 本线程的 X11 连接（必须与抓取在同一个线程上使用）。
 * @param[in] binding 解析结果。
 * @return keycode；0 = 当前键盘映射里没有这个键。
 * @note 非 US 布局按"能产生该字符的键"定位 keycode（keysym 语义），与 Windows 的
 *       物理 VK 语义不完全等价；未在非 US 布局上验证。
 */
unsigned int lookupKeycode(Display* display, const Binding& binding) {
    const KeySym keysym = toKeysym(binding.virtualKey);
    if (keysym == 0) {
        return 0;
    }
    KeyCode keycode = XKeysymToKeycode(display, keysym);
    if (keycode == 0 && keysym >= XK_a && keysym <= XK_z) {
        // 罕见布局只把大写字母放在第一层：回落一次大写 keysym。
        keycode = XKeysymToKeycode(display, static_cast<KeySym>(keysym - XK_a + XK_A));
    }
    return keycode;
}

// ---- 注册期的临时 X 错误处理器 ---------------------------------------------

/**
 * @brief 注册窗口期内是否见过 BadAccess（= 组合键已被别的 client 抓走）。
 *
 * X 错误处理器没有 user 指针，只能是进程级函数，状态也只能放文件作用域。竞态窗口
 * 只有 grabHotkey 里的两次 XSync：窗口期内别的组件若同时触发 BadAccess，会被误算成
 * "本热键冲突"——结果是多一条 WARN、跳过这一项，不会崩，也不影响其它热键。
 */
std::atomic<bool> g_grabBadAccess{false};

/** @brief 临时处理器之前装着的处理器（通常是 Xlib 默认的 _XDefaultError，非空）。 */
XErrorHandler g_previousErrorHandler = nullptr;

/**
 * @brief 注册期临时接管的 X 错误处理器：BadAccess 记成冲突，其它错误原样转发。
 *
 * XSetErrorHandler 是**进程级**设置，所以只在 XGrabKey/XSync 这几行周围临时接管，
 * 结束立刻装回原处理器——绝不能把其它组件（GLFW/settings_ui）的错误处理吞掉。
 *
 * @param[in] display 出错的连接。
 * @param[in] event   错误详情；可能为空。
 * @return 0 = 已处理（Xlib 不再按默认方式打印/退出）。
 */
int grabErrorHandler(Display* display, XErrorEvent* event) {
    if (event != nullptr && event->error_code == BadAccess) {
        g_grabBadAccess.store(true);
        return 0;
    }
    if (g_previousErrorHandler != nullptr) {
        return g_previousErrorHandler(display, event);
    }
    return 0;
}

}  // namespace

std::vector<unsigned int> HotkeyWindow::Impl::lockVariants() const {
    // 每个锁修饰位有"开/关"两种状态，组合数是 2^n：先放 0，再逐个把已有组合补上
    // 这一位，得到 base、base|Lock、base|NumLock、base|Lock|NumLock … 的全部组合。
    std::vector<unsigned int> variants;
    variants.push_back(0);
    for (const unsigned int lockMask : lockMasks) {
        const std::size_t current = variants.size();
        for (std::size_t index = 0; index < current; ++index) {
            variants.push_back(variants[index] | lockMask);
        }
    }
    return variants;
}

void HotkeyWindow::Impl::applyHotkeys() {
    ungrabAllHotkeys();
    if (suspended) {
        // 录入期间配置可能变化（set_config / config.applied 都会走到这里）：只撤销、不抓取。
        // 恢复时 setSuspended(false) 会重新读配置抓取，所以这里不用缓存"待注册"状态。
        shot::Logger(host).info("[hotkey] 热键处于暂停状态（设置页录入中），跳过注册");
        return;
    }
    if (display == nullptr) {
        // 纯 Wayland / 没有 X11 连接：start() 已经打过降级 WARN，这里只记 DEBUG，
        // 避免每次配置变化都刷一条"注册失败"。
        shot::Logger(host).debug("[hotkey] 没有 X11 连接（Wayland 降级），跳过全局热键注册");
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
                shot::Logger(host).warn("[hotkey] 列表给 " + entry.id + " 的默认组合键 \"" + text +
                                        "\" 不合法（" + error + "），已跳过这一项");
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
        const unsigned int keycode = lookupKeycode(display, binding);
        if (keycode == 0) {
            shot::Logger(host).warn("[hotkey] 热键 " + entry.id + "（" + binding.label +
                                    "）在当前 X11 键盘映射里找不到对应按键，已跳过这一项");
            continue;
        }
        const unsigned int modifiers = toX11Modifiers(binding.modifiers);
        // 同一批注册里先到者生效：X 的被动抓取是进程级资源，重复抓同一组合不会报错
        // （同一 client 重复抓是幂等的），所以只能在这里按 keycode+修饰位提前判掉，
        // 日志才能区分"列表自己冲突"和"被其它程序占用"。
        const auto duplicate = std::find_if(
            hotkeys.begin(), hotkeys.end(), [keycode, modifiers](const X11Hotkey& slot) {
                return slot.keycode == keycode && slot.modifiers == modifiers;
            });
        if (duplicate != hotkeys.end()) {
            shot::Logger(host).warn("[hotkey] 热键 " + entry.id + "（" + binding.label +
                                    "）与先注册的 " + duplicate->action +
                                    " 冲突，按列表顺序先到者生效，跳过这一项");
            continue;
        }
        grabHotkey(binding, keycode, entry.id, entry.event, entry.data);
    }
}

void HotkeyWindow::Impl::ungrabAllHotkeys() {
    if (display == nullptr) {
        hotkeys.clear();
        return;
    }
    const std::vector<unsigned int> variants = lockVariants();
    for (const X11Hotkey& slot : hotkeys) {
        for (const unsigned int lockMask : variants) {
            XUngrabKey(display, slot.keycode, slot.modifiers | lockMask, root);
        }
    }
    XSync(display, False);
    hotkeys.clear();
}

bool HotkeyWindow::Impl::grabHotkey(const Binding& binding, unsigned int keycode,
                                    const std::string& action, const std::string& topic,
                                    const std::string& payload) {
    const unsigned int modifiers = toX11Modifiers(binding.modifiers);
    const std::vector<unsigned int> variants = lockVariants();

    // 先 ungrab 再 grab：禁用→启用或上一次注册残留时，同一组合可能已经有本进程的
    // 旧抓取；先撤掉能保证"抓取成功"一定是这一次注册的结果，也不会越叠越多。
    g_grabBadAccess.store(false);
    const XErrorHandler previous = XSetErrorHandler(&grabErrorHandler);
    g_previousErrorHandler = previous;
    for (const unsigned int lockMask : variants) {
        XUngrabKey(display, keycode, modifiers | lockMask, root);
    }
    // X 错误是异步到达的：先 XSync 把上面 ungrab 的错误（如果有）放掉，再从干净的
    // 标志开始记 grab 的结果，避免把 ungrab 的错误算到这一次抓取头上。
    XSync(display, False);
    g_grabBadAccess.store(false);
    for (const unsigned int lockMask : variants) {
        XGrabKey(display, keycode, modifiers | lockMask, root, False, GrabModeAsync,
                 GrabModeAsync);
    }
    XSync(display, False);  // 让 BadAccess 在恢复错误处理器之前到达
    const bool grabbed = !g_grabBadAccess.load();
    if (!grabbed) {
        // 冲突可能只失败了一个锁变体：把这一组里已经抓到的也撤掉，整条热键跳过。
        g_grabBadAccess.store(false);
        for (const unsigned int lockMask : variants) {
            XUngrabKey(display, keycode, modifiers | lockMask, root);
        }
        XSync(display, False);
    }
    g_previousErrorHandler = nullptr;
    XSetErrorHandler(previous);

    if (!grabbed) {
        shot::Logger(host).warn("[hotkey] 热键 " + action + "（" + binding.label +
                                "）注册失败（X11 BadAccess：该组合键已被其它程序占用），"
                                "已跳过这一项");
        return false;
    }

    X11Hotkey slot;
    slot.keycode = keycode;
    slot.modifiers = modifiers;
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

    // PrintScreen 在桌面环境里也可能被系统截图工具先接管（GNOME 默认把 PrintScreen
    // 绑到截图），与 Win11 的同类拦截一样：抓取成功不等于按键一定到得了本插件。
    if (binding.virtualKey == kVkSnapshot && binding.modifiers == 0) {
        shot::Logger(host).warn(
            "[hotkey] 注意：裸按 PrintScreen 在部分桌面环境（如 GNOME）里会被系统截图工具先接管，"
            "可能收不到按键；建议改用 Ctrl+PrintScreen 等组合或取消桌面快捷键绑定");
    }
    return true;
}

void HotkeyWindow::Impl::publishHotkey(unsigned int keycode, unsigned int state) {
    // 只比较本插件支持的四个修饰位：state 里还可能有锁位与鼠标键位。锁位在抓取时
    // 已经用锁变体覆盖（服务器精确匹配），鼠标键位不影响按键语义。
    const unsigned int pressed = state & kSignificantModifiers;
    const auto it = std::find_if(hotkeys.begin(), hotkeys.end(),
                                 [keycode, pressed](const X11Hotkey& slot) {
                                     return slot.keycode == keycode && slot.modifiers == pressed;
                                 });
    if (it == hotkeys.end()) {
        // 抓取变体与匹配规则理论上不会漏；漏了也只记 DEBUG，不打断热键线程。
        shot::Logger(host).debug("[hotkey] 收到未登记的按键（keycode " + std::to_string(keycode) +
                                 "），已忽略");
        return;
    }
    shot::EventBus bus(host);
    if (!bus.publish(it->topic.c_str(), it->payload)) {
        shot::Logger(host).warn("[hotkey] 发布 " + it->topic +
                                " 失败（宿主没有事件总线或队列已满）");
        return;
    }
    // 这条日志是验收锚点："发布 capture.request" 出现即说明热键通路打通（与 Windows 同格式）。
    std::string message =
        "[hotkey] 发布 " + it->topic + "（" + it->action + " 热键 " + it->label + "）";
    if (!it->payload.empty()) {
        message += "，负载 " + it->payload;
    }
    shot::Logger(host).info(message);
}

}  // namespace hotkey
