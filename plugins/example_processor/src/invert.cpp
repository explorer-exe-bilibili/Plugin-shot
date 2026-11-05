// 逐字节取反 B/G/R；A 通道是否也取反由配置项 keep_alpha 决定。
//
// 这个文件同时演示一件事：配置项声明、读写、以及"不认识的 key 安全忽略"
// 全部由插件自己提供，宿主不需要知道任何配置项名字。

#include <cstring>
#include <iterator>
#include <new>

#include "processors.hpp"

namespace example_processor {
namespace {

const char kKeyKeepAlpha[] = "keep_alpha";

struct InvertState {
    bool keep_alpha = true;
};

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyKeepAlpha,
        .label         = "keep alpha",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "取反时跳过 A 通道",
    },
};

std::int32_t invert_process(void* self, ShotImage* image) {
    if (image == nullptr || image->data == nullptr) {
        return 1;
    }
    // 只认识 BGRA8。宿主可能比本插件老、结构里还没有 pixel_format，
    // 那种情况按当年的约定当成 BGRA8 处理（新插件 + 老宿主的正确降级）。
    if (SHOT_HAS_FIELD(image, ShotImage, pixel_format) &&
        image->pixel_format != SHOT_PIXEL_FORMAT_BGRA8) {
        return 1;
    }
    const auto* state = static_cast<const InvertState*>(state_of(self));
    const bool keep_alpha = (state == nullptr) ? true : state->keep_alpha;

    const std::int32_t channels = image->channels;
    // BGRA8 的第 4 字节是 alpha：取反它会改变透明度语义，所以默认跳过。
    const bool skip_alpha = keep_alpha && channels == 4;

    const bool ok = for_each_row(*image, [channels, skip_alpha](std::uint8_t* first, std::uint8_t* last) {
        for (std::uint8_t* p = first; p != last; ++p) {
            if (skip_alpha && ((p - first) % channels) == 3) {
                continue;
            }
            *p = static_cast<std::uint8_t>(255u - *p);
        }
    });
    return ok ? 0 : 1;
}

std::uint32_t invert_config_count(void* /*self*/) {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t invert_config_desc(void* /*self*/, std::uint32_t index, ShotConfigDesc* out) {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copy_config_desc(&kConfig[index], out);
}

std::int32_t invert_get_config(void* self, const char* key, char* buf, std::uint32_t buf_size) {
    if (key == nullptr) {
        return -1;
    }
    const auto* state = static_cast<const InvertState*>(state_of(self));
    if (std::strcmp(key, kKeyKeepAlpha) == 0) {
        const bool value = (state == nullptr) ? true : state->keep_alpha;
        return copy_config_value(buf, buf_size, value ? "1" : "0");
    }
    return -1;  // 不认识的 key：安全忽略，宿主升级也不会踩到这里
}

std::int32_t invert_set_config(void* self, const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    auto* state = static_cast<InvertState*>(state_of(self));
    if (state == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyKeepAlpha) == 0) {
        state->keep_alpha = !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
        return 0;
    }
    return -1;
}

void* invert_create() { return new (std::nothrow) InvertState(); }

void invert_destroy(void* self) { delete static_cast<InvertState*>(self); }

const ShotImageProcessorV1 kImageOps = SHOT_OPS(ShotImageProcessorV1, .process = &invert_process);

const ShotConfigurableV1 kConfigOps = SHOT_OPS(ShotConfigurableV1,
                                              .get_config_count = &invert_config_count,
                                              .get_config_desc  = &invert_config_desc,
                                              .get_config       = &invert_get_config,
                                              .set_config       = &invert_set_config);

const ShotIfaceEntry kIfaces[] = {
    { SHOT_IFACE_IMAGE_PROCESSOR, 1, &kImageOps },
    { SHOT_IFACE_CONFIGURABLE, 1, &kConfigOps },
    { nullptr, 0, nullptr },
};

}  // namespace

const PluginEntry kInvert = {
    "example.invert",
    "Invert (keep alpha)",
    "1.0.0",
    &invert_create,
    &invert_destroy,
    kIfaces,
};

}  // namespace example_processor
