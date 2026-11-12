// 截屏帧解码：std::ifstream 读文件 + stbi_load_from_memory 解码成 RGBA。
//
// 为什么不用 stb 的按文件名接口：它在 Windows 上把 char* 当 ANSI，
// %USERPROFILE% 里有中文用户名时打不开（与 image_saver / clipboard 踩过的是
// 同一个坑）。用 std::filesystem::path 的 u8string 构造把 UTF-8 转成宽字符、
// 自己开流读内存，才能跨平台支持 UTF-8 路径。
//
// 定义 STBI_NO_STDIO：本 TU 只用 *_from_memory，不需要 stb 的 fopen 版本，
// 顺便避开 MSVC /W4 下第三方头里 CRT 调用产生的 C4996。
//
// 本文件不包含任何平台头（解码是平台无关核心）。

#include "frame_loader.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_MSC_VER)
// stb_image 自身在 /W4 下有若干转换/未引用告警；只在这一段包含范围内屏蔽，
// 本插件自己的代码仍然按 /W4 保持干净（与 image_saver 同一写法）。
#pragma warning(push)
#pragma warning(disable : 4100 4244 4245 4267 4305 4324 4456 4457 4505 4701 4702 4706 4996)
#endif
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace long_capture {
namespace {

/** @brief 单个帧文件的大小上限：长截图单帧远小于它，超过多半是路径指向了别的东西。 */
constexpr std::uintmax_t kMaxFrameBytes = 64ULL * 1024ULL * 1024ULL;

/** @brief 按 UTF-8 字节构造 filesystem::path（Windows 上内部转宽字符）。 */
std::filesystem::path fromUtf8(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const std::u8string text(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(text);
}

}  // namespace

bool FrameLoader::readFile(const std::string& utf8Path, std::vector<std::uint8_t>& bytes,
                           std::string& error) {
    error.clear();
    if (utf8Path.empty()) {
        error = "帧文件路径为空";
        return false;
    }
    const std::filesystem::path path = fromUtf8(utf8Path);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "读不到帧文件（文件不存在或没有权限）：" + utf8Path;
        return false;
    }
    if (size == 0 || size > kMaxFrameBytes) {
        error = "帧文件大小异常（0 字节或超过 64 MiB）：" + utf8Path;
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        error = "打不开帧文件：" + utf8Path;
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream || static_cast<std::size_t>(stream.gcount()) != bytes.size()) {
        bytes.clear();
        error = "帧文件读取不完整：" + utf8Path;
        return false;
    }
    return true;
}

bool FrameLoader::load(const std::string& utf8Path, CaptureFrame& out, std::string& error) {
    error.clear();
    std::vector<std::uint8_t> bytes;
    if (!readFile(utf8Path, bytes, error)) {
        return false;
    }
    int width = 0;
    int height = 0;
    // req_comp = 4：不论源文件是 RGB 还是 RGBA，都统一成紧凑 RGBA。
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width,
                                            &height, nullptr, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        error = std::string("解码帧文件失败：") + ((reason != nullptr) ? reason : "未知原因");
        return false;
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        error = "解码结果尺寸非法";
        return false;
    }

    CaptureFrame frame;
    frame.width = width;
    frame.height = height;
    frame.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) *
                                         static_cast<std::size_t>(height) * 4U);
    stbi_image_free(pixels);
    out = std::move(frame);
    return true;
}

}  // namespace long_capture
