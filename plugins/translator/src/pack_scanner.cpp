// 语言包目录扫描：位置列表 → 解析后的包。
//
// 顺序决定覆盖关系（同一 priority 下后加载的包赢），所以目录按传入顺序处理、
// 目录内按文件名排序：同一批文件每次扫描得到同样的结果，排障时日志顺序也稳定。
// 目录不存在只记 INFO（用户语言包目录在没装包时本来就不存在），不可读才 WARN。

#include "pack_scanner.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

#include "shot_plugin_helpers.h"

namespace translator {
namespace {

/** @brief 扩展名转小写，用于大小写不敏感地识别 *.json。 */
std::string lowerExtension(const std::filesystem::path& path) {
    std::string lower = pathToUtf8(path.extension());
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower;
}

}  // namespace

PackScanner::PackScanner(const ShotHostServices* host) : m_host(host), m_loader(host) {}

std::vector<ParsedPack> PackScanner::scan(const std::vector<PackLocation>& locations) const {
    const shot::Logger log(m_host);
    std::vector<ParsedPack> packs;
    for (const PackLocation& location : locations) {
        // 先看"有没有"再看"是不是目录"：用户语言包目录在没装过包时本来就不存在，
        // 那是最常见的情况，只记 INFO；把不存在的目录记成 WARN 会让全新安装的日志
        // 每次启动都出现一条无意义的告警。
        std::error_code ec;
        if (!std::filesystem::exists(location.path, ec)) {
            log.info("[translator] 语言包目录不存在，跳过：" + pathToUtf8(location.path));
            continue;
        }
        if (!std::filesystem::is_directory(location.path, ec)) {
            log.warn("[translator] 语言包路径不是目录，跳过：" + pathToUtf8(location.path));
            continue;
        }
        const std::vector<std::filesystem::path> files = listFiles(location);
        for (const std::filesystem::path& file : files) {
            ParsedPack pack;
            if (!m_loader.load(file, location.priority, location.origin, packs.size(), pack)) {
                continue;  // 坏包：PackLoader 已经 WARN 过原因
            }
            log.info("[translator] 语言包已加载：" + pack.path + "（语言 " + pack.tag + "，" +
                     std::to_string(textCount(pack)) + " 条，来源 " + pack.origin +
                     "，priority " + std::to_string(pack.priority) + "）");
            packs.push_back(std::move(pack));
        }
    }
    return packs;
}

std::vector<std::filesystem::path> PackScanner::listFiles(const PackLocation& location) const {
    const shot::Logger log(m_host);
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    std::filesystem::directory_iterator iterator(location.path, ec);
    if (ec) {
        log.warn("[translator] 读取语言包目录失败（错误码 " + std::to_string(ec.value()) +
                 "）：" + pathToUtf8(location.path));
        return files;
    }
    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            continue;  // 子目录 / 目标不可读的链接：不是语言包，安静跳过
        }
        if (lowerExtension(entry.path()) != ".json") {
            continue;  // README.md 之类的说明文件就在同一个目录里
        }
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(),
              [](const std::filesystem::path& left, const std::filesystem::path& right) {
                  return pathToUtf8(left.filename()) < pathToUtf8(right.filename());
              });
    return files;
}

}  // namespace translator
