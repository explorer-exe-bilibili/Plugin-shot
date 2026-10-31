// 最小示例插件：逐像素取反 RGB，保留 A 通道（Image.channels == 4 即 BGRA）。
// 刻意不定义任何宏 —— 走 plugin_api_export.h 的插件默认分支（dllexport / visibility default）。

#include <cstddef>
#include <cstdint>
#include <new>

#include "plugin_api.h"

namespace {

int process_invert(void* /*internal_data*/, Image* image) {
    if (image == nullptr || image->data == nullptr || image->width <= 0 || image->height <= 0 ||
        image->channels <= 0) {
        return 0;
    }

    const std::ptrdiff_t row_bytes =
        static_cast<std::ptrdiff_t>(image->width) * static_cast<std::ptrdiff_t>(image->channels);
    const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(image->stride);
    // stride 可为负（自底向上）或大于 width*channels（行尾填充），所以必须按行寻址。
    if (stride >= 0 ? stride < row_bytes : -stride < row_bytes) {
        return 0;
    }

    const bool keep_alpha = (image->channels == 4);
    for (int y = 0; y < image->height; ++y) {
        std::uint8_t* row = image->data + static_cast<std::ptrdiff_t>(y) * stride;
        for (std::ptrdiff_t x = 0; x < row_bytes; ++x) {
            if (keep_alpha && (x % 4) == 3) {
                continue;  // BGRA 的第 4 字节是 alpha，取反会改变透明度语义
            }
            row[x] = static_cast<std::uint8_t>(255u - row[x]);
        }
    }
    return 1;
}

}  // namespace

extern "C" {

SCREENSHOT_API int plugin_abi_version(void) { return PLUGIN_ABI_VERSION; }

SCREENSHOT_API CPluginHandle* plugin_create_processor(void) {
    auto* handle = new (std::nothrow) CPluginHandle{};
    if (handle == nullptr) {
        return nullptr;
    }
    handle->type = PluginType::ImageProcessor;
    handle->name = "example.invert";
    handle->internal_data = nullptr;
    handle->process = &process_invert;
    return handle;
}

SCREENSHOT_API void plugin_destroy(CPluginHandle* handle) { delete handle; }

}  // extern "C"
