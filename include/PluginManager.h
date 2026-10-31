#pragma once

#include <string>
#include <unordered_map>
#include <memory>

#include "PluginInstances/base.h"

namespace Plugin{
class PluginManager{
    // <PluginType, PluginInstances>
    std::unordered_map<PluginType, std::vector<std::shared_ptr<PluginBase>>> m_plugins;
    // <DLL Path, Dllhander>
    std::unordered_map<std::string, std::weak_ptr<DllHandler>> m_dllCache;  // 缓存DLL
    bool loadSinglePlugin(const std::string& pluginPath);
    std::vector<std::shared_ptr<PluginBase>> loadPlugins
        (std::shared_ptr<DllHandler> dllHandle, const std::string& pluginPath);
public:
    PluginManager()=default;
    ~PluginManager() = default;

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