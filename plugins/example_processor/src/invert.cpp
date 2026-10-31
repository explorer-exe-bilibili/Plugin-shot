// 逐字节取反 B/G/R，A 通道保持不动（截图场景 channels == 4，BGRA）。

#include "example_processor/processors.hpp"

namespace example_processor {

int invert_bgra(void* /*internal_data*/, Image* image) {
    if (image == nullptr) {
        return 0;
    }

    const int channels = image->channels;
    const bool keep_alpha = (channels == 4);
    return for_each_row(*image, [channels, keep_alpha](std::uint8_t* first, std::uint8_t* last) {
        for (std::uint8_t* p = first; p != last; ++p) {
            if (keep_alpha && ((p - first) % channels) == 3) {
                continue;  // BGRA 的第 4 字节是 alpha，取反会改变透明度语义
            }
            *p = static_cast<std::uint8_t>(255u - *p);
        }
    }) ? 1 : 0;
}

}  // namespace example_processor
