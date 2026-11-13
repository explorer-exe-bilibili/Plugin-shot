// 长图 PNG 落盘：画布 RGBA → stbi_write_png_to_func → 自己的 ofstream。
//
// 为什么先开流再编码：stb 的按文件名接口在 Windows 上把 char* 当 ANSI，
// 中文目录会打不开；stbi_write_png_to_func 只回调字节，路径由
// std::filesystem::path（UTF-8 构造）负责，和 image_saver 的写法保持一致。
//
// 文件名固定 shot_long_YYYYMMDD_HHMMSS.png；同名文件已存在时追加 _1/_2…，
// 绝不覆盖用户已有的截图（同一秒里连续保存两次也不该丢一张）。
//
// 平台分支只有取系统图片目录一处（与 image_saver/src/output_path.cpp 同一套规则）：
// Windows 走 SHGetKnownFolderPath(FOLDERID_Pictures)（跟随「图片」库重定位），POSIX
// 按 XDG 规则读 $XDG_PICTURES_DIR / user-dirs.dirs，最后才回退 $HOME/Pictures。

#include "long_image_saver.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
// KnownFolder API：shell32 的 SHGetKnownFolderPath，以及释放它返回值的
// CoTaskMemFree（ole32）。NOMINMAX 由 shot_add_plugin 统一定义，这里再兜一层。
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

