// 最小示例插件：逐像素取反 RGB，保留 A 通道（Image.channels == 4 即 BGRA）。
// 刻意不定义任何宏 —— 走 plugin_api_export.h 的插件默认分支（dllexport / visibility default）。

#include <cstddef>
#include <cstdint>

#include "plugin_api.h"

namespace {

class InvertProcessor final : public IImageProcessor {
public:
    const char* name() const override { return "example.invert"; }

    void process(Image& image) override {
        if (image.data == nullptr || image.width <= 0 || image.height <= 0 || image.channels <= 0) {
            return;
        }

        const std::ptrdiff_t row_bytes =
            static_cast<std::ptrdiff_t>(image.width) * static_cast<std::ptrdiff_t>(image.channels);
        const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(image.stride);
        // stride 可为负（自底向上）或大于 width*channels（行尾填充），所以必须按行寻址。
        if (stride >= 0 ? stride < row_bytes : -stride < row_bytes) {
            return;
        }

        const bool keep_alpha = (image.channels == 4);
        for (int y = 0; y < image.height; ++y) {
            std::uint8_t* row = image.data + static_cast<std::ptrdiff_t>(y) * stride;
            for (std::ptrdiff_t x = 0; x < row_bytes; ++x) {
                if (keep_alpha && (x % 4) == 3) {
                    continue;  // BGRA 的第 4 字节是 alpha，取反会改变透明度语义
                }
                row[x] = static_cast<std::uint8_t>(255u - row[x]);
            }
        }
    }
};

}  // namespace

extern "C" {

SCREENSHOT_API int plugin_abi_version(void) { return PLUGIN_ABI_VERSION; }

// 由本模块的堆分配，也只能由本模块释放：宿主拿到的是不透明指针。
SCREENSHOT_API IImageProcessor* plugin_create_processor(void) { return new InvertProcessor(); }

SCREENSHOT_API void plugin_destroy_processor(IImageProcessor* processor) { delete processor; }

}  // extern "C"
