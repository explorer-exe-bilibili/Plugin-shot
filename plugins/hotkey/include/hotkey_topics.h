#pragma once

// ============================================================================
//  include/hotkey_topics.h —— 内部头：热键实现共用的订阅/投递主题
//
//  **不是插件对外接口**：只有本插件的平台实现（Windows / Linux X11 / 降级 stub）
//  和它们的内部头包含它。两个平台实现各编一份，主题名必须只有一个来源，否则
//  改了 Windows 的订阅却忘了 X11，两边行为会悄悄分叉。
//
//  主题语义见 document/EVENT_BUS.md 与 document/SETTINGS.md §4.1：
//    config.applied  —— 设置页应用后宿主广播（空负载），热键按当前配置整批重注册；
//    hotkey.suspend  —— 设置页开始录入组合键，撤销全部全局注册；
//    hotkey.resume   —— 录入结束/取消，按当前配置恢复注册；
//    host.shutdown   —— 宿主卸载 UI 插件前经 post_to_plugin 投递，等同 stop()。
// ============================================================================

namespace hotkey {

/** @brief 订阅：设置页应用后宿主发布空负载的 config.applied，触发热键重注册。 */
inline constexpr char kTopicConfigApplied[] = "config.applied";
/** @brief 订阅：设置页开始录入组合键（空负载），请求撤掉全部全局注册。 */
inline constexpr char kTopicHotkeySuspend[] = "hotkey.suspend";
/** @brief 订阅：设置页结束录入（空负载），请求按当前配置恢复注册。 */
inline constexpr char kTopicHotkeyResume[] = "hotkey.resume";

/** @brief 宿主卸载 UI 插件前投递的事件（走 post_to_plugin，不走总线）。 */
inline constexpr char kEventHostShutdown[] = "host.shutdown";

}  // namespace hotkey
