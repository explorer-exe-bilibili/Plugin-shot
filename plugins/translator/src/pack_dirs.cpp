// 语言包目录解析：唯一允许出现平台 API 的地方。
//
// 为什么不问宿主数据目录：ABI 里没有"宿主目录"能力（数据格式不进 ABI 的原则），
// 所以 exe 目录只能自己取；用户目录按宿主同一套约定推导：
//   Windows：%LOCALAPPDATA%\plugin-shot\langs
//   POSIX  ：$HOME/.plugin-shot/langs
// 两者都在本文件收敛，其余 TU 只用 std::filesystem，Linux/macOS 也能编译。

#include "pack_dirs.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <objbase.h>
#include <windows.h>

#include <ShlObj.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>

#include <climits>
#else
#include <unistd.h>
#endif

namespace translator {
namespace {

// 默认目录的优先级：与 document/I18N.md 的合并顺序一致（内置 100 < 官方 200 < 用户 300）。
constexpr std::int32_t kOfficialPriority = 200;
constexpr std::int32_t kUserPriority = 300;

// Windows 的 exe 路径缓冲初值：MAX_PATH 覆盖绝大多数情况，不够就翻倍重取。
constexpr std::size_t kPathBuffer = 512;

// 翻倍上限：超过 32767 个宽字符的路径在 Win32 里已经要 \\?\ 前缀了，不再尝试。
constexpr std::size_t kMaxPathBuffer = 32768;

/** @brief UTF-8 文本 → 路径（Windows 上内部转宽字符，中文目录才不会坏）。 */
std::filesystem::path fromUtf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const std::u8string utf8(reinterpret_cast<const char8_t*>(text.data()), text.size());
    return std::filesystem::path(utf8);
}

/** @brief 去掉首尾空白（配置值里常有用户随手打的空格）。 */
std::string trim(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

std::string pathToUtf8(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    // C++20 的 char8_t 通道是唯一可靠的 UTF-8 出口：path 的窄串接口按平台本地
    // 编码解释，Windows 上中文目录会直接变乱码。
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::vector<PackLocation> PackDirs::resolve(const std::string& configured) {
    std::vector<PackLocation> locations;
    const std::vector<std::string> configuredDirs = splitList(configured);
    if (!configuredDirs.empty()) {
        // 填了 pack_dirs 就只用它：混入默认目录会让"这个包到底从哪来"变糊，
        // 而用户显式指定目录通常正是为了做局部替换试验。
        locations.reserve(configuredDirs.size());
        for (const std::string& dir : configuredDirs) {
            std::filesystem::path path = fromUtf8(dir);
            if (path.empty()) {
                continue;
            }
            std::error_code ec;
            const std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
            if (!ec) {
                path = absolutePath;
            }
            // 配置目录按用户包优先级：相对内置表它一定是"用户意图更强"的来源。
            locations.push_back(PackLocation{std::move(path), kUserPriority, "配置"});
        }
        return locations;
    }

    const std::filesystem::path exeDir = executableDirectory();
    if (!exeDir.empty()) {
        locations.push_back(PackLocation{exeDir / "files" / "lang", kOfficialPriority, "官方"});
    }
    const std::filesystem::path userDir = defaultUserDirectory();
    if (!userDir.empty()) {
        locations.push_back(PackLocation{userDir, kUserPriority, "用户"});
    }
    return locations;
}

std::filesystem::path PackDirs::executableDirectory() {
    const std::filesystem::path path = executablePath();
    if (path.empty()) {
        return {};
    }
    return path.parent_path();
}

std::filesystem::path PackDirs::executablePath() {
#if defined(_WIN32)
    // GetModuleFileNameW(nullptr) = 当前进程的 exe；缓冲区不够时返回 size 并置
    // ERROR_INSUFFICIENT_BUFFER，所以按"返回值 >= 缓冲区大小"判断截断，翻倍重试。
    std::wstring buffer(kPathBuffer, L'\0');
    for (;;) {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        if (buffer.size() >= kMaxPathBuffer) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    // _NSGetExecutablePath 不保证绝对路径（可能含 "." / ".." / 符号链接），
    // 用 weakly_canonical 归一化；失败就退回原串，至少不是空。
    char rawPath[PATH_MAX] = {};
    std::uint32_t rawSize = sizeof(rawPath);
    if (_NSGetExecutablePath(rawPath, &rawSize) != 0) {
        return {};
    }
    const std::filesystem::path raw(rawPath);
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(raw, ec);
    return ec ? raw : canonical;
#else
    // Linux 读 /proc/self/exe；无 /proc 的容器里失败，返回空路径（跳过官方目录）。
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : target;
#endif
}

#if defined(_MSC_VER)
// C4996：MSVC 建议用 _dupenv_s，但那是 MSVC 专有 API，与"只用标准库 + 平台路径 API"
// 的约定冲突；std::getenv 是标准 C/C++，这里只读取环境、拿到值立刻构造 path，
// 不存在悬垂指针，所以精确屏蔽这一条（与 plugins/image_saver 同一处理）。
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
std::filesystem::path PackDirs::defaultUserDirectory() {
#if defined(_WIN32)
    PWSTR raw = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        const std::filesystem::path base(raw);
        ::CoTaskMemFree(raw);
        return base / "plugin-shot" / "langs";
    }
    // 精简容器/服务账号里 KnownFolder 可能失败：退回环境变量，至少不丢用户包。
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData != nullptr && localAppData[0] != '\0') {
        return fromUtf8(localAppData) / "plugin-shot" / "langs";
    }
    return {};
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".plugin-shot" / "langs";
    }
    return {};
#endif
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

std::vector<std::string> PackDirs::splitList(const std::string& value) {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t separator = value.find(';', start);
        const std::string piece =
            trim(value.substr(start, (separator == std::string::npos) ? std::string::npos
                                                                      : separator - start));
        if (!piece.empty()) {
            items.push_back(piece);
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    return items;
}

}  // namespace translator
