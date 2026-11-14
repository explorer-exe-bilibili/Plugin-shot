// 图像保存模板实现：BGRA8 → PNG / JPEG / BMP。
//
// 编码用 SDK 内置的 stb_image_write（third_party/include，header-only）：
//   · *_to_func 接口把编码结果写回调，本文件回调给 std::ofstream；
//   · 这样完全绕开 stb 的 from-file 接口（Windows 上它走 fopen + ANSI 代码页，
//     非 ASCII 目录会失败），路径统一用 std::filesystem / ofstream 处理。
//
// 必做转换：ShotImage 是 BGRA8，而 PNG/JPEG/BMP 的内存顺序是 RGBA，所以先按行
// 把像素拷进一块连续缓冲（顺便把负 stride / 行尾填充抹平），再交给 stb。
//
// 返回码遵循 include/plugin_api/image_saver.h：
//   0 = 已落盘；1 = 参数/图像格式非法；2 = path 缓冲不足；3 = 保存失败。
//
// label / tooltip 写的是语言表 key，不是中文：设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析，文案本体在 kLangTables。
// ENUM 的候选值（format.png / format.jpeg / format.bmp）本身也是 i18n key。

// stb_image_write 内部用 sprintf（它自己的跨平台兼容写法），MSVC /W4 会报 C4996
// "sprintf 不安全"。这是 SDK 内置第三方头的已知噪音，在包含它之前关掉该弃用警告。
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "image_saver_plugin.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "plugin_entry.h"

namespace image_saver_template {
namespace {

const char kKeyFormat[] = "format";
const char kKeyQuality[] = "quality";
const char kKeyDirectory[] = "directory";

// 默认值只在描述符里写一遍，构造函数也从这几个常量取，避免两处漂移。
const char kDefaultFormat[] = "format.png";
const char kDefaultQuality[] = "90";
const char kDefaultDirectory[] = "";  // 空 = 当前工作目录（宿主不保证有"图片"目录）

constexpr int kMinQuality = 1;
constexpr int kMaxQuality = 100;

// ENUM 候选值：带命名空间、同时是 i18n key；内部解析用 formatOption() 归一。
const char kOptions[] = "format.png|format.jpeg|format.bmp";

const char kLabelFormat[] = "config.format.label";
const char kTooltipFormat[] = "config.format.tooltip";
const char kLabelQuality[] = "config.quality.label";
const char kTooltipQuality[] = "config.quality.tooltip";
const char kLabelDirectory[] = "config.directory.label";
const char kTooltipDirectory[] = "config.directory.tooltip";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeyFormat,
        .label         = kLabelFormat,
        .default_value = kDefaultFormat,
        .options       = kOptions,
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipFormat,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_INT,
        .key           = kKeyQuality,
        .label         = kLabelQuality,
        .default_value = kDefaultQuality,
        .options       = nullptr,
        .min           = static_cast<double>(kMinQuality),
        .max           = static_cast<double>(kMaxQuality),
        .step          = 1.0,
        .tooltip       = kTooltipQuality,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_PATH,
        .key           = kKeyDirectory,
        .label         = kLabelDirectory,
        .default_value = kDefaultDirectory,
        // PATH 借 options 声明对话框类型：空默认值时不写会被推断成"选文件"。
        .options       = "directory",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipDirectory,
    },
};

// 语言表：每语言一张单行 JSON（@meta 必填）；整行会超过 120 列，所以用相邻
// raw 字面量拼接——拼出来仍然是一行合法 JSON。各语言的 key 集必须一致。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"{{PLUGIN_NAME}}",)json"
    R"json("plugin.description":"{{PLUGIN_DESCRIPTION}}",)json"
    R"json("config.format.label":"图片格式",)json"
    R"json("config.format.tooltip":"落盘文件格式；JPEG 有损、PNG 无损、BMP 不压缩",)json"
    R"json("format.png":"PNG（无损）",)json"
    R"json("format.jpeg":"JPEG（有损）",)json"
    R"json("format.bmp":"BMP（不压缩）",)json"
    R"json("config.quality.label":"JPEG 质量",)json"
    R"json("config.quality.tooltip":"只对 JPEG 生效：1 = 最省空间，100 = 画质最好",)json"
    R"json("config.directory.label":"保存目录",)json"
    R"json("config.directory.tooltip":"留空 = 当前工作目录；目录不存在时会自动创建"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"{{PLUGIN_NAME}}",)json"
    R"json("plugin.description":"{{PLUGIN_DESCRIPTION}}",)json"
    R"json("config.format.label":"Format",)json"
    R"json("config.format.tooltip":"Output file format: JPEG is lossy, PNG is lossless, BMP is uncompressed",)json"
    R"json("format.png":"PNG (lossless)",)json"
    R"json("format.jpeg":"JPEG (lossy)",)json"
    R"json("format.bmp":"BMP (uncompressed)",)json"
    R"json("config.quality.label":"JPEG quality",)json"
    R"json("config.quality.tooltip":"JPEG only: 1 = smallest file, 100 = best quality",)json"
    R"json("config.directory.label":"Output directory",)json"
    R"json("config.directory.tooltip":"Empty = current working directory; missing directories ")json"
    R"json("are created automatically"})json",
};

