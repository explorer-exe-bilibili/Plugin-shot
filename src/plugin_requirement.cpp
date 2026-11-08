#include "plugin_requirement.h"

#include <algorithm>
#include <cctype>

using namespace Plugin;

// 提供者名字的合法字符集。名字里出现别的字符（例如 "opencv!"）说明整条依赖写坏了：
// 与其去匹配一个永远不会存在的提供者、报一句"没有提供者"误导人，不如直接判成语法
// 错误，让日志说真话。
bool PluginRequirement::isValidProviderName(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (const unsigned char c : name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '_' || c == '.' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string PluginRequirement::trim(const std::string& text) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    auto begin = std::find_if (text.begin(), text.end(), notSpace);
    auto end = std::find_if (text.rbegin(), text.rend(), notSpace).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

bool PluginRequirement::satisfies(const std::string& requirementName,
                                  const ShotVersion& have) const {
    if (m_isBroken) {
        return false;  // 语法坏的依赖当成"无法满足"，宁可跳过这个插件也不误加载
    }
    if (requirementName != name) {
        return false;
    }
    if (!hasVersion) {
        return true;  // 只声明名字 = 任意版本
    }
    return have.satisfies(op, version);
}

Op PluginRequirement::parseOp(const std::string& opText) {
    if (opText == ">") return Op::Gt;
    if (opText == ">=") return Op::Gte;
    if (opText == "<") return Op::Lt;
    if (opText == "<=") return Op::Lte;
    if (opText == "==" || opText == "=") return Op::Eq;
    return Op::Invalid;  // 认不出的运算符：当成不满足，宁可跳过也不误加载
}

PluginRequirement::PluginRequirement(const std::string& text) {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        m_isBroken = true;
        return;
    }
    raw = trimmed;

    // 运算符可能出现的位置：第一个 '>' '<' '='。
    std::size_t pos = trimmed.find_first_of("><=");
    if (pos == std::string::npos) {
        name = trimmed;  // 只声明名字 = 任意版本
        if (!isValidProviderName(name)) {
            m_isBroken = true;
            return;
        }
        op = Op::None;
        return;
    }

    name = trim(trimmed.substr(0, pos));
    if (!isValidProviderName(name)) {
        m_isBroken = true;
        return;
    }


    std::string opText;
    if (trimmed.compare(pos, 2, ">=") == 0 || trimmed.compare(pos, 2, "<=") == 0 ||
        trimmed.compare(pos, 2, "==") == 0) {
        opText = trimmed.substr(pos, 2);
        pos += 2;
    } else if (trimmed[pos] == '>' || trimmed[pos] == '<') {
        opText = trimmed.substr(pos, 1);
        pos += 1;
    } else {
        // 单个 '=' 也当成 ==，避免手写习惯差异带来的静默失效
        opText = "==";
        pos += 1;
    }

    op = parseOp(opText);
    const std::string versionText = trim(trimmed.substr(pos));
    if (!version.parse(versionText)) {
        m_isBroken = true;
        return;
    }
    hasVersion = true;
}