namespace long_capture {
namespace {

// 下面三个辅助函数只服务本文件（UTF-8 路径转换与时间拆解），与任何类都无关，
// 放进匿名命名空间（STYLE.md §6.7 允许的"只在本 TU 使用"的具名函数）。

/** @brief PNG 固定 4 通道（RGBA）。 */
constexpr int kPngChannels = 4;

/** @brief 同名文件最多尝试 1000 个 _N 后缀：到上限说明目录里堆了大量同名截图，直接报失败。 */
constexpr int kMaxNameAttempts = 1000;

/** @brief 文件头：shot_long_。 */
const char kNamePrefix[] = "shot_long_";

/** @brief 文件尾：.png。 */
const char kNameSuffix[] = ".png";

/** @brief 按 UTF-8 字节构造 filesystem::path（Windows 上内部转宽字符）。 */
std::filesystem::path fromUtf8Path(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const std::u8string text(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(text);
}

/** @brief filesystem::path 转 UTF-8 文本。 */
std::string toUtf8Path(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

/** @brief 本地时间拆解；MSVC 用 localtime_s，其它平台用 localtime_r（线程安全）。 */
bool localTime(std::tm& out, std::time_t now) {
#if defined(_MSC_VER)
    return localtime_s(&out, &now) == 0;
#else
    return localtime_r(&now, &out) != nullptr;
#endif
}

// ---------------------------------------------------------------------------
// 系统图片目录：与 image_saver/src/output_path.cpp 保持同一套优先级，取不到
// 一律返回空串，由 LongImageSaver::defaultDirectory() 用同一条回退链收尾。
// ---------------------------------------------------------------------------

/** @brief 当前工作目录的 UTF-8 路径；取不到（极罕见）返回 "."。 */
std::string currentDirectoryUtf8() {
    std::error_code ec;
    const std::filesystem::path current = std::filesystem::current_path(ec);
    if (!ec) {
        return toUtf8Path(current);
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
        utf8 = toUtf8Path(std::filesystem::path(raw));
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
 * 手改出来的相对路径不可信。
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

SaveOutcome LongImageSaver::save(const StitchCanvas& canvas, const std::string& directory) {
    SaveOutcome outcome;
    if (canvas.empty() || canvas.data() == nullptr) {
        outcome.error = "画布是空的，没有可保存的内容";
        return outcome;
    }

    std::filesystem::path targetDirectory = fromUtf8Path(directory);
    if (targetDirectory.empty()) {
        targetDirectory = fromUtf8Path(defaultDirectory());
    }
    std::filesystem::path target = makeUnique(targetDirectory / fromUtf8Path(makeFileName()));

    // 宿主日志与 capture.done 要的是绝对路径；取不到就保留相对路径，让后面的写入暴露问题。
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(target, ec);
    if (!ec) {
        target = absolute;
    }

    if (!ensureParentDirectory(target)) {
        outcome.error = "创建保存目录失败：" + toUtf8Path(target.parent_path());
        return outcome;
    }

    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        outcome.error = "打不开输出文件（目录不可写？）：" + toUtf8Path(target);
        return outcome;
    }

    const int written = stbi_write_png_to_func(&LongImageSaver::writeToStream, &stream,
                                               canvas.width(), canvas.height(), kPngChannels,
                                               canvas.data(), canvas.width() * kPngChannels);
    stream.flush();
    const bool ok = (written != 0) && stream.good();
    stream.close();
    if (!ok) {
        // 失败时删掉写了一半的文件；目标名是刚挑出来的空名字，删掉不会误伤已有文件。
        std::error_code removeError;
        std::filesystem::remove(target, removeError);
        outcome.error = "PNG 编码或写盘失败（磁盘满？目录只读？）";
        return outcome;
    }
    outcome.ok = true;
    outcome.path = toUtf8Path(target);
    return outcome;
}

std::string LongImageSaver::defaultDirectory() {
#if defined(_MSC_VER)
    // getenv 在 MSVC 上是 C4996（"不安全"）警告；这里只读、不写环境，语义上安全。
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#if defined(_WIN32)
    // 1) 系统「图片」已知文件夹：跟随用户在属性 → 位置里的重定位。
    const std::string known = knownFolderPictures();
    if (!known.empty()) {
        return known;
    }
    // 2) 查询失败（极老系统 / 精简容器 / 服务账号）：退回 %USERPROFILE%\Pictures。
    const char* home = std::getenv("USERPROFILE");
#else
    // 1) XDG 图片目录（$XDG_PICTURES_DIR 或 user-dirs.dirs），中文桌面的 ~/图片 也走这里。
    const std::string pictures = posixPicturesDirectory();
    if (!pictures.empty()) {
        return pictures;
    }
    // 2) XDG 配置不可用：退回 $HOME/Pictures。
    const char* home = std::getenv("HOME");
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (home != nullptr && home[0] != '\0') {
        // getenv 给的是 CRT 本地编码的窄字符（Windows 上通常是 ANSI），
        // path(home) 按平台本地编码解释，拼好后再统一转 UTF-8。
        const std::filesystem::path picturesPath =
            std::filesystem::path(home) / std::filesystem::path("Pictures");
        return toUtf8Path(picturesPath);
    }
    // 3) 系统图片目录完全不可用：退回当前目录，至少第一次保存能成功。
    return currentDirectoryUtf8();
}

std::string LongImageSaver::makeFileName() {
    return std::string(kNamePrefix) + timestamp() + kNameSuffix;
}

std::string LongImageSaver::timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (now == static_cast<std::time_t>(-1) || !localTime(local, now)) {
        return "19700101_000000";
    }
    char text[32];
    const int written = std::snprintf(text, sizeof(text), "%04d%02d%02d_%02d%02d%02d",
                                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                      local.tm_hour, local.tm_min, local.tm_sec);
    if (written <= 0) {
        return "19700101_000000";
    }
    return std::string(text, static_cast<std::size_t>(written));
}

std::filesystem::path LongImageSaver::makeUnique(const std::filesystem::path& target) {
    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) {
        return target;
    }
    const std::filesystem::path parent = target.parent_path();
    const std::string stem = target.stem().string();
    const std::string extension = target.extension().string();
    for (int index = 1; index <= kMaxNameAttempts; ++index) {
        const std::filesystem::path candidate =
            parent / fromUtf8Path(stem + "_" + std::to_string(index) + extension);
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return target;  // 极端情况：目录里同名文件堆满，交给写入路径按失败报出
}

bool LongImageSaver::ensureParentDirectory(const std::filesystem::path& filePath) {
    const std::filesystem::path parent = filePath.parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        return false;
    }
    return std::filesystem::is_directory(parent, ec) && !ec;
}

void LongImageSaver::writeToStream(void* context, void* data, int size) {
    if (context == nullptr || data == nullptr || size <= 0) {
        return;
    }
    static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data),
                                                static_cast<std::streamsize>(size));
}

}  // namespace long_capture