/**
 * @brief 把文字值以 UTF-8 写进调用方缓冲。
 * @param[in]  value   要写入的文本。
 * @param[out] buf     接收缓冲；可为 nullptr（只查长度）。
 * @param[in]  bufSize buf 的字节数。
 * @return >= 0 = 值的字节数（不含 NUL）；< 0 = value 为空。
 */
std::int32_t copyValue(const char* value, char* buf, std::uint32_t bufSize) {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    if (buf != nullptr && bufSize > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

/**
 * @brief 把静态描述符拷进宿主提供的 ShotConfigDesc。
 * @param[in]  src 静态描述符。
 * @param[out] out 宿主输出结构（已填 struct_size）。
 * @return 0 = 成功；1 = 参数为空；2 = 宿主结构太短。
 */
std::int32_t copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (out == nullptr || src == nullptr) {
        return 1;
    }
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = *src;
    return 0;
}

/** @brief 把 ENUM 候选值（format.png 等）解析成内部格式；不认识的值回退 PNG。 */
SaverFormat parseFormat(const char* value) {
    if (value == nullptr) {
        return SaverFormat::Png;
    }
    // 同时接受"带命名空间的候选值"和裸技术名，方便手改配置的人。
    if (std::strcmp(value, "format.jpeg") == 0 || std::strcmp(value, "jpeg") == 0 ||
        std::strcmp(value, "jpg") == 0) {
        return SaverFormat::Jpeg;
    }
    if (std::strcmp(value, "format.bmp") == 0 || std::strcmp(value, "bmp") == 0) {
        return SaverFormat::Bmp;
    }
    return SaverFormat::Png;
}

/** @brief 内部格式 → 配置里存的值（带命名空间的 i18n key）。 */
const char* formatOption(SaverFormat format) {
    switch (format) {
        case SaverFormat::Jpeg: return "format.jpeg";
        case SaverFormat::Bmp: return "format.bmp";
        case SaverFormat::Png: break;
    }
    return "format.png";
}

/** @brief 内部格式 → 文件扩展名（含点）。 */
const char* formatExtension(SaverFormat format) {
    switch (format) {
        case SaverFormat::Jpeg: return ".jpg";
        case SaverFormat::Bmp: return ".bmp";
        case SaverFormat::Png: break;
    }
    return ".png";
}

/** @brief 把十进制文本解析成整数并夹到 [min, max]。 */
int parseClampedInt(const char* value, int min, int max, int fallback) {
    if (value == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || end == nullptr || *end != '\0') {
        return fallback;
    }
    if (parsed < min) {
        return min;
    }
    if (parsed > max) {
        return max;
    }
    return static_cast<int>(parsed);
}

/** @brief UTF-8 文本 → std::filesystem::path（Windows 上必须走 u8string 才是真 UTF-8）。 */
std::filesystem::path pathFromUtf8(const std::string& text) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
    return std::filesystem::path(text);
#endif
}

/** @brief std::filesystem::path → UTF-8 文本（capture.done / 日志里要的是 UTF-8）。 */
std::string toUtf8(const std::filesystem::path& path) {
#if defined(__cpp_char8_t)
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return path.u8string();
#endif
}

/** @brief 默认文件名：shot_YYYYMMDD_HHMMSS（本地时间，线程安全版本按平台选）。 */
std::string defaultBasename() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32] = {};
    if (std::strftime(buffer, sizeof(buffer), "shot_%Y%m%d_%H%M%S", &local) == 0) {
        return "shot";
    }
    return buffer;
}

/**
 * @brief 校验图像能不能按 BGRA8 处理。
 * @param[in]  image 宿主给的图像。
 * @param[out] error 失败原因（中文）。
 * @return true = 可以处理；false = 参数/格式不支持。
 */
