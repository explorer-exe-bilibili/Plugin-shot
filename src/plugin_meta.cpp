#include "plugin_meta.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#include "plugin_api.h"

using namespace Plugin;

// 只在文件尾部这一小段里找标记：元数据贴着文件尾，读整个 DLL（几 MB）没必要。
constexpr std::streamoff kTailWindow = 64 * 1024;

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
