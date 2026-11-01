#pragma once

// ============================================================================
//  include/shot_plugin_helpers.h —— 插件侧的小工具（仅 C++ 使用）
//
//  这一层不跨界，只是让每个插件不必手写同样的样板：一张"能力 id -> 函数表"的
//  目录数组，以及一个按 id 查目录的循环。
// ============================================================================

#include <cstring>

#include "plugin_api.h"

// 目录项：一个能力 id、这个能力最高支持到第几版、指向该能力的函数表。
// 数组必须以 { nullptr, 0, nullptr } 结尾，shot_query_interface 靠它停下。
struct ShotIfaceEntry {
    const char* id;
    uint32_t    max_version;
    const void* ops;
};

// 按 id 与版本查目录。
//   宿主问 version = N：返回"能提供 <= N 版"的那个函数表；
//   没有这个能力、或本插件只提供更高版本 -> 返回 nullptr，宿主安静跳过。
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

// 生成一个能力函数表实例，顺手把 struct_size / version 填对。
// 手写 struct_size 是最容易漏的一步：漏了宿主就会把这个结构当成不存在。
//
// 用法（字段顺序必须与结构体声明一致，因为设计初始化器要求按序出现）：
//    const ShotImageProcessorV1 g_ops = SHOT_OPS(ShotImageProcessorV1, .process = my_process);
#define SHOT_OPS(type, ...) { .struct_size = sizeof(type), .version = 1, __VA_ARGS__ }
