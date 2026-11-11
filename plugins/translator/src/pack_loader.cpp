// 单个语言包文件 → ParsedPack。
//
// 解析顺序：读文件（大小上限）→ 剥 BOM → 解析 JSON 对象 → 取 @meta（字符串或
// {tag,name,fallback}）→ 归一化 tag → 校验文件名 → 逐层收 texts。
// 任何一步失败都只 WARN + 返回 false，绝不抛异常、绝不打印 ERROR：语言包是可被
// 手改的输入，坏包不该让插件加载失败（document/I18N.md §2.1 的约束）。

#include "pack_loader.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "pack_dirs.h"
#include "shot_plugin_helpers.h"

namespace translator {
namespace {

// 单个语言包的大小上限：正常包几 KB，4 MiB 已经足够放几十种语言的完整译文；
// 再大只可能是放错了文件，按坏包跳过比读进内存更安全。
constexpr std::uintmax_t kMaxPackBytes = 4 * 1024 * 1024;

/** @brief 去掉首尾空白（owner / 文件名这类标识符允许用户随手打空格）。 */
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

/**
 * @brief 取 JSON 对象里的字符串字段。
 * @param[in] object 已确认是对象的 JSON。
 * @param[in] key    字段名。
 * @return 字段值；缺失或不是字符串时返回空串（调用方按"没写"处理）。
 */
std::string stringField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

}  // namespace

PackLoader::PackLoader(const ShotHostServices* host) noexcept : m_host(host) {}

bool PackLoader::load(const std::filesystem::path& file, std::int32_t priority,
                      const std::string& origin, std::size_t order, ParsedPack& out) const {
    const shot::Logger log(m_host);
    const std::string path = pathToUtf8(file);
    std::string raw;
    if (!readFile(file, raw)) {
        return false;
    }

    // 语言包常带 UTF-8 BOM（记事本默认加）。nlohmann 当前会跳过合法 BOM，但这里
    // 显式剥掉：不依赖第三方库的细节，也避免 BOM 变成第一个字符串的隐藏前缀。
    std::string_view text(raw);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    const nlohmann::json doc = nlohmann::json::parse(text.begin(), text.end(), nullptr,
                                                     /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        log.warn("[translator] 语言包不是合法 JSON 对象，已跳过：" + path);
        return false;
    }

    // @meta 两种写法：内置表风格 "@meta":"ja-JP"，外部语言包风格
    // "@meta":{"tag":"ja-JP","name":"日本語","fallback":"en-US"}。
    const auto metaIt = doc.find("@meta");
    if (metaIt == doc.end()) {
        log.warn("[translator] 语言包缺少 @meta（字符串或 {tag,name,fallback} 对象），已跳过：" +
                 path);
        return false;
    }
    std::string tag;
    std::string name;
    std::string fallback;
    if (metaIt->is_string()) {
        tag = metaIt->get<std::string>();
    } else if (metaIt->is_object()) {
        tag = stringField(*metaIt, "tag");
        name = stringField(*metaIt, "name");
        fallback = stringField(*metaIt, "fallback");
    } else {
        log.warn("[translator] 语言包的 @meta 既不是字符串也不是对象，已跳过：" + path);
        return false;
    }
    tag = normalizeLanguageTag(tag);
    if (tag.empty()) {
        log.warn("[translator] 语言包的 @meta 没有有效语言标签，已跳过：" + path);
        return false;
    }
    fallback = normalizeLanguageTag(fallback);

    // 文件名与 tag 不一致时以 @meta.tag 为准（用户重命名文件很常见），但记一条 WARN：
    // 文本只有按 tag 才能进对语言，文件名错位通常是复制包时忘了改 @meta。
    const std::string stem = pathToUtf8(file.stem());
    if (!stem.empty() && normalizeLanguageTag(stem) != tag) {
        log.warn("[translator] 语言包文件名 " + stem + " 与 @meta.tag " + tag +
                 " 不一致，以 @meta 为准：" + path);
    }

    const auto textsIt = doc.find("texts");
    if (textsIt == doc.end() || !textsIt->is_object()) {
        log.warn("[translator] 语言包缺少 texts 对象，已跳过：" + path);
        return false;
    }

    // texts[owner][key] = 文本。只有"非空字符串"算可用文本：
    //   · 空串 = 这条还没翻（作者拿它占位），跳过让它走回退链；
    //   · 非字符串 = 结构写错，计入 malformed 后统一 WARN 一次。
    std::map<std::string, std::map<std::string, std::string>> texts;
    std::size_t malformed = 0;
    for (auto ownerIt = textsIt->begin(); ownerIt != textsIt->end(); ++ownerIt) {
        if (!ownerIt->is_object()) {
            ++malformed;
            continue;
        }
        const std::string owner = trim(ownerIt.key());
        if (owner.empty()) {
            malformed += ownerIt->size();
            continue;
        }
        std::map<std::string, std::string> bucket;
        for (auto keyIt = ownerIt->begin(); keyIt != ownerIt->end(); ++keyIt) {
            if (!keyIt->is_string()) {
                ++malformed;
                continue;
            }
            const std::string value = keyIt->get<std::string>();
            if (value.empty()) {
                continue;  // 空串视为缺失，不算坏数据
            }
            const std::string key = keyIt.key();
            if (key.empty() || key == "@meta") {
                // @meta 是 i18n_list 的保留 id，宿主的 lists.add 入口会整条丢弃。
                ++malformed;
                continue;
            }
            bucket.emplace(key, value);
        }
        if (!bucket.empty()) {
            texts.emplace(owner, std::move(bucket));
        }
    }
    if (malformed > 0) {
        log.warn("[translator] 语言包里有 " + std::to_string(malformed) +
                 " 条文本格式不合法（不是字符串/键为空/用 @meta 作键），已跳过：" + path);
    }
    if (texts.empty()) {
        log.warn("[translator] 语言包没有可用文案，已跳过：" + path);
        return false;
    }

    out.path = path;
    out.tag = std::move(tag);
    out.name = name.empty() ? out.tag : std::move(name);
    out.fallback = std::move(fallback);
    out.origin = origin;
    out.priority = priority;
    out.order = order;
    out.texts = std::move(texts);
    return true;
}

bool PackLoader::readFile(const std::filesystem::path& file, std::string& out) const {
    const shot::Logger log(m_host);
    const std::string path = pathToUtf8(file);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) {
        // 只报错误码不报 ec.message()：Windows 上它是本地编码（GBK），混进 UTF-8
        // 日志会变成乱码，反而更难查。
        log.warn("[translator] 读取语言包大小失败（错误码 " + std::to_string(ec.value()) +
                 "），已跳过：" + path);
        return false;
    }
    if (size > kMaxPackBytes) {
        log.warn("[translator] 语言包过大（" + std::to_string(size) + " 字节，上限 " +
                 std::to_string(kMaxPackBytes) + "），已跳过：" + path);
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        log.warn("[translator] 打开语言包失败，已跳过：" + path);
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out.empty()) {
        log.warn("[translator] 语言包是空文件，已跳过：" + path);
        return false;
    }
    return true;
}

