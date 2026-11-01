#include "plugin_meta.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>

#include <nlohmann/json.hpp>

#include "plugin_api.h"

namespace {

// 只在文件尾部这一小段里找标记：元数据贴着文件尾，读整个 DLL（几 MB）没必要。
constexpr std::streamoff kTailWindow = 64 * 1024;

std::string trim(const std::string& text) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    auto begin = std::find_if(text.begin(), text.end(), notSpace);
    auto end = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

}  // namespace

bool shotParseRequirement(const std::string& text, PluginRequirement& out) {
    out = PluginRequirement{};
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        return false;
    }
    out.raw = trimmed;

    // 运算符可能出现的位置：第一个 '>' '<' '='。
    std::size_t pos = trimmed.find_first_of("><=");
    if (pos == std::string::npos) {
        out.name = trimmed;  // 只声明名字 = 任意版本
        return !out.name.empty();
    }

    out.name = trim(trimmed.substr(0, pos));
    if (out.name.empty()) {
        return false;
    }

    std::string op;
    if (trimmed.compare(pos, 2, ">=") == 0 || trimmed.compare(pos, 2, "<=") == 0 ||
        trimmed.compare(pos, 2, "==") == 0) {
        op = trimmed.substr(pos, 2);
        pos += 2;
    } else if (trimmed[pos] == '>' || trimmed[pos] == '<') {
        op = trimmed.substr(pos, 1);
        pos += 1;
    } else {
        // 单个 '=' 也当成 ==，避免手写习惯差异带来的静默失效
        op = "==";
        pos += 1;
    }

    const std::string versionText = trim(trimmed.substr(pos));
    if (!shotParseVersion(versionText.c_str(), out.version)) {
        return false;
    }
    out.op = op;
    out.hasVersion = true;
    return true;
}

bool readPluginMeta(const std::string& dllPath, PluginPackageMeta& out, std::string& error) {
    out = PluginPackageMeta{};
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

    // 取【最后一对】标记：AppendPluginMeta 可能追加过多次，只有最后一份有效。
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

    out.schema = doc.value("schema", 0);
    if (doc.contains("abi") && doc["abi"].is_object()) {
        out.abiMajor = doc["abi"].value("major", 0);
        out.abiMinor = doc["abi"].value("minor", 0);
    }

    const auto pluginsIt = doc.find("plugins");
    if (pluginsIt == doc.end() || !pluginsIt->is_array()) {
        error = "元数据里没有 plugins 数组";
        return false;
    }

    for (const auto& item : *pluginsIt) {
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
                PluginRequirement requirement;
                if (shotParseRequirement(value.get<std::string>(), requirement)) {
                    entry.requireList.push_back(std::move(requirement));
                } else {
                    // 语法坏的依赖当成"无法满足"，宁可跳过这个插件也不误加载
                    PluginRequirement broken;
                    broken.name = value.get<std::string>();
                    broken.raw = broken.name;
                    broken.op = ">= " ;  // 故意不可能满足
                    broken.hasVersion = true;
                    entry.requireList.push_back(std::move(broken));
                }
            }
        }
        if (!entry.id.empty()) {
            out.plugins.push_back(std::move(entry));
        }
    }

    if (out.plugins.empty()) {
        error = "元数据里没有一个带 id 的插件";
        return false;
    }
    return true;
}