bool validateImage(const ShotImage* image, std::string& error) {
    if (image == nullptr || image->data == nullptr) {
        error = "图像或像素指针为空";
        return false;
    }
    if (image->width < 1 || image->height < 1) {
        error = "图像尺寸非法";
        return false;
    }
    // TODO: 目前只支持 BGRA8；将来契约新增 HDR / 浮点像素格式时，这里按 pixel_format 分支。
    if (!SHOT_HAS_FIELD(image, ShotImage, pixel_format) ||
        image->pixel_format != SHOT_PIXEL_FORMAT_BGRA8) {
        error = "像素格式不是 BGRA8";
        return false;
    }
    if (image->channels != 4) {
        error = "每像素通道数不是 4";
        return false;
    }
    const std::int32_t rowBytes = image->width * 4;
    const std::int32_t absStride = image->stride < 0 ? -image->stride : image->stride;
    if (absStride < rowBytes) {
        error = "stride 比一行像素还短";
        return false;
    }
    return true;
}

/** @brief 把 BGRA8 图像拷成连续内存的 RGBA（includeAlpha = false 时只留 RGB）。 */
std::vector<std::uint8_t> toRgba(const ShotImage& image, bool includeAlpha) {
    const std::size_t channels = includeAlpha ? 4 : 3;
    const std::size_t rowPixels = static_cast<std::size_t>(image.width);
    std::vector<std::uint8_t> buffer(rowPixels * static_cast<std::size_t>(image.height) * channels);
    std::uint8_t* dstRow = buffer.data();
    for (std::int32_t y = 0; y < image.height; ++y) {
        // stride 可负（自底向上存储）：data + y*stride 仍然指向第 y 行。
        const std::uint8_t* srcRow =
            image.data + static_cast<std::ptrdiff_t>(y) * image.stride;
        for (std::int32_t x = 0; x < image.width; ++x) {
            const std::uint8_t* srcPixel = srcRow + static_cast<std::ptrdiff_t>(x) * 4;
            std::uint8_t* dstPixel = dstRow + static_cast<std::ptrdiff_t>(x) * channels;
            dstPixel[0] = srcPixel[2];  // R
            dstPixel[1] = srcPixel[1];  // G
            dstPixel[2] = srcPixel[0];  // B
            if (includeAlpha) {
                dstPixel[3] = srcPixel[3];  // A
            }
        }
        dstRow += rowPixels * channels;
    }
    return buffer;
}

/** @brief stb 的写回调：把编码出来的字节追加进 std::ofstream。 */
void writeToStream(void* context, void* data, int size) {
    auto* out = static_cast<std::ofstream*>(context);
    if (out != nullptr && data != nullptr && size > 0) {
        out->write(static_cast<const char*>(data), size);
    }
}

}  // namespace

ImageSaverPlugin::ImageSaverPlugin()
    : m_format(parseFormat(kDefaultFormat)),
      m_quality(parseClampedInt(kDefaultQuality, kMinQuality, kMaxQuality, 90)),
      m_directory(kDefaultDirectory) {}

ImageSaverPlugin::~ImageSaverPlugin() {
    stop();
}

bool ImageSaverPlugin::start(const ShotHostServices* host) {
    m_host = host;
    // 语言表要在宿主生成 config_list 快照之前发布；create() 拿不到服务表，
    // 这里是第一个能拿到 host 的生命周期路径。
    m_i18n = std::make_unique<shot::I18n>(host, kSaver.id);
    if (!m_i18n->loadTables(kLangTables, std::size(kLangTables))) {
        shot::Logger(host).warn("[{{TARGET}}] 语言表解析失败，配置项文案回退为 key");
    }
    shot::Logger(host).info("[{{TARGET}}] 图像保存插件就绪（PNG / JPEG / BMP）");
    return true;
}

void ImageSaverPlugin::stop() {
    m_i18n.reset();
}

