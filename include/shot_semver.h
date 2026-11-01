#pragma once

// ============================================================================
//  include/shot_semver.h —— 版本比较（宿主与构建脚本共用同一套语义）
//
//  版本写成 MAJOR[.MINOR[.PATCH]]，缺的段按 0 处理；尾部附加信息（如 "-dev"）
//  被忽略。比较按数字段进行，不按字符串。
// ============================================================================

#include <cstdlib>
#include <cstring>
#include <string>

struct ShotVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

// 解析 "4.12.0" / "4.12" / "4"。失败返回 false。
inline bool shotParseVersion(const char* text, ShotVersion& out) {
    if (text == nullptr) {
        return false;
    }
    int parts[3] = {0, 0, 0};
    int filled = 0;
    const char* p = text;
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
    out.major = parts[0];
    out.minor = parts[1];
    out.patch = parts[2];
    return true;
}

inline int shotCompareVersion(const ShotVersion& a, const ShotVersion& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

// 约束满足性。op 取 ">=" ">" "<=" "<" "=="；op 为空表示"任意版本"。
inline bool shotSatisfies(const ShotVersion& have, const char* op, const ShotVersion& need) {
    if (op == nullptr || op[0] == '\0') {
        return true;
    }
    const int cmp = shotCompareVersion(have, need);
    if (std::strcmp(op, ">=") == 0) return cmp >= 0;
    if (std::strcmp(op, ">") == 0)  return cmp > 0;
    if (std::strcmp(op, "<=") == 0) return cmp <= 0;
    if (std::strcmp(op, "<") == 0)  return cmp < 0;
    if (std::strcmp(op, "==") == 0) return cmp == 0;
    return false;  // 认不出的运算符：当成不满足，宁可跳过也不误加载
}

inline std::string shotVersionToString(const ShotVersion& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}
