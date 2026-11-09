// 图片文件 → Win32 剪贴板（CF_DIB，可选 CF_HDROP）。
//
// 为什么从文件读而不是从宿主拿像素：document/CAPTURE.md §3 明确"像素不进总
// 线"，capture.done 只有 path/width/height/processors；剪贴板是"复制"这一侧
// 的消费者，读保存插件刚落盘的文件正好，不需要新 ABI。
//
// 解码/格式转换的三步（顺序不能反）：
//   1. 自己读文件字节（std::filesystem 把 UTF-8 路径转宽字符；stb 的按文件名接口
//      走 ANSI，中文目录会打不开），再交给 stbi_load_from_memory 解成 RGBA；
//   2. stb 给的是 top-down 的 RGBA，而 CF_DIB 的惯例是 32bpp BGRA + bottom-up
//      行序（biHeight 为正），所以这里换通道顺序并把行倒过来；
//   3. 整块 BITMAPINFOHEADER + 像素放进 GMEM_MOVEABLE 的全局内存，SetClipboardData
//      成功后所有权归系统（失败必须自己 GlobalFree，否则泄漏）。
//
// 失败语义：剪贴板是进程间共享资源，随时可能被别的程序占用；OpenClipboard 最多
// 重试 10 次（每次 20ms），仍失败就返回 false + 中文原因，由调用方记 WARN。文件
// 已经落盘，所以这次失败不影响截屏结果，下一次 capture.done 会重新尝试。

#include "clipboard_writer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <shlobj.h>  // DROPFILES（CF_HDROP 的文件列表头）

// stb_image 是 header-only 第三方库：实现只能在一个 TU 里展开一次。
// /W4 下 stb 自身有若干转换/未引用告警，只在包含它的范围内屏蔽；本插件自己的
// 代码仍然按 /W4 保持干净。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4244 4245 4267 4305 4324 4386 4456 4457 4505 4701 4702 4706 4996)
#endif
#define STBI_NO_STDIO  // 只用内存接口：路径先由 std::filesystem 转宽字符再自己读文件
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace clipboard {
namespace {

// 剪贴板被占用时最多重试 10 次、每次 20ms：用户机器上常见的占用（截图工具、输入法、
// 剪贴板管理器）通常几十毫秒内就释放，超过这个量级继续等只会拖住宿主事件分发线程。
constexpr int kOpenClipboardRetries = 10;
constexpr DWORD kOpenClipboardRetryDelayMs = 20;

// 图片文件上限：一次截屏的 PNG/JPEG 远小于这个值；设上限是防御"事件里的路径指向
// 一个巨大的非图片文件"时把内存吃爆（stb 解码前还要整读一遍）。
constexpr std::uintmax_t kMaxImageFileBytes = 512u * 1024u * 1024u;

// CF_DIB 上限：超过这个尺寸的 DIB 已经不是"截图"该有的体积，直接拒绝而不是尝试分配。
constexpr std::uint64_t kMaxDibBytes = 2ull * 1024ull * 1024ull * 1024ull;  // 2 GiB

// stbi_load_from_memory 的长度参数是 int；文件上限远小于 INT_MAX，这里只是让转换
// 显式化。
constexpr int kMaxStbInputBytes = std::numeric_limits<int>::max();

/**
 * @brief 把 UTF-8 文本转成 std::filesystem::path。
 *
 * C++20 的 char8_t 构造按 UTF-8 解释字节，Windows 上内部再转宽字符；
 * path(std::string) 用的是平台本地窄编码（Windows = ANSI），中文路径会坏。
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
 * @brief 把 stb 解出的 RGBA（top-down）转成 CF_DIB 期望的 32bpp BGRA + bottom-up。
 * @param[in] rgba   stb 输出，每像素 4 字节 R/G/B/A，第一行是图像顶部。
 * @param[in] width  像素宽，必须 > 0。
 * @param[in] height 像素高，必须 > 0。
 * @return DIB 字节（BITMAPINFOHEADER + 像素）；尺寸非法/溢出时返回空 vector。
 */
std::vector<std::uint8_t> makeDib(const std::uint8_t* rgba, int width, int height) {
    if (rgba == nullptr || width <= 0 || height <= 0) {
        return {};
    }
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(width) * 4u;
    const std::uint64_t pixelBytes = rowBytes * static_cast<std::uint64_t>(height);
    const std::uint64_t totalBytes = static_cast<std::uint64_t>(sizeof(BITMAPINFOHEADER)) + pixelBytes;
    if (totalBytes == 0 || totalBytes > kMaxDibBytes) {
        return {};
    }

    std::vector<std::uint8_t> dib(static_cast<std::size_t>(totalBytes), 0);
    auto* header = reinterpret_cast<BITMAPINFOHEADER*>(dib.data());
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = static_cast<LONG>(width);
    // 正数 biHeight = bottom-up 行序：这是 CF_DIB 的通用惯例，资源管理器/Office/
    // 聊天软件都按这个解释；负数（top-down）虽然合法，但兼容性差。
    header->biHeight = static_cast<LONG>(height);
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixelBytes);

    const std::size_t sourceRowBytes = static_cast<std::size_t>(rowBytes);
    std::uint8_t* base = dib.data() + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < height; ++y) {
        // 源第一行是图像顶部，DIB 最后一行才是图像顶部。
        std::uint8_t* destination = base + sourceRowBytes * static_cast<std::size_t>(height - 1 - y);
        const std::uint8_t* source = rgba + sourceRowBytes * static_cast<std::size_t>(y);
        for (int x = 0; x < width; ++x) {
            destination[0] = source[2];  // B
            destination[1] = source[1];  // G
            destination[2] = source[0];  // R
            destination[3] = source[3];  // A
            destination += 4;
            source += 4;
        }
    }
    return dib;
}

