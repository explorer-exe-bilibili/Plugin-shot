#pragma once

// ============================================================================
//  include/shot_semver.h —— 版本比较（宿主与构建脚本共用同一套语义）
//
//  版本写成 MAJOR[.MINOR[.PATCH]]，缺的段按 0 处理；尾部附加信息（如 "-dev"）
//  被忽略。比较按数字段进行，不按字符串。
// ============================================================================

#include <cstdlib>
#include <string>

enum class Op {
    None,   // 任意版本
    Invalid,  // 语法坏的依赖当成"无法满足"，宁可跳过这个插件也不误加载
    Gt,     // ">"
    Gte,    // ">="
    Lt,     // "<"
    Lte,    // "<="
    Eq,     // "=="
};

class ShotVersion {
public:
    ShotVersion() = default;
    ShotVersion(int major, int minor, int patch) : major(major), minor(minor), patch(patch) {}
    int compare(const ShotVersion& other) const {
        if (major != other.major) return major < other.major ? -1 : 1;
        if (minor != other.minor) return minor < other.minor ? -1 : 1;
        if (patch != other.patch) return patch < other.patch ? -1 : 1;
        return 0;
    }
// 约束满足性。op 取 ">=" ">" "<=" "<" "=="；op 为空表示"任意版本"。
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
    std::string to_string() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }
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
    int major = 0;
    int minor = 0;
    int patch = 0;
};
