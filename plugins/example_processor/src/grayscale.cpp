// 灰度：mode 选算法，strength 控制混合强度。
//
//   mode = luma    : Y = 0.299R + 0.587G + 0.114B（BT.601，×256 定点）
//   mode = average : (B + G + R) / 3
//   strength 0..100: 0 = 原图，100 = 完全灰度
//
// 只在 channels == 4（BGRA8）时生效，A 通道原样保留。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>

#include "processors.hpp"

namespace example_processor {
namespace {

const char kKeyMode[] = "mode";
const char kKeyStrength[] = "strength";

constexpr int kBlueWeight = 29;    // 0.114 * 256
constexpr int kGreenWeight = 150;  // 0.587 * 256
constexpr int kRedWeight = 77;     // 0.299 * 256

struct GrayscaleState {
    bool use_luma = true;
    int strength = 100;  // 0..100
};

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeyMode,
        .label         = "mode",
        .default_value = "luma",
        .options       = "luma|average",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "灰度算法",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_INT,
        .key           = kKeyStrength,
        .label         = "strength",
        .default_value = "100",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 100.0,
        .step          = 1.0,
        .tooltip       = "0 = 原图，100 = 完全灰度",
    },
};

std::int32_t grayscale_process(void* self, ShotImage* image) {
    if (image == nullptr || image->data == nullptr || image->channels != 4) {
        return 1;
    }
    // 本算法只对 BGRA8 有定义。宿主可能比本插件老、结构里还没有 pixel_format，
    // 那种情况按当年的约定当成 BGRA8 处理。
    if (SHOT_HAS_FIELD(image, ShotImage, pixel_format) &&
        image->pixel_format != SHOT_PIXEL_FORMAT_BGRA8) {
        return 1;
    }
    const auto* state = static_cast<const GrayscaleState*>(state_of(self));
    const bool use_luma = (state == nullptr) ? true : state->use_luma;
    const int strength = (state == nullptr) ? 100 : state->strength;

    const bool ok = for_each_row(*image, [use_luma, strength](std::uint8_t* first, std::uint8_t* last) {
        for (std::uint8_t* p = first; p + 3 < last; p += 4) {
            const int b = p[0];
            const int g = p[1];
            const int r = p[2];
            const int gray = use_luma ? ((kBlueWeight * b + kGreenWeight * g + kRedWeight * r) >> 8)
                                      : ((b + g + r) / 3);
            // strength 混合：v = orig + (gray - orig) * strength / 100
            for (int channel = 0; channel < 3; ++channel) {
                const int orig = p[channel];
                int mixed = orig + ((gray - orig) * strength) / 100;
                mixed = mixed < 0 ? 0 : (mixed > 255 ? 255 : mixed);
                p[channel] = static_cast<std::uint8_t>(mixed);
            }
            // p[3] 是 alpha，保留
        }
    });
    return ok ? 0 : 1;
}

std::uint32_t grayscale_config_count(void* /*self*/) {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t grayscale_config_desc(void* /*self*/, std::uint32_t index, ShotConfigDesc* out) {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copy_config_desc(&kConfig[index], out);
}

std::int32_t grayscale_get_config(void* self, const char* key, char* buf, std::uint32_t buf_size) {
    if (key == nullptr) {
        return -1;
    }
    const auto* state = static_cast<const GrayscaleState*>(state_of(self));
    if (std::strcmp(key, kKeyMode) == 0) {
        const bool use_luma = (state == nullptr) ? true : state->use_luma;
        return copy_config_value(buf, buf_size, use_luma ? "luma" : "average");
    }
    if (std::strcmp(key, kKeyStrength) == 0) {
        const int strength = (state == nullptr) ? 100 : state->strength;
        char text[16];
        const int len = std::snprintf(text, sizeof(text), "%d", strength);
        if (len < 0) {
            return -1;
        }
        return copy_config_value(buf, buf_size, text);
    }
    return -1;
}

std::int32_t grayscale_set_config(void* self, const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    auto* state = static_cast<GrayscaleState*>(state_of(self));
    if (state == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyMode) == 0) {
        if (std::strcmp(value, "luma") == 0) {
            state->use_luma = true;
            return 0;
        }
        if (std::strcmp(value, "average") == 0) {
            state->use_luma = false;
            return 0;
        }
        return -1;  // 非法取值：拒绝，但宿主可以继续工作
    }
    if (std::strcmp(key, kKeyStrength) == 0) {
        const int parsed = std::atoi(value);
        state->strength = parsed < 0 ? 0 : (parsed > 100 ? 100 : parsed);
        return 0;
    }
    return -1;
}

void* grayscale_create() { return new (std::nothrow) GrayscaleState(); }

void grayscale_destroy(void* self) { delete static_cast<GrayscaleState*>(self); }

const ShotImageProcessorV1 kImageOps = SHOT_OPS(ShotImageProcessorV1, .process = &grayscale_process);

const ShotConfigurableV1 kConfigOps = SHOT_OPS(ShotConfigurableV1,
                                               .get_config_count = &grayscale_config_count,
                                               .get_config_desc  = &grayscale_config_desc,
                                               .get_config       = &grayscale_get_config,
                                               .set_config       = &grayscale_set_config);

const ShotIfaceEntry kIfaces[] = {
    { SHOT_IFACE_IMAGE_PROCESSOR, 1, &kImageOps },
    { SHOT_IFACE_CONFIGURABLE, 1, &kConfigOps },
    { nullptr, 0, nullptr },
};

}  // namespace

const PluginEntry kGrayscale = {
    "example.grayscale",
    "Grayscale (luma / average)",
    "1.0.0",
    &grayscale_create,
    &grayscale_destroy,
    kIfaces,
};

}  // namespace example_processor
