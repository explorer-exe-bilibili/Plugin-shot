// 路径解析：目录 + 文件名模板 + 扩展名 → 绝对路径。
//
// 这里刻意把"算路径"和"落盘"分开：save 先算出路径、确认宿主 path 缓冲装得下，
// 再建目录、再编码。这样 path 缓冲不足（返回 2）时不会留下空目录或半截文件。
//
// 平台差异只允许出现在两处，其余全部走标准库：
//   · 取系统图片目录：Windows 用 SHGetKnownFolderPath(FOLDERID_Pictures)，POSIX 按
//     XDG 规则（$XDG_PICTURES_DIR → user-dirs.dirs → $HOME/Pictures）；
//   · 取本地时间：MSVC 用 localtime_s，其它用 localtime_r（std::localtime 返回
//     共享静态缓冲，非线程安全）。
//
// 为什么不再硬拼目录：Windows 的「图片」库可以在属性里搬迁到别的盘/目录，固定写
// %USERPROFILE%\Pictures 会把截图写回旧位置；Linux 的图片目录名是本地化的（中文
// 桌面是 ~/图片），固定写 $HOME/Pictures 同样错。两条用户报告都出在这上面。

#include "output_path.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
// 「图片」已知文件夹：shell32 的 SHGetKnownFolderPath，以及释放它返回值的
// CoTaskMemFree（ole32）。NOMINMAX 已由 shot_add_plugin 统一定义，这里再兜一层，
// 让本文件在 SDK 独立构建（没有那层定义）时也不受 windows.h 的 min/max 宏影响。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif

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

// ---------------------------------------------------------------------------
// 系统图片目录：平台分支只出现在下面几个辅助函数里，取不到一律返回空串，
// 由 OutputPath::defaultPicturesDirectory() 用同一条回退链收尾。
// ---------------------------------------------------------------------------

/** @brief 当前工作目录的 UTF-8 路径；取不到（极罕见）返回 "."。 */
std::string currentDirectoryUtf8() {
    std::error_code ec;
    const std::filesystem::path current = std::filesystem::current_path(ec);
    if (!ec) {
        return OutputPath::toUtf8(current);
    }
    return ".";
}

#if defined(_WIN32)
/**
 * @brief 系统「图片」已知文件夹的 UTF-8 路径。
 *
 * 用户可以在资源管理器里把「图片」库搬迁到别的盘/目录（属性 → 位置）；
 * SHGetKnownFolderPath 返回搬迁后的真实位置，这就是不能硬拼 %USERPROFILE% 的原因。
 *
 * @return UTF-8 路径；API 失败或返回空路径时返回空串（由调用方回退）。
 */
std::string knownFolderPictures() {
    wchar_t* raw = nullptr;
    const HRESULT status =
        ::SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &raw);
    std::string utf8;
    if (SUCCEEDED(status) && raw != nullptr && raw[0] != L'\0') {
        utf8 = OutputPath::toUtf8(std::filesystem::path(raw));
    }
    if (raw != nullptr) {
        // KnownFolder API 的返回值归调用方所有：漏掉这次释放就是每次保存泄漏一块内存。
        ::CoTaskMemFree(raw);
    }
    return utf8;
}
#else
/** @brief user-dirs.dirs 里登记图片目录的键名。 */
const char kPicturesKey[] = "XDG_PICTURES_DIR";

/** @brief 去掉首尾空白；user-dirs.dirs 允许键值前后有空格/制表符。 */
std::string trimSpaces(const std::string& text) {
    const auto notSpace = [](unsigned char ch) { return std::isspace(ch) == 0; };
    const auto begin = std::find_if(text.begin(), text.end(), notSpace);
    const auto end = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

/** @brief 读环境变量；未定义或空串都按"没配置"返回空串。 */
std::string envOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr) ? std::string(value) : std::string();
}

/** @brief 家目录（去掉末尾分隔符）；没有 $HOME 时返回空串。 */
std::string homeDirectory() {
    std::string home = envOrEmpty("HOME");
    while (home.size() > 1 && home.back() == '/') {
        home.pop_back();
    }
    return home;
}

/**
 * @brief 展开 user-dirs.dirs 值里的 $HOME / ${HOME}。
 * @param[in] home  家目录；空串表示环境里没有 HOME。
 * @param[in] value 原始值（已去引号）。
 * @return 展开后的值；引用了 $HOME 但拿不到 HOME 时返回空串（该值不可用）。
 */
