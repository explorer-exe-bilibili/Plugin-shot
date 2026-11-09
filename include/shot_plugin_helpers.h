#pragma once

// ============================================================================
//  include/shot_plugin_helpers.h —— 插件侧工具汇总头（仅 C++ 使用，不走 ABI）
//
//  这一层不跨界，只是让每个插件不必手写同样的样板。按 STYLE.md §6.8，这里只做
//  include 汇总、不声明类；每个类各有一个头文件：
//    · shot_plugin_helpers/shot_iface_entry.h    —— ShotIfaceEntry + shot_query_interface + SHOT_OPS
//    · shot_plugin_helpers/shot_logger.h         —— shot::Logger（写宿主日志）
//    · shot_plugin_helpers/shot_config.h         —— shot::Config（读写宿主全局配置）
//    · shot_plugin_helpers/shot_event_bus.h      —— shot::EventBus（主题发布/订阅）
//    · shot_plugin_helpers/shot_plugin_lists.h   —— shot::PluginLists（列表服务：按 list id 读 JSON 快照）
//    · shot_plugin_helpers/shot_plugin_registry.h—— shot::PluginStatus / shot::PluginRegistry（插件启停）
//
//  插件通常只需要 #include "shot_plugin_helpers.h"；要用到具体类型、或想减少依赖时，
//  也可以只包含对应的那个头文件。
// ============================================================================

#include "shot_plugin_helpers/shot_config.h"
#include "shot_plugin_helpers/shot_event_bus.h"
#include "shot_plugin_helpers/shot_iface_entry.h"
#include "shot_plugin_helpers/shot_logger.h"
#include "shot_plugin_helpers/shot_plugin_lists.h"
#include "shot_plugin_helpers/shot_plugin_registry.h"
