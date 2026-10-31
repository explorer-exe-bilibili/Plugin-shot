// BT.601 亮度：Y = 0.299R + 0.587G + 0.114B，用 ×256 定点权重避免浮点。
// 只在 channels == 4（BGRA）时生效，A 通道原样保留。

#include "example_processor/processors.hpp"

namespace example_processor {
namespace {
constexpr int kBlueWeight = 29;   // 0.114 * 256
constexpr int kGreenWeight = 150; // 0.587 * 256
constexpr int kRedWeight = 77;    // 0.299 * 256
}  // namespace

int grayscale_bgra(void* /*internal_data*/, Image* image) {
    if (image == nullptr || image->channels != 4 || image->data == nullptr) {
        return 0;
    }

    return for_each_row(*image, [](std::uint8_t* first, std::uint8_t* last) {
        for (std::uint8_t* p = first; p + 3 < last; p += 4) {
            const int luma = (kBlueWeight * p[0] + kGreenWeight * p[1] + kRedWeight * p[2]) >> 8;
            const auto value = static_cast<std::uint8_t>(luma);
            p[0] = value;  // B
            p[1] = value;  // G
            p[2] = value;  // R
            // p[3] 是 alpha，跳过
        }
    }) ? 1 : 0;
}

}  // namespace example_processor
