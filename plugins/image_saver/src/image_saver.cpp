// 图像保存插件的能力实现：ShotImageSaverV1::save + ShotConfigurableV1 四个配置项。
//
// save 只做四件事：参数校验 → 取配置快照（合并调用方的覆盖参数）→ 解析目标路径
// → 编码落盘。编码在 ImageEncoder，路径规则在 OutputPath，配置在 SaverConfig；
// 本文件只负责把它们串起来，以及把类方法转成契约要求的 C 回调（STYLE §6.6/§6.8）。
//
// 契约要点（include/plugin_api.h 的 ShotImageSaverV1 注释）：
//   · image 只读，插件不得修改像素或结构体字段；
//   · directory / basename 为 NULL 时用本插件配置，非 NULL 时作为本次覆盖参数；
//   · 成功时把最终路径（NUL 结尾）写进 path，装不下返回 2；
//   · 返回码：0 成功 / 1 参数非法 / 2 path 缓冲不足 / 3 保存失败。

#include "image_saver.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <new>
#include <string>

#include "image_encoder.h"
#include "output_path.h"
#include "plugin_api.h"
#include "plugin_entry.h"
#include "shot_plugin_helpers.h"

namespace image_saver {

namespace {

// 最终路径的硬上限：正常路径远小于它，超大值只可能来自病态配置（或过长的目录），
// 与其把宿主给的小缓冲写爆，不如按保存失败报出去。
constexpr std::uint32_t kMaxPathBytes = 32768;

}  // namespace

ImageSaver::ImageSaver() = default;

std::int32_t ImageSaver::save(const ShotImage* image, const char* directory, const char* basename,
                              char* path, std::uint32_t pathSize) {
    if (image == nullptr || path == nullptr) {
        return 1;
    }
    if (ImageEncoder::validate(*image) != 0) {
        return 1;
    }

    const SaverConfig::Snapshot config = m_config.snapshot();
    // 快照里的格式理论上已是合法值（set 校验过），这里再兜一层：配置可能来自旧版本
    // 或手改的 JSON，非法格式退回 PNG，总比保存失败好。
    const std::string format = ImageEncoder::supportsFormat(config.format) ? config.format : "png";

    // directory / basename 非 NULL 是宿主的一次性覆盖参数（"另存为"），以传入值为准。
    const std::string outputDirectory = (directory != nullptr) ? directory : config.directory;
    const std::string nameTemplate = (basename != nullptr) ? basename : config.filename;

    const OutputPath output(outputDirectory, nameTemplate, ImageEncoder::extensionFor(format));
    const std::filesystem::path target = output.resolve();
    if (target.empty()) {
        return 3;  // 目录/名字解析不出可用路径（例如目录里同名文件已经堆满）
    }

    const std::string utf8Path = OutputPath::toUtf8(target);
    // 先确认宿主缓冲装得下再落盘：返回 2 的调用不应该已经建了目录、写了文件。
    if (utf8Path.size() + 1 > pathSize) {
        return 2;
    }
    if (utf8Path.size() + 1 > kMaxPathBytes) {
        return 3;
    }

    if (!OutputPath::ensureParentDirectory(target)) {
        return 3;
    }
    if (!ImageEncoder::encode(*image, format, config.quality, target)) {
        return 3;
    }

    // 成功：把最终绝对路径（UTF-8，含扩展名）写回宿主缓冲。
    std::memcpy(path, utf8Path.c_str(), utf8Path.size() + 1);
    return 0;
}

std::uint32_t ImageSaver::configCount() const noexcept {
    return SaverConfig::itemCount();
}

std::int32_t ImageSaver::configDesc(std::uint32_t index, ShotConfigDesc* out) const noexcept {
    return SaverConfig::describe(index, out);
}

std::int32_t ImageSaver::getConfig(const char* key, char* buf, std::uint32_t bufSize) const {
    return m_config.get(key, buf, bufSize);
}

std::int32_t ImageSaver::setConfig(const char* key, const char* value) {
    return m_config.set(key, value);
}

// ---------------------------------------------------------------------------
// ABI 胶水：能力回调 + 函数表 + 能力目录 + 插件身份证。
// 匿名命名空间里的都是 C 回调：签名由契约固定，只能是自由函数（STYLE §6.7 的
// 合法例外）；self 是 PluginInstance*，先 state_of 再转回 ImageSaver。
// ---------------------------------------------------------------------------
namespace {

std::int32_t image_save(void* self, const ShotImage* image, const char* directory,
                        const char* basename, char* path, std::uint32_t pathSize) {
    auto* saver = static_cast<ImageSaver*>(state_of(self));
    if (saver == nullptr) {
        return 1;
    }
    try {
        return saver->save(image, directory, basename, path, pathSize);
    } catch (...) {
        // 异常绝不跨越 DLL 边界（document/ABI_POLICY.md §2 第 6 条）：
        // 内存/文件系统的意外都收敛成"保存失败"。
        return 3;
    }
}

std::uint32_t image_config_count(void* self) {
    auto* saver = static_cast<ImageSaver*>(state_of(self));
    return (saver == nullptr) ? 0 : saver->configCount();
}

std::int32_t image_config_desc(void* self, std::uint32_t index, ShotConfigDesc* out) {
    auto* saver = static_cast<ImageSaver*>(state_of(self));
    if (saver == nullptr) {
        return 1;
    }
    return saver->configDesc(index, out);
}

std::int32_t image_get_config(void* self, const char* key, char* buf, std::uint32_t bufSize) {
    auto* saver = static_cast<ImageSaver*>(state_of(self));
    if (saver == nullptr) {
        return -1;
    }
    try {
        return saver->getConfig(key, buf, bufSize);
    } catch (...) {
        return -1;
    }
}

std::int32_t image_set_config(void* self, const char* key, const char* value) {
    auto* saver = static_cast<ImageSaver*>(state_of(self));
    if (saver == nullptr) {
        return -1;
    }
    try {
        return saver->setConfig(key, value);
    } catch (...) {
        return -1;
    }
}

void* create_state() {
    // new (nothrow) 只保证分配失败返回 nullptr；构造里的字符串分配仍可能抛异常，
    // 而 create 是宿主调进来的 ABI 边界，异常绝不能穿过去。
    try {
        return new (std::nothrow) ImageSaver();
    } catch (...) {
        return nullptr;
    }
}

void destroy_state(void* self) {
    try {
        delete static_cast<ImageSaver*>(self);
    } catch (...) {
        // 析构链理论上不抛；真抛了也只能就地吞掉，不能跨出 DLL 边界。
    }
}

// SHOT_OPS 负责填对 struct_size / version（漏填等于整张表不存在）。
const ShotImageSaverV1 kSaverOps = SHOT_OPS(ShotImageSaverV1, .save = &image_save);

const ShotConfigurableV1 kConfigOps = SHOT_OPS(ShotConfigurableV1,
                                               .get_config_count = &image_config_count,
                                               .get_config_desc  = &image_config_desc,
                                               .get_config       = &image_get_config,
                                               .set_config       = &image_set_config);

const ShotIfaceEntry kIfaces[] = {
    { SHOT_IFACE_IMAGE_SAVER, 1, &kSaverOps },
    { SHOT_IFACE_CONFIGURABLE, 1, &kConfigOps },
    { nullptr, 0, nullptr },
};

}  // namespace

const PluginEntry kImageSaver = {
    "image_saver.main",
    "Image Saver",
    "0.1.0",
    &create_state,
    &destroy_state,
    kIfaces,
};

}  // namespace image_saver
