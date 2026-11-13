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
#include <memory>
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

// 本插件的用户可见文案：自己的显示名/描述（plugin.name / plugin.description，
// 设置页配置分区标题用）、四个配置项的 label/tooltip，以及 ENUM 候选值
// （候选值本身直接当 key，如 format.png；查不到时设置页显示原始值）。
// 配置描述符里写的是这些 key（见 saver_config.cpp），设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析；key 拼写必须与
// saver_config.cpp 完全一致，拼错会原样显示 key。
// 每语言一张 JSON 对象，@meta 必填，两张表的 key 集必须一致；JSON 文本里不能有
// 裸换行，而整行又可能超过 120 列，所以用相邻 raw 字面量拼接——拼出来仍是一行。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"图像保存",)json"
    R"json("plugin.description":"把截到的图像保存为 PNG/JPEG；目录、格式、质量与文件名模板都可配置",)json"
    R"json("config.directory.label":"保存目录",)json"
    R"json("config.directory.tooltip":)json"
    R"json("留空使用系统图片目录（Windows 的「图片」库、Linux 的 XDG 图片目录）",)json"
    R"json("config.format.label":"保存格式",)json"
    R"json("config.format.tooltip":"PNG 无损；JPEG 有损、文件更小（质量见下一项）",)json"
    R"json("format.png":"PNG（无损）",)json"
    R"json("format.jpeg":"JPEG（有损）",)json"
    R"json("config.quality.label":"JPEG 质量",)json"
    R"json("config.quality.tooltip":"1 = 最小文件，100 = 最好画质；PNG 忽略此项",)json"
    R"json("config.filename.label":"文件名模板",)json"
    R"json("config.filename.tooltip":)json"
    R"json("不含扩展名；支持 %Y %m %d %H %M %S，如 shot_%Y%m%d_%H%M%S"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"Image Saver",)json"
    R"json("plugin.description":"Saves the captured image as PNG or JPEG; folder, format, quality and file name template are configurable",)json"
    R"json("config.directory.label":"Save folder",)json"
    R"json("config.directory.tooltip":)json"
    R"json("Leave empty to use the system Pictures folder )json"
    R"json((the Windows Pictures library / the Linux XDG pictures directory)",)json"
    R"json("config.format.label":"Save format",)json"
    R"json("config.format.tooltip":)json"
    R"json("PNG is lossless; JPEG is lossy and produces smaller files )json"
    R"json((quality is the next setting)",)json"
    R"json("format.png":"PNG (lossless)",)json"
    R"json("format.jpeg":"JPEG (lossy)",)json"
    R"json("config.quality.label":"JPEG quality",)json"
    R"json("config.quality.tooltip":)json"
    R"json("1 = smallest file, 100 = best quality; ignored for PNG",)json"
    R"json("config.filename.label":"File name template",)json"
    R"json("config.filename.tooltip":)json"
    R"json("Without extension; supports %Y %m %d %H %M %S, )json"
    R"json(e.g. shot_%Y%m%d_%H%M%S"})json",
};

}  // namespace

ImageSaver::ImageSaver() = default;

ImageSaver::~ImageSaver() { stop(); }

bool ImageSaver::start(const ShotHostServices* host) {
    // I18n 构造会订阅 lists.changed、loadTables 会发布 lists.add，两者都可能分配内存；
    // 本方法由 ABI 边界 create_instance 调用，异常绝不能穿过去，所以在这里收敛成 false。
    try {
        // owner 用插件身份证，避免与 metadata.json / kImageSaver 里的字符串漂移。
        auto i18n = std::make_unique<shot::I18n>(host, kImageSaver.id);
        if (!i18n->loadTables(kLangTables, std::size(kLangTables))) {
            shot::Logger(host).warn("[image_saver] 语言表解析失败，配置项文案回退为 key");
        }
        m_i18n = std::move(i18n);
        return true;
    } catch (...) {
        m_i18n.reset();
        shot::Logger(host).warn("[image_saver] 初始化多语言失败，配置项文案回退为 key");
        return false;
    }
}

void ImageSaver::stop() {
    // I18n 析构会退订 lists.changed：必须在宿主卸载 DLL 之前完成（destroy 路径）。
    m_i18n.reset();
}

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
    auto* saver = static_cast<ImageSaver*>(self);
    if (saver == nullptr) {
        return;
    }
    try {
        // 先退订 lists.changed（语言表析构），再释放状态；宿主之后可能 FreeLibrary
        // 本 DLL，destroy 返回后不能再有指向插件代码的回调。
        saver->stop();
    } catch (...) {
        // 析构链理论上不抛；真抛了也只能就地吞掉，不能跨出 DLL 边界。
    }
    try {
        delete saver;
    } catch (...) {
        // 与上面同理：delete 里的析构异常也不能越过 create/destroy 边界。
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
