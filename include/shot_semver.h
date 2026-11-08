#pragma once

// ============================================================================
//  include/shot_semver.h —— 版本比较（宿主与构建脚本共用同一套语义）
//
//  版本写成 MAJOR[.MINOR[.PATCH]]，缺的段按 0 处理；尾部附加信息（如 "-dev"）
//  被忽略。比较按数字段进行，不按字符串。
// ============================================================================

#include <cstdlib>
#include <string>

/**
 * @brief 依赖声明里的版本运算符。
 *
 * 出现在 "opencv>=4.12" 这种写法里：`None` 表示只写名字（任意版本），
 * `Invalid` 表示运算符写坏了——按"永远不满足"处理，让日志说真话。
 */
enum class Op {
    None,     /**< 任意版本（声明里只有名字） */
    Invalid,  /**< 语法坏的依赖当成"无法满足"，宁可跳过这个插件也不误加载 */
    Gt,       /**< ">" */
    Gte,      /**< ">=" */
    Lt,       /**< "<" */
    Lte,      /**< "<=" */
    Eq,       /**< "=="（单个 "=" 也按 "==" 处理） */
};

/**
 * @brief 只按数字段比较的版本号（MAJOR.MINOR.PATCH）。
 *
 * 不处理 semver 的预发布/构建元数据：parse 时尾部附加信息被忽略。
 * 数字段缺省按 0 处理，所以 "1" 与 "1.0.0" 相等。
 */
class ShotVersion {
public:
    /** @brief 默认构造：0.0.0。 */
    ShotVersion() = default;

    /**
     * @brief 直接给定三段版本号。
     * @param[in] major 主版本。
     * @param[in] minor 次版本。
     * @param[in] patch 修订号。
     */
    ShotVersion(int major, int minor, int patch) : major(major), minor(minor), patch(patch) {}

    /**
     * @brief 按数字段比较两个版本。
     * @param[in] other 另一个版本。
     * @return 小于 = -1，相等 = 0，大于 = 1。
     * @note 不做字符串比较，所以 1.10 > 1.9。
     */
    int compare(const ShotVersion& other) const {
        if (major != other.major) return major < other.major ? -1 : 1;
        if (minor != other.minor) return minor < other.minor ? -1 : 1;
        if (patch != other.patch) return patch < other.patch ? -1 : 1;
        return 0;
    }

    /**
     * @brief 判断当前版本是否满足 "op 版本" 形式的约束。
     * @param[in] op 运算符；Op::None 恒真，Op::Invalid 恒假。
     * @param[in] other 被约束的版本。
     * @return true = 满足；坏运算符返回 false。
     */
    bool satisfies(Op op, const ShotVersion& other) const {
        switch (op){
            case Op::None: return true;
            case Op::Invalid: return false;
            case Op::Gt: return compare(other) > 0;
            case Op::Gte: return compare(other) >= 0;
            case Op::Lt: return compare(other) < 0;
            case Op::Lte: return compare(other) <= 0;
            case Op::Eq: return compare(other) == 0;
        }
        return false;
    }

    /** @brief 规范化成 "MAJOR.MINOR.PATCH"（总是三段）。 */
    std::string to_string() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    /**
     * @brief 解析 "MAJOR[.MINOR[.PATCH]]"，尾部附加信息（如 "-dev"）被忽略。
     * @param[in] text 输入文本；空串直接返回 false。
     * @return true = 至少解析出一个数字段；false = 一个数字都没有或数字后格式不合法。
     * @note 解析失败时不修改已有字段。
     */
    bool parse(const std::string& text) {
        if (text.empty()) {
            return false;
        }
        int parts[3] = {0, 0, 0};
        int filled = 0;
        const char* p = text.c_str();
        while (filled < 3 && *p >= '0' && *p <= '9') {
            char* end = nullptr;
            const long value = std::strtol(p, &end, 10);
            if (end == p) {
                return false;
            }
            parts[filled++] = static_cast<int>(value);
            p = end;
            if (*p == '.') {
                ++p;
            } else {
                break;
            }
        }
        if (filled == 0) {
            return false;
        }
        major = parts[0];
        minor = parts[1];
        patch = parts[2];
        return true;
    }

    int major = 0;  /**< 主版本 */
    int minor = 0;  /**< 次版本 */
    int patch = 0;  /**< 修订号 */
};