std::int32_t ImageSaverPlugin::save(const ShotImage* image, const char* directory,
                                    const char* basename, char* path,
                                    std::uint32_t pathSize) {
    if (path == nullptr || pathSize == 0) {
        return 1;
    }
    std::string error;
    if (!validateImage(image, error)) {
        shot::Logger(m_host).warn("[{{TARGET}}] save 已拒绝：" + error);
        return 1;
    }

    const Settings settings = settingsSnapshot();

    // 覆盖参数优先；空值视为"用插件配置 / 默认策略"（契约：NULL = 用配置）。
    std::string directoryText =
        (directory != nullptr && directory[0] != '\0') ? std::string(directory) : settings.directory;
    std::string basenameText =
        (basename != nullptr && basename[0] != '\0') ? std::string(basename) : defaultBasename();
    const char* extension = formatExtension(settings.format);

    std::error_code fsError;
    std::filesystem::path dirPath;
    if (directoryText.empty()) {
        dirPath = std::filesystem::current_path(fsError);
    } else {
        dirPath = pathFromUtf8(directoryText);
    }
    if (fsError) {
        shot::Logger(m_host).error("[{{TARGET}}] 取当前目录失败：" + fsError.message());
        return 3;
    }
    std::filesystem::create_directories(dirPath, fsError);
    if (fsError) {
        shot::Logger(m_host).error("[{{TARGET}}] 创建输出目录失败：" + fsError.message());
        return 3;
    }

    // 同名文件不覆盖：依次尝试 name_1、name_2……最多 1000 次，避免误删已有截图。
    std::filesystem::path filePath = dirPath / (basenameText + extension);
    for (int suffix = 1; std::filesystem::exists(filePath, fsError) && suffix < 1000; ++suffix) {
        filePath = dirPath / (basenameText + "_" + std::to_string(suffix) + extension);
    }
    if (std::filesystem::exists(filePath, fsError)) {
        shot::Logger(m_host).error("[{{TARGET}}] 同名文件过多，放弃保存：" + toUtf8(filePath));
        return 3;
    }

    if (!encode(*image, settings, toUtf8(filePath), error)) {
        shot::Logger(m_host).error("[{{TARGET}}] 保存失败：" + error);
        return 3;
    }

    // path 缓冲不足时返回 2（不要截断路径）：宿主据此知道该给更大的缓冲。
    const std::string finalPath = toUtf8(filePath);
    if (finalPath.size() + 1 > pathSize) {
        shot::Logger(m_host).warn("[{{TARGET}}] 回写路径失败：缓冲不足");
        return 2;
    }
    std::memcpy(path, finalPath.c_str(), finalPath.size() + 1);
    shot::Logger(m_host).info("[{{TARGET}}] 已保存：" + finalPath);
    return 0;
}

ImageSaverPlugin::Settings ImageSaverPlugin::settingsSnapshot() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    Settings settings;
    settings.format = m_format;
    settings.quality = m_quality;
    settings.directory = m_directory;
    return settings;
}

bool ImageSaverPlugin::encode(const ShotImage& image, const Settings& settings,
                              const std::string& filePath, std::string& error) const {
    // 用 ofstream + 回调写文件：MSVC 的 ofstream 直接接受宽字符路径，非 ASCII 目录没问题。
    std::ofstream out(pathFromUtf8(filePath), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        error = "无法打开输出文件（目录不可写？）";
        return false;
    }

    int written = 0;
    if (settings.format == SaverFormat::Jpeg) {
        // JPEG 不支持 alpha：只送 3 通道，避免 stb 对 A 通道做未定义处理。
        std::vector<std::uint8_t> rgb = toRgba(image, false);
        written = stbi_write_jpg_to_func(&writeToStream, &out, image.width, image.height, 3,
                                         rgb.data(), settings.quality);
    } else if (settings.format == SaverFormat::Bmp) {
        std::vector<std::uint8_t> rgba = toRgba(image, true);
        written = stbi_write_bmp_to_func(&writeToStream, &out, image.width, image.height, 4,
                                         rgba.data());
    } else {
        std::vector<std::uint8_t> rgba = toRgba(image, true);
        written = stbi_write_png_to_func(&writeToStream, &out, image.width, image.height, 4,
                                         rgba.data(), image.width * 4);
    }
    out.flush();
    if (written == 0 || !out.good()) {
        error = "stb 编码或写盘失败";
        return false;
    }
    return true;
}

std::uint32_t ImageSaverPlugin::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t ImageSaverPlugin::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t ImageSaverPlugin::getConfig(const char* key, char* buf,
                                         std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (std::strcmp(key, kKeyFormat) == 0) {
        return copyValue(formatOption(m_format), buf, bufSize);
    }
    if (std::strcmp(key, kKeyQuality) == 0) {
        return copyValue(std::to_string(m_quality).c_str(), buf, bufSize);
    }
    if (std::strcmp(key, kKeyDirectory) == 0) {
        return copyValue(m_directory.c_str(), buf, bufSize);
    }
    return -1;  // 不认识的 key：安全忽略
}

std::int32_t ImageSaverPlugin::setConfig(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (std::strcmp(key, kKeyFormat) == 0) {
        m_format = parseFormat(value);
        return 0;
    }
    if (std::strcmp(key, kKeyQuality) == 0) {
        m_quality = parseClampedInt(value, kMinQuality, kMaxQuality, m_quality);
        return 0;
    }
    if (std::strcmp(key, kKeyDirectory) == 0) {
        m_directory = value;
        return 0;
    }
    return -1;
}

}  // namespace image_saver_template
