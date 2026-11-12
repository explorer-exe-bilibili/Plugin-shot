// 图片文件 → PNG + text/uri-list：X11 CLIPBOARD 的数据准备。
//
// 为什么先编码 PNG：X11 剪贴板没有"系统帮你转格式"这回事，owner 必须按请求里
// 的 target 返回字节；image/png 是桌面环境里兼容性最好的图片 target（相比
// image/bmp / image/x-MS-bmp 等）。PNG 编码只在截图落盘时做一次，之后每次粘贴
// 都复用同一份字节。
//
// 为什么 text/uri-list 要自己做百分号编码：Linux 的"拖文件"协议是 RFC 2483 的
// URI 列表，路径里的空格、中文（UTF-8 多字节）必须按 RFC 3986 转义，否则文件
// 管理器会把一个路径拆成多个 token。
//
// 失败语义与 Windows 版本一致：读不到文件、解码失败、编码失败都只返回 false +
// 中文原因，由调用方记 WARN；截图文件已经落盘，下一次 capture.done 会重试。

#include "clipboard_image.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>

#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace clipboard {
namespace {

// 图片文件上限：一次截屏的 PNG/JPEG 远小于这个值；设上限是防御"事件里的路径指向
// 一个巨大的非图片文件"时把内存吃爆（stb 解码前还要整读一遍）。
constexpr std::uintmax_t kMaxImageFileBytes = 512u * 1024u * 1024u;

// stbi_load_from_memory 的长度参数是 int；文件上限远小于 INT_MAX，这里只是让
// 转换显式化。
constexpr int kMaxStbInputBytes = std::numeric_limits<int>::max();

// XChangeProperty 的 nelements 也是 int：编码结果超过 INT_MAX 不可能写进 X11。
constexpr std::size_t kMaxPngBytes = static_cast<std::size_t>(kMaxStbInputBytes);

/**
 * @brief 把 UTF-8 文本转成 std::filesystem::path。
 *
 * C++20 的 char8_t 构造按 UTF-8 解释字节；Linux 上 path(std::string) 虽然也是
 * 原样字节，但走 char8_t 能让这段代码与 Windows 版本保持同一种写法（不会退化成
 * 平台本地编码）。
 */
std::filesystem::path fromUtf8(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const std::u8string text(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(text);
}

/**
 * @brief 读整个文件到字节缓冲。
 * @param[in]  utf8Path UTF-8 路径。
 * @param[out] bytes    读到的字节。
 * @param[out] error    失败时写入中文原因。
 * @return true = 读取完成。
 */
bool readFileBytes(const std::string& utf8Path, std::vector<std::uint8_t>& bytes,
                   std::string& error) {
    const std::filesystem::path path = fromUtf8(utf8Path);
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        error = "打开图片文件失败：" + utf8Path;
        return false;
    }
    const std::streamoff size = stream.tellg();
    if (size <= 0) {
        error = "图片文件为空或无法读取大小：" + utf8Path;
        return false;
    }
    if (static_cast<std::uintmax_t>(size) > kMaxImageFileBytes) {
        error = "图片文件过大（超过 512 MiB），拒绝读取：" + utf8Path;
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream) {
        error = "读取图片文件失败：" + utf8Path;
        return false;
    }
    return true;
}

/**
 * @brief stb_image_write 的写出回调：把一段编码结果追加到 vector。
 * @param[in] context 目标 vector（由 stbi_write_png_to_func 原样传回）。
 * @param[in] data    stb 写出的字节。
 * @param[in] size    字节数。
 */
void appendPng(void* context, void* data, int size) {
    if (context == nullptr || data == nullptr || size <= 0) {
        return;
    }
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* first = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), first, first + size);
}

/** @brief 判断字节是否属于 RFC 3986 的 unreserved 集合（路径里可以原样保留）。 */
bool isUnreserved(unsigned char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' ||
           value == '~';
}

/**
 * @brief 把 UTF-8 路径按 RFC 3986 百分号编码。
 *
 * 只保留 unreserved 与 '/'；其它字节（空格、中文、'#'、'?' 等）全部转成 %XX，
 * 否则 text/uri-list 的接收方会把 '#'、'?' 当成 URI 的 fragment / query。
 */
std::string percentEncodePath(std::string_view path) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(path.size());
    for (const unsigned char value : path) {
        if (isUnreserved(value) || value == '/') {
            encoded.push_back(static_cast<char>(value));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[(value >> 4) & 0x0F]);
        encoded.push_back(kHex[value & 0x0F]);
    }
    return encoded;
}

}  // namespace

std::string ClipboardImage::makeFileUriList(const std::string& utf8Path) {
    if (utf8Path.empty()) {
        return {};
    }
    const std::filesystem::path path = fromUtf8(utf8Path);
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec || absolute.empty()) {
        // 取不到绝对路径时仍按原样拼：总比完全不提供 text/uri-list 好，接收方
        // 至少能看到文件名；路径能不能打开由接收方决定。
        absolute = path;
    }
    const std::u8string native = absolute.generic_u8string();
    std::string text(reinterpret_cast<const char*>(native.data()), native.size());
    if (text.empty()) {
        return {};
    }
    if (text.front() != '/') {
        text.insert(text.begin(), '/');
    }
    return "file://" + percentEncodePath(text) + "\r\n";
}

bool ClipboardImage::build(const std::string& utf8Path, bool alsoFileDrop, Payload& out,
                           std::string& error) {
    error.clear();
    out = Payload{};
    if (utf8Path.empty()) {
        error = "capture.done 没有给出文件路径";
        return false;
    }

    std::vector<std::uint8_t> fileBytes;
    if (!readFileBytes(utf8Path, fileBytes, error)) {
        return false;
    }
    if (fileBytes.size() > static_cast<std::size_t>(kMaxStbInputBytes)) {
        error = "图片文件超过 stb 可接受的长度上限";
        return false;
    }

    int width = 0;
    int height = 0;
    int channelsInFile = 0;
    std::unique_ptr<stbi_uc, void (*)(void*)> pixels(
        stbi_load_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &width, &height,
                              &channelsInFile, 4),
        &stbi_image_free);
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        error = std::string("stb_image 解码失败") +
                (reason != nullptr ? (": " + std::string(reason)) : "");
        return false;
    }

    if (stbi_write_png_to_func(&appendPng, &out.png, width, height, 4, pixels.get(),
                               width * 4) == 0 ||
        out.png.empty()) {
        error = "stb_image_write 编码 PNG 失败";
        out = Payload{};
        return false;
    }
    if (out.png.size() > kMaxPngBytes) {
        error = "PNG 编码结果超过 X11 属性的长度上限";
        out = Payload{};
        return false;
    }

    if (alsoFileDrop) {
        out.uriList = makeFileUriList(utf8Path);
        // 路径已经被成功读成图片，正常情况下绝对路径一定能拼出来；这里仍然按
        // "拿不到就不提供 uri-list"降级，不因为一个可选 target 丢掉整张图。
        out.hasUriList = !out.uriList.empty();
    }
    return true;
}

}  // namespace clipboard
