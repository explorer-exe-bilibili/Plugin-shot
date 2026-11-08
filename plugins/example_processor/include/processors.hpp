#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "plugin_api.h"
#include "shot_plugin_helpers.h"

namespace example_processor {

/**
 * @brief 逐行遍历 ShotImage 的有效像素区间。
 *
 * visit 的签名是 void(std::uint8_t* first, std::uint8_t* last)，last 为开区间；
 * 每行的有效字节数按 width*channels 计算，行距由 stride 决定。
 *
 * @param[in] image 待处理的图像；data 为空或宽/高/通道数非正时直接失败。
 * @param[in] visit 每行调用一次，只能访问 [first, last)。
 * @return true = 所有行遍历完成；false = 参数非法或 |stride| 装不下一行，此时不会调用 visit。
 * @warning stride 以字节计，可以为负（自底向上存储），也可以大于 width*channels
 *          （行尾填充）；只能按行寻址，把 data 当连续块是这类插件最常见的越界来源。
 */
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

/**
 * @brief 插件侧对一个插件的完整描述：身份证 + 能力目录 + 生命周期。
 *
 * 放在这里而不是写死在 plugin_entry.cpp，是为了做到"一个插件一个 .cpp"：
 * invert.cpp 自己声明它会什么，entry 只按索引把它们排成一行。
 */
struct PluginEntry {
    const char* id;                /**< 全局唯一 id（"作者.名字"），必须与 metadata.json 一致。 */
    const char* name;              /**< 界面显示名。 */
    const char* version;           /**< 插件自己的语义化版本，如 "1.0.0"。 */
    void* (*create)();             /**< 创建插件状态；失败返回 nullptr，写 nullptr 表示无状态。 */
    void (*destroy)(void* self);   /**< 释放 create 返回的状态；与 create 成对，可为 nullptr。 */
    const ShotIfaceEntry* ifaces;  /**< 能力目录，必须以 { nullptr, 0, nullptr } 结尾。 */
};

/** @brief 插件 example.invert 的静态描述：逐字节取反 B/G/R，可选保留 alpha。 */
extern const PluginEntry kInvert;
/** @brief 插件 example.grayscale 的静态描述：按亮度或算术平均转灰度，可调混合强度。 */
extern const PluginEntry kGrayscale;

/**
 * @brief 宿主拿到的不透明实例：哪个插件 + 插件自己的状态。
 *
 * 注意：能力函数表里收到的 self 也是这个指针，不是插件状态本身。
 * 所以每个 process / 配置函数都要先 state_of(self) 取回自己的状态 ——
 * 直接 static_cast<XxxState*>(self) 会把 entry 指针当成状态，读出来是垃圾，
 * 写进去则损坏内存。
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
    return self != nullptr ? static_cast<PluginInstance*>(self)->state : nullptr;
}

/**
 * @brief 按契约把配置值写进宿主缓冲区，并报告值的字节数。
 *
 * 返回值是值本身的长度（不含结尾 NUL）：buf 不够长时只报告长度、不写，
 * 宿主据此扩容后重试。
 *
 * @param[out] buf 宿主缓冲区，可为 nullptr（此时只查询长度）。
 * @param[in]  buf_size buf 的字节数；大于返回值时才会写入，写入内容含结尾 NUL。
 * @param[in]  value 要返回的字符串；不允许为 nullptr。
 * @return >= 0 = 值的字节数（不含结尾 NUL）；-1 = value 为空（调用方传了非法参数）。
 * @warning 返回值 >= 0 只表示长度，负数只表示 key 不认识——不要把"buf 装不下"当成失败。
 */
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

/**
 * @brief 把插件侧的配置项声明整段拷进宿主给的 ShotConfigDesc。
 *
 * 宿主要多少就填多少：先按 out->struct_size 判断宿主认识的结构够不够长，
 * 不够就报 2，绝不把字段写到宿主内存之外。
 *
 * @param[in]  src 插件内的静态配置声明，不能为空。
 * @param[out] out 宿主提供的输出结构；进入时读 struct_size。
 * @return 0 = 已填入；1 = src 或 out 为空；2 = out->struct_size 太短，装不下 tooltip 字段。
 */
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
