// 长图 PNG 落盘：画布 RGBA → stbi_write_png_to_func → 自己的 ofstream。
//
// 为什么先开流再编码：stb 的按文件名接口在 Windows 上把 char* 当 ANSI，
// 中文目录会打不开；stbi_write_png_to_func 只回调字节，路径由
// std::filesystem::path（UTF-8 构造）负责，和 image_saver 的写法保持一致。
//
// 文件名固定 shot_long_YYYYMMDD_HHMMSS.png；同名文件已存在时追加 _1/_2…，
// 绝不覆盖用户已有的截图（同一秒里连续保存两次也不该丢一张）。
//
// 本文件不包含任何平台头；取系统图片目录是唯一的 #if 分支（与
// image_saver/src/output_path.cpp 的既有做法一致）。

#include "long_image_saver.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

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
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (home != nullptr && home[0] != '\0') {
        // getenv 给的是 CRT 本地编码的窄字符（Windows 上通常是 ANSI），
        // path(home) 按平台本地编码解释，拼好后再统一转 UTF-8。
        const std::filesystem::path pictures =
            std::filesystem::path(home) / std::filesystem::path("Pictures");
        return toUtf8Path(pictures);
    }
    // 环境变量缺失（服务账号、精简容器）：退回当前目录，至少第一次保存能成功。
    std::error_code ec;
    const std::filesystem::path current = std::filesystem::current_path(ec);
    if (!ec) {
        return toUtf8Path(current);
    }
    return ".";
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
