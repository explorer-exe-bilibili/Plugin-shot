#pragma once

#include <cstddef>
#include <cstdint>

#include "plugin_api.h"

namespace example_processor {

// 逐行遍历 Image 的有效像素区间。
//
// stride 以字节计，可以为负（自底向上存储），也可以大于 width*channels（行尾填充），
// 所以只能按行寻址；把 data 当连续块是这类插件最常见的越界来源。
// visit 的签名是 void(std::uint8_t* first, std::uint8_t* last)；last 为开区间。
template <typename Visit>
bool for_each_row(const Image& image, Visit&& visit) {
    if (image.data == nullptr || image.width <= 0 || image.height <= 0 || image.channels <= 0) {
        return false;
    }

    const std::ptrdiff_t row_bytes =
        static_cast<std::ptrdiff_t>(image.width) * static_cast<std::ptrdiff_t>(image.channels);
    const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(image.stride);
    if (stride >= 0 ? stride < row_bytes : -stride < row_bytes) {
        return false;
    }

    for (int y = 0; y < image.height; ++y) {
        std::uint8_t* first = image.data + static_cast<std::ptrdiff_t>(y) * stride;
        visit(first, first + row_bytes);
    }
    return true;
}

// 两个示例处理器的实现都在 src/ 下。返回非 0 表示处理成功（见 CPluginHandle::process）。
int invert_bgra(void* internal_data, Image* image);
int grayscale_bgra(void* internal_data, Image* image);

}  // namespace example_processor
