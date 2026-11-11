// 灰度：mode 选算法，strength 控制混合强度。
//
//   mode = luma    : Y = 0.299R + 0.587G + 0.114B（BT.601，×256 定点）
//   mode = average : (B + G + R) / 3
//   strength 0..100: 0 = 原图，100 = 完全灰度
//
// 只在 channels == 4（BGRA8）时生效，A 通道原样保留。
//
// 配置项的 label/tooltip 写的是语言表 key（文案本体在本包共用的
// plugin_entry.cpp kLangTables 里）：设置页按 display(owner, key) 解析，ENUM 的
// 存储值 luma / average 属于技术值，保持不翻译。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>

#include "processors.hpp"

namespace example_processor {
namespace {

const char kKeyMode[] = "mode";
const char kKeyStrength[] = "strength";

// 文案 key：必须与 plugin_entry.cpp 的 kLangTables 完全一致；拼错时设置页会
// 原样显示 key，表现为"配置项标题变成 config.xxx.label"。
const char kLabelMode[] = "config.mode.label";
const char kTooltipMode[] = "config.mode.tooltip";
const char kLabelStrength[] = "config.strength.label";
const char kTooltipStrength[] = "config.strength.tooltip";

constexpr int kBlueWeight = 29;    // 0.114 * 256
constexpr int kGreenWeight = 150;  // 0.587 * 256
constexpr int kRedWeight = 77;     // 0.299 * 256

struct GrayscaleState {
    bool use_luma = true;
    int strength = 100;  // 0..100
    // create() 拿不到宿主服务表：I18n 由 create_instance 在拿到 host 后调
    // grayscale_start() 构造；destroy 里先 reset（析构会退订 lists.changed）再释放状态。
    std::unique_ptr<shot::I18n> i18n;
};

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeyMode,
        .label         = kLabelMode,
        .default_value = "luma",
        .options       = "luma|average",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipMode,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_INT,
        .key           = kKeyStrength,
        .label         = kLabelStrength,
        .default_value = "100",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 100.0,
        .step          = 1.0,
        .tooltip       = kTooltipStrength,
    },
};

/**
 * @brief 拿到宿主服务表后初始化多语言（create_instance 调，create() 拿不到 host）。
 *
 * @param[in] self 插件状态（GrayscaleState*）。
 * @param[in] host 宿主服务表；老宿主上可能为 nullptr。
 * @return true = 语言表已发布；false = 状态为空或语言表全部非法（宿主记 WARN 后继续）。
 */
bool grayscale_start(void* self, const ShotHostServices* host) {
    auto* state = static_cast<GrayscaleState*>(self);
    if (state == nullptr) {
        return false;
    }
    // owner 用插件身份证（kGrayscale.id），宿主按它校验 lists.add、并在卸载时按它清理贡献。
    state->i18n = std::make_unique<shot::I18n>(host, kGrayscale.id);
    return load_language_tables(*state->i18n);
}

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

void grayscale_destroy(void* self) {
    auto* state = static_cast<GrayscaleState*>(self);
    if (state == nullptr) {
        return;
    }
    // 先退订 lists.changed 再释放状态：宿主之后可能卸载本 DLL，
    // 留着订阅会回调已释放代码。
    state->i18n.reset();
    delete state;
}

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
    &grayscale_start,
};

}  // namespace example_processor
