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
//
//  依赖声明的解析在 plugin_requirement.h，本头文件只负责"一个包有哪些插件"。
// ============================================================================

#include <string>
#include <vector>

#include "plugin_requirement.h"

namespace Plugin {

/**
 * @brief metadata.json 里 `plugins[]` 的一项：一个插件的身份与依赖声明。
 *
 * 顺序即 create(index) 的索引——宿主靠它把"声明"对上"实现"。
 */
struct PluginMetaEntry {
    std::string id;                              /**< "作者.名字"，全局唯一 */
    std::string name;                            /**< 界面显示名 */
    std::string version;                         /**< 插件自己的语义化版本 */
    std::string description;                     /**< 一行说明，可为空 */
    std::vector<std::string> interfaces;         /**< 声称实现的能力 id（预筛用） */
    std::vector<PluginRequirement> requireList;  /**< 不能叫 requires：那是 C++20 关键字 */
    std::vector<std::string> provides;           /**< 向进程提供的名字（不带版本） */
};

/**
 * @brief 一个插件包（DLL 尾部那份 metadata.json）的解析结果。
 *
 * 宿主在 LoadLibrary 之前用它做预筛与依赖拓扑排序；读不到或解析失败不阻止加载，
 * 该包按"无元数据"处理（最后加载、不做依赖检查）。
 */
class PluginPackageMeta {
    int schema = 0;                     /**< 元数据格式版本，与 ABI 版本无关 */
    int abiMajor = 0;                   /**< 本包实现的契约 major */
    int abiMinor = 0;                   /**< 本包实现的契约 minor */
    std::vector<PluginMetaEntry> datas; /**< plugins[] 的顺序 == create(index) 的顺序 */
public:
    /** @brief 元数据格式版本（字段语义见 document/METADATA.md）。 */
    int getSchema() const { return schema; }
    /** @brief 本包实现的 ABI major。 */
    int getAbiMajor() const { return abiMajor; }
    /** @brief 本包实现的 ABI minor。 */
    int getAbiMinor() const { return abiMinor; }
    /** @brief 所有插件声明，顺序即 create(index) 的索引。 */
    const std::vector<PluginMetaEntry>& getMetaData() const { return datas; }

    /**
     * @brief 从文件尾读出 metadata.json 并解析。
     *
     * 只读尾部 64 KiB：元数据贴着文件尾，没必要读整个几 MB 的 DLL。
     * 只负责读取与 JSON 解析，不做语义校验（能力 id 是否注册、abi 是否匹配，
     * 交给 cmake/ValidatePluginMeta.cmake 在构建期拦）。
     *
     * @param[in]  dllPath 插件文件路径。
     * @param[out] error   失败时写入中文原因；成功时清空。
     * @return true = 读到合法 JSON 且至少有一个带 id 的插件。
     * @note 失败是正常情况（第三方 DLL 可能根本没有元数据），调用方继续加载即可。
     */
    bool readFromFile(const std::string& dllPath, std::string& error);
};

}  // namespace Plugin
