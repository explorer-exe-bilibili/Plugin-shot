// 逐字节取反 B/G/R；A 通道是否也取反由配置项 keep_alpha 决定。
//
// 这个文件同时演示一件事：配置项声明、读写、以及"不认识的 key 安全忽略"
// 全部由插件自己提供，宿主不需要知道任何配置项名字。
//
// 配置项的 label/tooltip 写的是语言表 key（文案本体在本包共用的
// plugin_entry.cpp kLangTables 里）：设置页按 display(owner, key) 解析，所以这里
// 不硬编码任何用户可见文案。

#include <cstring>
#include <iterator>
#include <memory>
#include <new>

#include "processors.hpp"

namespace example_processor {
namespace {

const char kKeyKeepAlpha[] = "keep_alpha";

// 文案 key：必须与 plugin_entry.cpp 的 kLangTables 完全一致；拼错时设置页会
// 原样显示 key，表现为"配置项标题变成 config.xxx.label"。
const char kLabelKeepAlpha[] = "config.keep_alpha.label";
const char kTooltipKeepAlpha[] = "config.keep_alpha.tooltip";

struct InvertState {
    bool keep_alpha = true;
    // create() 拿不到宿主服务表：I18n 由 create_instance 在拿到 host 后调
    // invert_start() 构造；destroy 里先 reset（析构会退订 lists.changed）再释放状态。
    std::unique_ptr<shot::I18n> i18n;
};

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyKeepAlpha,
        .label         = kLabelKeepAlpha,
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = kTooltipKeepAlpha,
    },
};

/**
 * @brief 拿到宿主服务表后初始化多语言（create_instance 调，create() 拿不到 host）。
 *
 * @param[in] self 插件状态（InvertState*）。
 * @param[in] host 宿主服务表；老宿主上可能为 nullptr。
 * @return true = 语言表已发布；false = 状态为空或语言表全部非法（宿主记 WARN 后继续）。
 */
bool invert_start(void* self, const ShotHostServices* host) {
    auto* state = static_cast<InvertState*>(self);
    if (state == nullptr) {
        return false;
    }
    // owner 用插件身份证（kInvert.id），宿主按它校验 lists.add、并在卸载时按它清理贡献。
    state->i18n = std::make_unique<shot::I18n>(host, kInvert.id);
    return load_language_tables(*state->i18n);
}

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

void invert_destroy(void* self) {
    auto* state = static_cast<InvertState*>(self);
    if (state == nullptr) {
        return;
    }
    // 先退订 lists.changed 再释放状态：宿主之后可能卸载本 DLL，
    // 留着订阅会回调已释放代码。
    state->i18n.reset();
    delete state;
}

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
    &invert_start,
};

}  // namespace example_processor
