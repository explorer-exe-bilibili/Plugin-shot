#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <utility>
#include <vector>

#include "PluginInstances/base.h"
#include "plugin_api.h"

namespace Plugin {
class PluginManager{
    // <PluginType, PluginInstances>
    std::unordered_map<PluginType, std::vector<std::shared_ptr<PluginBase>>> m_plugins;
    // <DLL Path, Dllhander>
    std::unordered_map<std::string, std::weak_ptr<DllHandler>> m_dllCache;  // 缓存DLL
    // 已 init 过的模块：卸载时要在所有实例销毁之后 shutdown，再放掉 DLL。
    std::vector<std::pair<std::shared_ptr<DllHandler>, const ShotModule*>> m_modules;

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
}