/**
 * @brief 把一段字节拷进 GMEM_MOVEABLE 全局内存（SetClipboardData 要求这种句柄）。
 * @param[in] data  源数据，可为 nullptr（此时只分配不拷贝）。
 * @param[in] bytes 字节数；必须 > 0。
 * @param[in] flags GlobalAlloc 的附加标志（CF_HDROP 需要 GMEM_ZEROINIT）。
 * @return 全局内存句柄；失败返回 nullptr。
 * @note 调用方在 SetClipboardData 成功后不得再释放句柄（所有权已转移给系统）。
 */
HGLOBAL copyToGlobalMemory(const void* data, std::size_t bytes, UINT flags) {
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | flags, bytes);
    if (handle == nullptr) {
        return nullptr;
    }
    void* destination = GlobalLock(handle);
    if (destination == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    if (data != nullptr && bytes > 0) {
        std::memcpy(destination, data, bytes);
    }
    GlobalUnlock(handle);
    return handle;
}

/**
 * @brief 带重试地打开剪贴板。
 * @return true = 已打开，调用方必须在同一线程上 CloseClipboard。
 */
bool openClipboardWithRetry() {
    for (int attempt = 0; attempt < kOpenClipboardRetries; ++attempt) {
        if (OpenClipboard(nullptr) != FALSE) {
            return true;
        }
        Sleep(kOpenClipboardRetryDelayMs);
    }
    return false;
}

/** @brief 剪贴板会话：构造时打开、析构时关闭，保证所有失败路径都不会漏 CloseClipboard。 */
class ClipboardSession {
public:
    ClipboardSession() = default;
    ~ClipboardSession() { close(); }

    ClipboardSession(const ClipboardSession&) = delete;
    ClipboardSession& operator=(const ClipboardSession&) = delete;

    /**
     * @brief 打开剪贴板（内部重试）。
     * @return true = 已打开；false = 一直被别的程序占用。
     */
    bool open() {
        m_open = openClipboardWithRetry();
        return m_open;
    }

    /** @brief 关闭剪贴板；未打开或重复调用是空操作。 */
    void close() {
        if (m_open) {
            CloseClipboard();
            m_open = false;
        }
    }

private:
    bool m_open = false;
};

/**
 * @brief UTF-8 文本转 UTF-16 宽字符（Win32 API 用）。
 * @param[in]  utf8Path UTF-8 路径。
 * @param[out] wide     转换结果。
 * @param[out] error    失败时写入中文原因。
 * @return true = 转换成功。
 */
bool toWide(const std::string& utf8Path, std::wstring& wide, std::string& error) {
    if (utf8Path.empty()) {
        error = "文件路径为空";
        return false;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.data(),
                                           static_cast<int>(utf8Path.size()), nullptr, 0);
    if (length <= 0) {
        error = "文件路径不是合法 UTF-8（或超长）：" + utf8Path;
        return false;
    }
    wide.assign(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.data(),
                        static_cast<int>(utf8Path.size()), wide.data(), length);
    return true;
}