std::string PackLoader::normalizeLanguageTag(std::string_view tag) {
    // 与宿主 Host::I18n::normalizeTag / helper 同一套规则：语言小写、4 字母脚本
    // 首字母大写、2 字母地区（或 3 位数字）大写，其余小写；"_" 当成 "-"。
    // 不统一的话 "zh_CN" 与 "zh-CN" 会被当成两种语言，用户写错一个分隔符就整包失效。
    std::string out;
    std::size_t start = 0;
    int part = 0;
    while (start < tag.size()) {
        const std::size_t separator = tag.find_first_of("-_", start);
        const std::size_t end = (separator == std::string_view::npos) ? tag.size() : separator;
        const std::string_view piece = tag.substr(start, end - start);
        start = (separator == std::string_view::npos) ? tag.size() : separator + 1;
        if (piece.empty()) {
            continue;
        }
        if (part > 0) {
            out.push_back('-');
        }
        bool allAlpha = true;
        bool allDigit = true;
        for (const char ch : piece) {
            const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            const bool digit = (ch >= '0' && ch <= '9');
            allAlpha = allAlpha && alpha;
            allDigit = allDigit && digit;
        }
        const auto lowerAll = [&out, piece]() {
            for (const char ch : piece) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
        };
        if (part == 0) {
            lowerAll();
        } else if (allAlpha && piece.size() == 4) {
            for (std::size_t index = 0; index < piece.size(); ++index) {
                const char ch = piece[index];
                out.push_back(static_cast<char>(index == 0
                                                    ? std::toupper(static_cast<unsigned char>(ch))
                                                    : std::tolower(static_cast<unsigned char>(ch))));
            }
        } else if ((allAlpha && piece.size() == 2) || (allDigit && piece.size() == 3)) {
            for (const char ch : piece) {
                out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
            }
        } else {
            lowerAll();
        }
        ++part;
    }
    return out;
}

}  // namespace translator
