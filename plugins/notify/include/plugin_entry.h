#pragma once

#include "plugin_api.h"
#include "shot_plugin_helpers/shot_iface_entry.h"

namespace notify {

// ============================================================================
//  插件身份证与实例（与 example_processor / tray / clipboard 同一套结构）
// ============================================================================

/**
 * @brief 插件侧对一个插件的完整描述：身份证 + 能力目录 + 生命周期。
 *
 * 与 example_processor / tray / clipboard 保持同一套结构，宿主侧完全不需要区分
 * 插件类型：一个 DLL 里装几个插件，就按 kEntries[] 的顺序排几个 PluginEntry。
 */
struct PluginEntry {
    const char* id;                /**< 全局唯一 id（"作者.名字"），必须与 metadata.json 一致。 */
    const char* name;              /**< 界面显示名。 */
    const char* version;           /**< 插件自己的语义化版本，如 "0.1.0"。 */
    void* (*create)();             /**< 创建插件状态；失败返回 nullptr，写 nullptr 表示无状态。 */
    void (*destroy)(void* self);   /**< 释放 create 返回的状态；与 create 成对，可为 nullptr。 */
    const ShotIfaceEntry* ifaces;  /**< 能力目录，必须以 { nullptr, 0, nullptr } 结尾。 */
};

/**
 * @brief 插件 notify.main 的静态描述。
 *
 * 只实现 shot.configurable（没有 ui_provider）：不会有 post_to_plugin 回调，
 * 订阅/线程都在 create(index, host) 里建立、destroy 里收尾。
 */
extern const PluginEntry kNotify;

/**
 * @brief 宿主拿到的不透明实例：哪个插件 + 插件自己的状态。
 *
 * 能力函数表里收到的 self 也是这个指针，不是插件状态本身；所以每个回调
 * 都要先 state_of(self) 取回插件自己的状态。
 */
struct PluginInstance {
    const PluginEntry* entry;  /**< 指向静态插件描述；生命周期 = 整个 DLL。 */
    void* state;               /**< 插件 create 出来的状态，由插件自己解释与释放。 */
};

/**
 * @brief 从宿主给的 self 取回插件状态指针。
 *
 * self 是 PluginInstance*，不是状态本身；这是能力回调最容易写错的一步。
 *
 * @param[in] self 宿主传入的不透明实例，允许为 nullptr。
 * @return 插件状态；self 为空时返回 nullptr。
 */
inline void* state_of(void* self) {
    auto* instance = static_cast<PluginInstance*>(self);
    return (instance == nullptr) ? nullptr : instance->state;
}

}  // namespace notify
