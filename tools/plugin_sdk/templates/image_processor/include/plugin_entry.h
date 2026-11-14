#pragma once

// ============================================================================
//  include/plugin_entry.h —— 图像处理器模板的 ABI 胶水描述
//
//  一个 DLL 可以装多个插件：kEntries[] 的顺序就是 create(index) 的下标，必须与
//  metadata.json 的 plugins[] 一一对应。本模板只有一个插件，但保留索引表结构。
//
//  PluginEntry 里的函数指针都【不跨边界】：只是同一 DLL 内部的静态描述。
//  process（shot.image_processor）的回调与配置回调都通过本表找到插件状态。
// ============================================================================

#include <cstdint>

#include "plugin_api.h"
#include "shot_plugin_helpers/shot_iface_entry.h"

namespace image_processor_template {

/** @brief 插件状态对象；定义在 invert_processor.h，这里只前向声明。 */
class InvertProcessor;

/**
 * @brief 一个插件的静态描述：身份、能力目录、生命周期回调与配置回调。
 *
 * @note id / name / version 必须是编译期静态字符串：get_info 会把指针直接交给
 *       宿主，有效期要求到 DLL 卸载。
 */
struct PluginEntry {
    const char* id;     /**< 全局唯一插件 id，必须与 metadata.json 的 plugins[].id 一致。 */
    const char* name;   /**< 界面显示名；语言表里的 plugin.name 是它的本地化版本。 */
    const char* version;/**< 插件语义化版本，如 "1.0.0"。 */
    const ShotIfaceEntry* ifaces; /**< 能力目录，必须以 {nullptr, 0, nullptr} 结尾。 */

    void* (*create)(void);  /**< 分配插件状态；失败返回 nullptr。 */
    void (*destroy)(void* state); /**< 释放 create 的状态。 */
    bool (*start)(void* state, const ShotHostServices* host); /**< 绑定宿主；失败只降级。 */

    std::uint32_t (*configCount)(void* state); /**< shot.configurable：配置项个数。 */
    std::int32_t (*configDesc)(void* state, std::uint32_t index, ShotConfigDesc* out);
    std::int32_t (*configGet)(void* state, const char* key, char* buf, std::uint32_t bufSize);
    std::int32_t (*configSet)(void* state, const char* key, const char* value);
};

/**
 * @brief 宿主 create(index) 拿到的不透明句柄：插件描述 + 插件自己的状态对象。
 *
 * 宿主只持有指针并原样回传，绝不访问字段；能力回调通过 state_of() 取状态。
 */
struct PluginInstance {
    const PluginEntry* entry; /**< 指向静态描述（kEntries 的一项）。 */
    void* state;              /**< 插件状态，由 entry->create 分配。 */
};

/**
 * @brief 从宿主回传的 self 取插件状态对象。
 * @param[in] self create(index) 的返回值；允许为 nullptr。
 * @return 插件状态指针；self 为空时返回 nullptr。
 */
void* state_of(void* self);

/** @brief 本模板唯一插件的静态描述（定义在 plugin_entry.cpp）。 */
extern const PluginEntry kInvert;

}  // namespace image_processor_template
