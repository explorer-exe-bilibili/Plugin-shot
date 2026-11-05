#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <utility>
#include <vector>
#include <map>

#include "PluginInstances/base.h"

struct ShotModule;

namespace Plugin {
class PluginManager {
    // ---------------------------------------------------------------------------
    // 依赖图的节点 = 一个插件（不是 DLL）：同一包里不同插件可以有不同依赖。
    // ---------------------------------------------------------------------------
    struct Node;
    struct PreparedDll;
    // <PluginType, PluginInstances>
    std::unordered_map<PluginType, std::vector<std::shared_ptr<PluginBase>>> m_plugins;
    // <DLL Path, DllHandler>
    std::unordered_map<std::string, std::weak_ptr<DllHandler>> m_dllCache;  // 缓存DLL
    // 已 init 过的模块：卸载时要在所有实例销毁之后 shutdown，再放掉 DLL。
    std::vector<std::pair<std::shared_ptr<DllHandler>, const ShotModule*>> m_modules;

    static std::string nodeLabel(const Node& node);
    static bool checkRequirements(const Node& node,
                      const std::map<std::string, std::size_t>& providers,
                      const std::vector<Node>& nodes,
                      std::string& reason);
public:
    PluginManager()=default;
    ~PluginManager();

    // 扫描目录：先读每个 DLL 尾部的 metadata 建依赖图，按拓扑顺序加载，
    // 依赖不满足的插件单独跳过（不影响同包里其它插件和整个宿主）。
    bool loadPlugins(const std::string& dir);

    void unloadAll();

    template<typename T>
    std::vector<std::shared_ptr<T>> getPluginsByType(PluginType type)const {
        std::vector<std::shared_ptr<T>> result;
        auto it = m_plugins.find(type);
        if(it != m_plugins.end()){
            for(auto& plugin : it->second){
                if(auto casted = std::dynamic_pointer_cast<T>(plugin)){
                    result.push_back(casted);
                }
            }
        }
        return result;
    }
};
}  // namespace Plugin
