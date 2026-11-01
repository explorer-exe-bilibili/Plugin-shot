#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "plugin_api.h"
#include "shot_plugin_helpers.h"

namespace example_processor {

// 逐行遍历 ShotImage 的有效像素区间。
//
// stride 以字节计，可以为负（自底向上存储），也可以大于 width*channels（行尾填充），
// 所以只能按行寻址；把 data 当连续块是这类插件最常见的越界来源。
// visit 的签名是 void(std::uint8_t* first, std::uint8_t* last)；last 为开区间。
template <typename Visit>
bool for_each_row(const ShotImage& image, Visit&& visit) {
    if (image.data == nullptr || image.width <= 0 || image.height <= 0 || image.channels <= 0) {
        return false;
    }

    const std::ptrdiff_t row_bytes =
        static_cast<std::ptrdiff_t>(image.width) * static_cast<std::ptrdiff_t>(image.channels);
    const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(image.stride);
    if (stride >= 0 ? stride < row_bytes : -stride < row_bytes) {
        return false;
    }

    for (std::int32_t y = 0; y < image.height; ++y) {
        std::uint8_t* first = image.data + static_cast<std::ptrdiff_t>(y) * stride;
        visit(first, first + row_bytes);
    }
    return true;
}

// 插件侧对"一个插件"的完整描述：身份证 + 能力目录 + 生命周期。
// 放在这里而不是写死在 plugin_entry.cpp，是为了做到"一个插件一个 .cpp"：
// invert.cpp 自己声明它会什么，entry 只按索引把它们排成一行。
struct PluginEntry {
    const char* id;
    const char* name;
    const char* version;
    void* (*create)();
    void (*destroy)(void* self);
    const ShotIfaceEntry* ifaces;  // 以 { nullptr, 0, nullptr } 结尾
};

extern const PluginEntry kInvert;
extern const PluginEntry kGrayscale;

// 宿主拿到的不透明指针就是它：哪个插件 + 插件自己的状态。
//
// 注意：能力函数表里收到的 self 也是这个指针，不是插件状态本身。
// 所以每个 process / 配置函数都要先 state_of(self) 取回自己的状态 ——
// 直接 static_cast<XxxState*>(self) 会把 entry 指针当成状态，读出来是垃圾，
// 写进去则损坏内存。
struct PluginInstance {
    const PluginEntry* entry;
    void* state;
};

inline void* state_of(void* self) {
    return self != nullptr ? static_cast<PluginInstance*>(self)->state : nullptr;
}

// 配置读值的小工具：把一段字符串值按契约写进宿主给的 buf。
// 返回值 = 值的字节数（不含结尾 NUL）；buf 不够长时只报告长度、不写。
inline std::int32_t copy_config_value(char* buf, std::uint32_t buf_size, const char* value) {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    if (buf != nullptr && buf_size > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

// 把配置项整段交给宿主（宿主要多少就填多少）。
inline std::int32_t copy_config_desc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (out == nullptr || src == nullptr) {
        return 1;
    }
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;  // 宿主的结构比本插件认识的还短，装不下
    }
    *out = *src;
    return 0;
}

}  // namespace example_processor
