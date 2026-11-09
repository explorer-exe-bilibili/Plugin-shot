// 路径解析：目录 + 文件名模板 + 扩展名 → 绝对路径。
//
// 这里刻意把"算路径"和"落盘"分开：save 先算出路径、确认宿主 path 缓冲装得下，
// 再建目录、再编码。这样 path 缓冲不足（返回 2）时不会留下空目录或半截文件。
//
// 平台差异只允许出现在两处，其余全部走标准库：
//   · 取系统图片目录：Windows 用 %USERPROFILE%\Pictures，POSIX 用 $HOME/Pictures；
//   · 取本地时间：MSVC 用 localtime_s，其它用 localtime_r（std::localtime 返回
//     共享静态缓冲，非线程安全）。

#include "output_path.h"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace image_saver {
namespace {

const char kFallbackBasename[] = "shot";
const char kPicturesDirName[] = "Pictures";

// 同名文件最多尝试 1000 个后缀：到上限说明目录里堆了大量同秒截图，再尝试也只是
// 拖慢保存，直接按保存失败报出去（save 返回 3）。
constexpr int kMaxNameAttempts = 1000;

// 纯字符判定，与 OutputPath 的任何状态无关，所以放匿名命名空间而不是类成员；
// 只在本 TU 使用（STYLE §6.7 允许的匿名命名空间小工具）。
bool isIllegalFilenameChar(char ch) {
    if (static_cast<unsigned char>(ch) < 0x20) {
        return true;
    }
    switch (ch) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '/':
        case '\\':
        case '|':
        case '?':
        case '*':
            return true;
        default:
            return false;
    }
}

}  // namespace

OutputPath::OutputPath(std::string directory, std::string basename, std::string extension)
    : m_directory(std::move(directory)),
      m_basename(std::move(basename)),
      m_extension(std::move(extension)) {}

std::filesystem::path OutputPath::resolve() const {
    std::filesystem::path directory = fromUtf8(m_directory);
    if (directory.empty()) {
        directory = fromUtf8(defaultPicturesDirectory());
    }

    std::string name = expandTimestamp(m_basename);
    name = sanitizeBasename(name);
    if (name.empty()) {
        name = kFallbackBasename;
    }

    std::filesystem::path target = directory / fromUtf8(name + m_extension);
    // 宿主日志与事件负载里要的是绝对路径；相对目录相对宿主的当前工作目录解析。
    // absolute 失败（极少见）时保留原路径，让编码阶段用错误码暴露问题。
    std::error_code ec;
    const std::filesystem::path absolutePath = std::filesystem::absolute(target, ec);
    if (!ec) {
        target = absolutePath;
    }
    return makeUnique(target);
}

bool OutputPath::ensureParentDirectory(const std::filesystem::path& filePath) {
    const std::filesystem::path parent = filePath.parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        return false;
    }
    const bool isDirectory = std::filesystem::is_directory(parent, ec);
    return isDirectory && !ec;
}

