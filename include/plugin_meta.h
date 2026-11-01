#pragma once

// ============================================================================
//  include/plugin_meta.h —— 读插件 DLL 尾部的 metadata.json
//
//  元数据只是"声明"：它用于【加载前】预筛、排加载顺序、给界面填列表。
//  运行时一切以插件自己的回答为准（abi_major / get_info / query_interface），
//  两者不一致时宿主只警告。
//
//  读法：从文件尾部往前找最后一对标记（AppendPluginMeta 可能追加过多次，
//  只有最后一份有效），取出中间的 JSON 交给 nlohmann_json 解析。
// ============================================================================

#include <string>
#include <vector>

#include "shot_semver.h"

// 一条依赖声明：name[op version]，例如 "opencv>=4.12" / "core>=0.1.0" / "acme.shaper"。
struct PluginRequirement {
    std::string name;
    std::string op;          // ">=" ">" "<=" "<" "=="；空 = 任意版本
    ShotVersion version;     // op 非空时有效
    bool hasVersion = false;
    std::string raw;         // 原始文本，日志里直接打
};

struct PluginMetaEntry {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> interfaces;
    std::vector<PluginRequirement> requireList;  // 不能叫 requires：那是 C++20 关键字
    std::vector<std::string> provides;
};

struct PluginPackageMeta {
    int schema = 0;
    int abiMajor = 0;
    int abiMinor = 0;
    std::vector<PluginMetaEntry> plugins;
};

// 解析一条依赖声明文本（"opencv>=4.12.0"）。语法不合法返回 false。
bool shotParseRequirement(const std::string& text, PluginRequirement& out);

// 从 DLL 尾部读出元数据并解析。
// 返回 false 时 error 里是原因；调用方应当把它当成"这个包没有可用的元数据"，
// 照常加载但不做依赖检查（失败开放：一个包不能因为少了一段 JSON 就打不开）。
bool readPluginMeta(const std::string& dllPath, PluginPackageMeta& out, std::string& error);
