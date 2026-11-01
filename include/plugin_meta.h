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
namespace Plugin{

class PluginRequirement{
    bool m_isBroken=false;  // 语法坏的依赖当成"无法满足"，宁可跳过这个插件也不误加载
    std::string name;
    Op op=Op::Invalid;
    ShotVersion version;     // op 非空时有效
    bool hasVersion = false;
    std::string raw;
    static std::string trim(const std::string& str);
    static Op parseOp(const std::string& opText);
    static bool isValidProviderName(const std::string& name);
public:
    PluginRequirement() = default;
    PluginRequirement(const std::string& text);
    const std::string& getName() const { return name; }
    const std::string& getRaw() const { return raw; }
    Op getOp() const { return op; }
    const ShotVersion& getVersion() const { return version; }
    operator std::string() const;
    bool satisfies(const std::string& requirementName, const ShotVersion& have) const;
    bool isBroken() const { return m_isBroken; }
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

class PluginPackageMeta {
    int schema = 0;
    int abiMajor = 0;
    int abiMinor = 0;
    std::vector<PluginMetaEntry> datas;
public:
    int getSchema() const { return schema; }
    int getAbiMajor() const { return abiMajor; }
    int getAbiMinor() const { return abiMinor; }
    const std::vector<PluginMetaEntry>& getMetaData() const { return datas; }
    bool readFromFile(const std::string& dllPath, std::string& error);
};

} // namespace Plugin
