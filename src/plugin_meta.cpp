#include "plugin_meta.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>

#include <nlohmann/json.hpp>

#include "plugin_api.h"

using namespace Plugin;

// 只在文件尾部这一小段里找标记：元数据贴着文件尾，读整个 DLL（几 MB）没必要。
constexpr std::streamoff kTailWindow = 64 * 1024;

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
    auto begin = std::find_if(text.begin(), text.end(), notSpace);
    auto end = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

bool PluginRequirement::satisfies(const std::string& requirementName,
                                  const ShotVersion& have) const {
    if(m_isBroken) {
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

bool PluginPackageMeta::readFromFile(const std::string& dllPath, std::string& error) {
    schema = 0;
    abiMajor = 0;
    abiMinor = 0;
    datas.clear();
    error.clear();

    std::ifstream file(dllPath, std::ios::binary);
    if (!file) {
        error = "打不开文件";
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        error = "空文件";
        return false;
    }
    const std::streamoff window = std::min(size, kTailWindow);
    file.seekg(size - window, std::ios::beg);
    std::string tail(static_cast<std::size_t>(window), '\0');
    if (!file.read(tail.data(), static_cast<std::streamsize>(window))) {
        error = "读文件尾失败";
        return false;
    }
    const std::string startMarker = PLUGIN_META_START_MARKER;
    const std::string endMarker = PLUGIN_META_END_MARKER;

    const std::size_t endPos = tail.rfind(endMarker);
    if (endPos == std::string::npos) {
        error = "文件尾没有元数据标记";
        return false;
    }

    const std::size_t startPos = tail.rfind(startMarker, endPos);
    if (startPos == std::string::npos) {
        error = "元数据标记不成对";
        return false;
    }

    const std::size_t jsonBegin = startPos + startMarker.size();
    if (jsonBegin >= endPos) {
        error = "元数据区间为空";
        return false;
    }

    const std::string jsonText = tail.substr(jsonBegin, endPos - jsonBegin);
    nlohmann::json doc = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        error = "元数据不是合法 JSON";
        return false;
    }
    schema = doc.value("schema", 0);
    if (doc.contains("abi") && doc["abi"].is_object()) {
        abiMajor = doc["abi"].value("major", 0);
        abiMinor = doc["abi"].value("minor", 0);
    }

    const auto pluginsIt = doc.find("plugins");
    if (pluginsIt == doc.end() || !pluginsIt->is_array()) {
        error = "元数据里没有 plugins 数组";
        return false;
    }

    for(const auto& item : *pluginsIt) {
        if (!item.is_object()) {
            continue;
        }
        PluginMetaEntry entry;
        entry.id = item.value("id", std::string{});
        entry.name = item.value("name", std::string{});
        entry.version = item.value("version", std::string{});
        entry.description = item.value("description", std::string{});

        if (const auto it = item.find("interfaces"); it != item.end() && it->is_array()) {
            for (const auto& value : *it) {
                if (value.is_string()) {
                    entry.interfaces.push_back(value.get<std::string>());
                }
            }
        }
        if (const auto it = item.find("provides"); it != item.end() && it->is_array()) {
            for (const auto& value : *it) {
                if (value.is_string()) {
                    entry.provides.push_back(value.get<std::string>());
                }
            }
        }
        if (const auto it = item.find("requires"); it != item.end() && it->is_array()) {
            for (const auto& value : *it) {
                if (!value.is_string()) {
                    continue;
                }
                entry.requireList.push_back(PluginRequirement(value.get<std::string>()));
            }
        }
        if (!entry.id.empty()) {
            datas.push_back(std::move(entry));
        }
    }

    if (datas.empty()) {
        error = "元数据里没有一个带 id 的插件";
        return false;
    }
    return true;
}
