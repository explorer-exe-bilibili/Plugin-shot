// BGRA8 → PNG/JPEG 编码。
//
// 分两步：
//   1. 按 stride 逐行把 BGRA 转成紧凑缓冲（PNG 用 RGBA、JPEG 用 RGB）——
//      ShotImage 的 stride 可以为负（自底向上存储）、也可以带行尾填充，把 data
//      当连续块是这类插件最常见的越界来源；
//   2. 用 stb_image_write 的 *_to_func 接口写进自己的 ofstream。
//
// 为什么不用 stb 的按文件名接口：它吃 char* 路径（Windows 上是 ANSI），中文目录会
// 打不开；先用 std::filesystem::path 的 UTF-8 构造把路径变成宽字符再开流，才能
// 跨平台支持 UTF-8 路径。
//
// 定义 STBI_WRITE_NO_STDIO：本 TU 只用 *_to_func，不需要 stb 的 fopen 版本，顺便
// 避开 MSVC /W4 下第三方头里 CRT 调用产生的 C4996。

#include "image_encoder.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

#include "output_path.h"

// stb_image_write 是 header-only 第三方库：实现只能在一个 TU 里展开一次。
// /W4 下 stb 自身有若干转换/未引用告警，只在包含它的范围内屏蔽，本插件自己的代码
// 仍然按 /W4 保持干净。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4244 4245 4267 4305 4324 4456 4457 4505 4701 4702 4706 4996)
#endif
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace image_saver {
namespace {

constexpr std::int32_t kChannelsBgra = 4;
constexpr int kPngChannels = 4;   // RGBA
constexpr int kJpegChannels = 3;  // RGB（JPEG 没有 alpha 通道）

}  // namespace

bool ImageEncoder::supportsFormat(std::string_view format) noexcept {
    return format == "png" || format == "jpeg";
}

const char* ImageEncoder::extensionFor(std::string_view format) noexcept {
    // 扩展名和格式名成对出现；不认识的格式在 save 里已经回退 PNG，这里保持一致。
    return (format == "jpeg") ? ".jpg" : ".png";
}

std::int32_t ImageEncoder::validate(const ShotImage& image) noexcept {
    // 老宿主的 ShotImage 可能比当前契约短：先确认字段存在再读，绝不越界读。
    if (image.struct_size < SHOT_REQUIRES(ShotImage, channels)) {
        return 1;
    }
    if (image.data == nullptr || image.width <= 0 || image.height <= 0) {
        return 1;
    }
    if (image.channels != kChannelsBgra) {
        return 1;  // v1 只定义 BGRA8
    }
    // 老宿主的 ShotImage 里可能没有 pixel_format；那种情况按当年约定用 channels == 4
    // 当成 BGRA8（新插件 + 老宿主的降级路径）。
    if (SHOT_HAS_FIELD(&image, ShotImage, pixel_format) &&
        image.pixel_format != SHOT_PIXEL_FORMAT_BGRA8) {
        return 1;
    }
    // stride 可以为负（自底向上存储），绝对值必须装得下一行有效像素。
    const std::int64_t rowBytes = static_cast<std::int64_t>(image.width) * kChannelsBgra;
    const std::int64_t stride = static_cast<std::int64_t>(image.stride);
    if (stride >= 0 ? stride < rowBytes : -stride < rowBytes) {
        return 1;
    }
    return 0;
}

bool ImageEncoder::encode(const ShotImage& image, std::string_view format, int quality,
                          const std::filesystem::path& path) {
    const bool wantJpeg = (format == "jpeg");
    std::vector<std::uint8_t> pixels;
    const bool converted = wantJpeg ? convertToRgb(image, pixels) : convertToRgba(image, pixels);
    if (!converted) {
        return false;
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return false;
    }

    int written = 0;
    if (wantJpeg) {
        const int clampedQuality = (quality < 1) ? 1 : ((quality > 100) ? 100 : quality);
        written = stbi_write_jpg_to_func(&ImageEncoder::writeToStream, &stream, image.width,
                                         image.height, kJpegChannels, pixels.data(),
                                         clampedQuality);
    } else {
        written = stbi_write_png_to_func(&ImageEncoder::writeToStream, &stream, image.width,
                                         image.height, kPngChannels, pixels.data(),
                                         image.width * kPngChannels);
    }

    stream.flush();
    const bool ok = (written != 0) && stream.good();
    stream.close();
    if (!ok) {
        // 失败时删掉写了一半的文件，不留垃圾。这个文件名是刚挑出来的空名字
        // （makeUnique 保证请求前不存在），删掉不会误伤用户已有文件。
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    return ok;
}

bool ImageEncoder::convertToRgba(const ShotImage& image, std::vector<std::uint8_t>& out) {
    if (validate(image) != 0) {
        return false;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * kPngChannels;
    out.resize(rowBytes * static_cast<std::size_t>(image.height));
    const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(image.stride);
    for (std::int32_t y = 0; y < image.height; ++y) {
        // 行首按 stride 寻址：负 stride 时 data 指向内存里第一行、后续行朝低地址走，
        // 输出缓冲仍然按 y 递增写，得到自上而下的紧凑像素。
        const std::uint8_t* src = image.data + static_cast<std::ptrdiff_t>(y) * stride;
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(y) * rowBytes;
        for (std::int32_t x = 0; x < image.width; ++x) {
            // 内存顺序是 B、G、R、A，PNG 要 R、G、B、A。
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
            src += kChannelsBgra;
            dst += kPngChannels;
        }
    }
    return true;
}

bool ImageEncoder::convertToRgb(const ShotImage& image, std::vector<std::uint8_t>& out) {
    if (validate(image) != 0) {
        return false;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * kJpegChannels;
    out.resize(rowBytes * static_cast<std::size_t>(image.height));
    const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(image.stride);
    for (std::int32_t y = 0; y < image.height; ++y) {
        const std::uint8_t* src = image.data + static_cast<std::ptrdiff_t>(y) * stride;
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(y) * rowBytes;
        for (std::int32_t x = 0; x < image.width; ++x) {
            // JPEG 丢弃 alpha：截图场景 alpha 恒为不透明，丢掉不改变视觉结果。
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            src += kChannelsBgra;
            dst += kJpegChannels;
        }
    }
    return true;
}

void ImageEncoder::writeToStream(void* context, void* data, int size) {
    if (context == nullptr || data == nullptr || size <= 0) {
        return;
    }
    static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data),
                                                static_cast<std::streamsize>(size));
}

}  // namespace image_saver
