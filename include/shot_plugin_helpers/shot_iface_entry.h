#pragma once

// ============================================================================
//  include/shot_plugin_helpers/shot_iface_entry.h —— 能力目录与 SHOT_OPS 宏（仅 C++ 使用，不走 ABI）
//
//  插件用一张 { id, 最高版本, 函数表 } 的静态数组回答 query_interface；
//  SHOT_OPS 负责填对 struct_size / version（漏填等于整张表不存在）。
//  本头文件只有能力目录 struct + 宏，不声明类；插件通常只包含汇总头 shot_plugin_helpers.h。
// ============================================================================

#include <cstdint>
#include <cstring>

#include "plugin_api.h"

/**
 * @brief 能力目录项：一个能力 id、它能提供的最高版本、以及函数表指针。
 *
 * 用 max_version 而不是精确版本：插件升级函数表后，老宿主按自己认识的版本
 * 来问，仍能拿到兼容的那张表。
 *
 * @note 目录数组必须以 { nullptr, 0, nullptr } 结尾，shot_query_interface 靠它停下。
 */
struct ShotIfaceEntry {
    const char* id;          /**< 能力 id，如 SHOT_IFACE_IMAGE_PROCESSOR；编译期静态字符串 */
    uint32_t    max_version; /**< 本实现提供的最高函数表版本，当前都是 1 */
    const void* ops;         /**< 指向该能力的函数表，如 ShotImageProcessorV1；不能为 NULL */
};

/**
 * @brief 按 id 与版本查能力目录，命中时返回函数表指针。
 *
 * 宿主问 version = N：返回"能提供 <= N 版"的那个函数表；
 * 没有这个能力、或本插件只提供更高版本 -> 返回 nullptr，宿主安静跳过。
 *
 * @param[in] ifaces  目录数组，必须以 { nullptr, 0, nullptr } 结尾；可为 nullptr（视作空目录）。
 * @param[in] id      要问的能力 id；为 nullptr 时直接返回 nullptr。
 * @param[in] version 宿主认识的能力版本，通常为 1。
 * @return 命中的函数表指针；没有匹配返回 nullptr。
 * @note 只读、线程安全；返回的指针归插件所有，有效期到 DLL 卸载。
 */
inline void* shot_query_interface(const ShotIfaceEntry* ifaces,
                                  const char* id,
                                  uint32_t version) {
    if (ifaces == nullptr || id == nullptr) {
        return nullptr;
    }
    for (const ShotIfaceEntry* entry = ifaces; entry->id != nullptr; ++entry) {
        if (std::strcmp(entry->id, id) == 0 && version <= entry->max_version) {
            return const_cast<void*>(entry->ops);
        }
    }
    return nullptr;
}

/**
 * @brief 生成能力函数表实例，并把 struct_size / version 填对。
 *
 * 第一个参数是结构体类型名，用来取 sizeof；其余参数是字段的指定初始化器，
 * 字段顺序必须与结构体声明一致，否则编译不过。
 * 手写 struct_size 是最容易漏的一步：漏了宿主就会把这个结构当成不存在
 * （SHOT_HAS_FIELD 判否），整张表等于没提供。
 *
 * @note 用法：const ShotImageProcessorV1 g_ops = SHOT_OPS(ShotImageProcessorV1, .process = my_process);
 * @warning 仅 C++ 可用（本头文件就是 C++ only）；version 固定填 1，出现新版本结构体时要跟着改宏。
 */
#define SHOT_OPS(type, ...) { .struct_size = sizeof(type), .version = 1, __VA_ARGS__ }