std::string expandHome(const std::string& home, const std::string& value) {
    // xdg-user-dirs 写出的值只用 $HOME 与 ${HOME} 两种写法，其它 $ 原样保留
    // （可能是文件名里的字面 $）。
    static const char kHomeName[] = "HOME";
    const std::size_t nameLength = sizeof(kHomeName) - 1;

    std::string result;
    result.reserve(value.size() + home.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '$') {
            result.push_back(value[i]);
            continue;
        }
        std::size_t nameBegin = i + 1;
        bool braced = false;
        if (nameBegin < value.size() && value[nameBegin] == '{') {
            braced = true;
            ++nameBegin;
        }
        std::size_t next = nameBegin + nameLength;
        if (value.compare(nameBegin, nameLength, kHomeName) != 0) {
            result.push_back(value[i]);
            continue;
        }
        if (braced) {
            if (next >= value.size() || value[next] != '}') {
                result.push_back(value[i]);
                continue;
            }
            ++next;
        }
        if (home.empty()) {
            return {};
        }
        result.append(home);
        i = next - 1;
    }
    return result;
}

/**
 * @brief user-dirs.dirs 的路径：$XDG_CONFIG_HOME/user-dirs.dirs，缺省在 $HOME/.config 下。
 * @param[in] home 家目录；为空且没有 $XDG_CONFIG_HOME 时返回空串。
 */
std::string userDirsPath(const std::string& home) {
    const std::string configHome = envOrEmpty("XDG_CONFIG_HOME");
    if (!configHome.empty()) {
        return configHome + "/user-dirs.dirs";
    }
    if (home.empty()) {
        return {};
    }
    return home + "/.config/user-dirs.dirs";
}

/**
 * @brief 从 user-dirs.dirs 里读 XDG_PICTURES_DIR 的原始值。
 *
 * 文件不存在、没有这一项都返回空串；注释与空行跳过（规范格式是 KEY="VALUE"，
 * 引号可有可无）。
 *
 * @param[in] path 配置文件路径；空串直接返回空。
 * @return 去掉双引号与首尾空白的原始值（尚未展开 $HOME）。
 */
std::string readUserDirsPictures(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return {};
    }
    std::string line;
    bool firstLine = true;
    while (std::getline(stream, line)) {
        // 文件可能带 UTF-8 BOM（用户在 Windows 编辑器里改过），只在第一行去掉。
        if (firstLine && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        firstLine = false;
        const std::string trimmed = trimSpaces(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const std::size_t equal = trimmed.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        if (trimSpaces(trimmed.substr(0, equal)) != kPicturesKey) {
            continue;
        }
        std::string value = trimSpaces(trimmed.substr(equal + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return {};
}

/**
 * @brief POSIX 系统的图片目录。
 *
 * 优先级：$XDG_PICTURES_DIR 环境变量 → user-dirs.dirs 里的 XDG_PICTURES_DIR →
 * 空串（调用方回退 $HOME/Pictures）。只接受绝对路径：XDG 规范要求绝对路径，
 * 手改出来的相对路径不可信，否则截图会落到宿主工作目录旁边的怪目录里。
 */
std::string posixPicturesDirectory() {
    const std::string home = homeDirectory();
    std::string candidate = expandHome(home, envOrEmpty("XDG_PICTURES_DIR"));
    if (!candidate.empty() && candidate.front() == '/') {
        return candidate;
    }
    candidate = expandHome(home, readUserDirsPictures(userDirsPath(home)));
    if (!candidate.empty() && candidate.front() == '/') {
        return candidate;
    }
    return {};
}
#endif

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
    // 1) 系统「图片」已知文件夹：跟随用户在属性 → 位置里的重定位。
    const std::string known = knownFolderPictures();
    if (!known.empty()) {
        return known;
    }
    // 2) 查询失败（极老系统 / 精简容器 / 服务账号）：退回 %USERPROFILE%\Pictures。
    const char* home = std::getenv("USERPROFILE");
    if (home != nullptr && home[0] != '\0') {
        // Windows 上 getenv 给的是 CRT 的窄字符环境（本地编码），所以 path(home)
        // 按平台本地编码解释；拼好目录后统一转 UTF-8 给宿主和自己使用。
        const std::filesystem::path pictures =
            std::filesystem::path(home) / std::filesystem::path(kPicturesDirName);
        return toUtf8(pictures);
    }
#else
    // 1) XDG 图片目录（$XDG_PICTURES_DIR 或 user-dirs.dirs），中文桌面的 ~/图片 也走这里。
    const std::string pictures = posixPicturesDirectory();
    if (!pictures.empty()) {
        return pictures;
    }
    // 2) XDG 配置不可用：退回 $HOME/Pictures。
    const std::string home = homeDirectory();
    if (!home.empty()) {
        return home + "/" + kPicturesDirName;
    }
#endif
    // 3) 系统图片目录完全不可用：退回当前目录，至少第一次保存能成功。
    return currentDirectoryUtf8();
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