std::filesystem::path OutputPath::fromUtf8(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    // C++20 的 char8_t 构造按 UTF-8 解释字节，Windows 上内部转成宽字符；
    // path(std::string) 用的是平台本地窄编码（Windows = ANSI），中文路径会坏。
    const std::u8string text(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(text);
}

std::string OutputPath::toUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

#if defined(_MSC_VER)
// C4996：MSVC 建议用 _dupenv_s，但那是 MSVC 专有 API，与本插件"只用标准库 + stb"
// 的约定冲突；std::getenv 是标准 C/C++，这里只读取环境、拿到值立刻转成 path，
// 不存在悬垂指针，所以在这个函数范围内精确屏蔽这一条。
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
std::string OutputPath::defaultPicturesDirectory() {
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (home != nullptr && home[0] != '\0') {
        // Windows 上 getenv 给的是 CRT 的窄字符环境（本地编码），所以 path(home)
        // 按平台本地编码解释；拼好目录后统一转 UTF-8 给宿主和自己使用。
        const std::filesystem::path pictures =
            std::filesystem::path(home) / std::filesystem::path(kPicturesDirName);
        return toUtf8(pictures);
    }
    // 环境变量缺失（服务账号、精简容器）：退回当前目录，至少第一次保存能成功。
    std::error_code ec;
    const std::filesystem::path current = std::filesystem::current_path(ec);
    if (!ec) {
        return toUtf8(current);
    }
    return ".";
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

std::string OutputPath::expandTimestamp(const std::string& pattern) {
    if (pattern.empty()) {
        return {};
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (now == static_cast<std::time_t>(-1) || !localTime(local, now)) {
        return pattern;  // 时间都取不到：保留模板原文，别让文件名变空
    }

    std::string result;
    result.reserve(pattern.size() + 16);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (ch != '%' || i + 1 >= pattern.size()) {
            result.push_back(ch);
            continue;
        }
        const char spec = pattern[++i];
        int value = 0;
        switch (spec) {
            case 'Y':
                value = local.tm_year + 1900;
                break;
            case 'm':
                value = local.tm_mon + 1;
                break;
            case 'd':
                value = local.tm_mday;
                break;
            case 'H':
                value = local.tm_hour;
                break;
            case 'M':
                value = local.tm_min;
                break;
            case 'S':
                value = local.tm_sec;
                break;
            case '%':
                result.push_back('%');
                continue;
            default:
                // 不认识的占位符原样保留：配置写错时文件名里多一个 %Q，总比丢字符好。
                result.push_back('%');
                result.push_back(spec);
                continue;
        }
        char field[8];
        const int written =
            std::snprintf(field, sizeof(field), (spec == 'Y') ? "%04d" : "%02d", value);
        if (written > 0) {
            result.append(field, static_cast<std::size_t>(written));
        }
    }
    return result;
}

bool OutputPath::localTime(std::tm& out, std::time_t value) noexcept {
#if defined(_MSC_VER)
    // MSVC 的 localtime_s 返回 errno_t；POSIX 的 localtime_r 返回指针。
    return ::localtime_s(&out, &value) == 0;
#else
    return ::localtime_r(&value, &out) != nullptr;
#endif
}

std::string OutputPath::sanitizeBasename(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (const char ch : name) {
        result.push_back(isIllegalFilenameChar(ch) ? '_' : ch);
    }
    // Windows 不允许文件名以空格或点结尾（"a. " 会被静默截断或拒绝）。
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
        result.pop_back();
    }
    if (isReservedWindowsName(result)) {
        result.insert(result.begin(), '_');
    }
    return result;
}

bool OutputPath::isReservedWindowsName(const std::string& name) noexcept {
    if (name.empty()) {
        return false;
    }
    // 只比较第一个点之前的部分，且大小写不敏感（Windows 文件系统不区分大小写）。
    const std::size_t stemLength = name.find('.');
    const std::string stem = name.substr(0, stemLength);
    std::string upper;
    upper.reserve(stem.size());
    for (const char ch : stem) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    static const char* const kReservedNames[] = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9",
    };
    for (const char* reserved : kReservedNames) {
        if (upper == reserved) {
            return true;
        }
    }
    return false;
}

std::filesystem::path OutputPath::makeUnique(const std::filesystem::path& base) {
    if (base.empty()) {
        return {};
    }
    std::error_code ec;
    if (!std::filesystem::exists(base, ec)) {
        return base;  // 查询失败（ec）也按不存在处理：真写不进去由编码器报保存失败
    }
    // 用 path += 拼名字而不是 string：Windows 上 path::string() 会把宽字符转成本地
    // 窄编码，中文文件名会坏在这条路径上。
    std::filesystem::path stem = base.parent_path() / base.stem();
    const std::filesystem::path extension = base.extension();
    for (int index = 1; index <= kMaxNameAttempts; ++index) {
        std::filesystem::path candidate = stem;
        candidate += fromUtf8("-" + std::to_string(index));
        candidate += extension;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace image_saver