/**
 * @brief 构造 CF_HDROP 的 DROPFILES 全局内存块（单个文件、宽字符、双 NUL 结尾）。
 * @param[in]  utf8Path UTF-8 文件路径。
 * @param[out] handle   全局内存句柄；失败保持 nullptr。
 * @param[out] error    失败时写入中文原因。
 * @return true = 构造完成。
 */
bool makeFileDrop(const std::string& utf8Path, HGLOBAL& handle, std::string& error) {
    std::wstring wide;
    if (!toWide(utf8Path, wide, error)) {
        return false;
    }
    // DROPFILES 的路径列表以"一个空字符结束的宽字符串 + 一个额外空字符"结束，
    // GMEM_ZEROINIT 保证额外那个空字符一定在（不需要手工再写一遍）。
    const std::size_t pathBytes = (wide.size() + 2) * sizeof(wchar_t);
    const std::size_t totalBytes = sizeof(DROPFILES) + pathBytes;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (memory == nullptr) {
        error = "GlobalAlloc(CF_HDROP) 失败";
        return false;
    }
    auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
    if (drop == nullptr) {
        GlobalFree(memory);
        error = "GlobalLock(CF_HDROP) 失败";
        return false;
    }
    drop->pFiles = static_cast<DWORD>(sizeof(DROPFILES));
    drop->fWide = TRUE;
    auto* destination = reinterpret_cast<wchar_t*>(reinterpret_cast<std::byte*>(drop) + sizeof(DROPFILES));
    std::memcpy(destination, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    handle = memory;
    return true;
}

}  // namespace

bool ClipboardWriter::copyImageFile(const std::string& utf8Path, bool alsoFileDrop,
                                    std::string& error) const {
    error.clear();
    if (utf8Path.empty()) {
        error = "capture.done 没有给出文件路径";
        return false;
    }

    // ---- 步骤 1：读文件 + stb 解码成 RGBA ----
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
        error = std::string("stb_image 解码失败") + (reason != nullptr ? (": " + std::string(reason)) : "");
        return false;
    }

    // ---- 步骤 2：RGBA（top-down）→ CF_DIB（32bpp BGRA, bottom-up） ----
    const std::vector<std::uint8_t> dib = makeDib(pixels.get(), width, height);
    if (dib.empty()) {
        error = "图片尺寸非法或超出 CF_DIB 上限，拒绝写入剪贴板";
        return false;
    }

    // ---- 步骤 3：打开剪贴板 → 清空 → 写入 ----
    ClipboardSession clipboard;
    if (!clipboard.open()) {
        error = "剪贴板被其它程序占用（重试 " + std::to_string(kOpenClipboardRetries) +
                " 次后仍打不开）";
        return false;
    }
    if (EmptyClipboard() == FALSE) {
        error = "EmptyClipboard 失败（GetLastError=" + std::to_string(GetLastError()) + "）";
        return false;
    }

    HGLOBAL dibHandle = copyToGlobalMemory(dib.data(), dib.size(), 0);
    if (dibHandle == nullptr) {
        error = "GlobalAlloc(CF_DIB) 失败";
        return false;
    }
    if (SetClipboardData(CF_DIB, dibHandle) == nullptr) {
        // 写入失败时所有权没有转移，必须自己释放，否则这块全局内存泄漏到进程退出。
        GlobalFree(dibHandle);
        error = "SetClipboardData(CF_DIB) 失败（GetLastError=" + std::to_string(GetLastError()) + "）";
        return false;
    }

    // ---- 步骤 4（可选）：再放一份 CF_HDROP 文件列表 ----
    if (alsoFileDrop) {
        HGLOBAL dropHandle = nullptr;
        if (!makeFileDrop(utf8Path, dropHandle, error)) {
            // 图片已经写进剪贴板：返回 false 让调用方 WARN，但图片粘贴仍可用。
            error = "图片已写入剪贴板，但 CF_HDROP 构造失败：" + error;
            return false;
        }
        if (SetClipboardData(CF_HDROP, dropHandle) == nullptr) {
            GlobalFree(dropHandle);
            error = "图片已写入剪贴板，但 SetClipboardData(CF_HDROP) 失败（GetLastError=" +
                    std::to_string(GetLastError()) + "）";
            return false;
        }
    }
    // CloseClipboard 由 ClipboardSession 析构完成（成功/失败路径统一）。
    return true;
}

}  // namespace clipboard
