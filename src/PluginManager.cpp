#include "PluginManager.h"
#include "PluginInstances/ImageProcessor.h"
#include <iostream>
#include <filesystem>

using namespace Plugin;

bool PluginManager::loadPlugins(const std::string& dir){
    namespace fs = std::filesystem;
    if(!fs::exists(dir) || !fs::is_directory(dir)){
        std::cerr << "Plugin directory does not exist or is not a directory: " << dir << std::endl;
        return false;
    }
    for(const auto& entry : fs::directory_iterator(dir)){
        if(entry.is_regular_file()){
            std::string ext = entry.path().extension().string();
#ifdef _WIN32
            if(ext == ".dll")
#else
            if(ext == ".so" || ext == ".dylib")
#endif
                if(!loadSinglePlugin(entry.path().string())){
                    std::cerr << "Failed to load plugin: " << entry.path().string() << std::endl;
                }
        }
    }
    return true;
}

bool PluginManager::loadSinglePlugin(const std::string& pluginPath){
    std::string pathStr = pluginPath;
    std::shared_ptr<DllHandler> dllHandle;

    if(auto it = m_dllCache.find(pathStr); it != m_dllCache.end()){
        dllHandle = it->second.lock();
        if(!dllHandle){
            m_dllCache.erase(it);
        }
    }

    if(!dllHandle){
        try{
            dllHandle = std::make_shared<DllHandler>(pathStr);
            m_dllCache[pathStr] = dllHandle;
        }catch(const std::exception& e){
            std::cerr << "Failed to load DLL: " << pathStr << ", error: " << e.what() << std::endl;
            return false;
        }
    }

    try{
        auto abiFn = (int(*)())dllHandle->getFunction("plugin_abi_version");
        const int abi = abiFn();
        if(abi != PLUGIN_ABI_VERSION){
            std::cerr << "ABI mismatch in " << pathStr << ": plugin reports " << abi
                      << ", host requires " << PLUGIN_ABI_VERSION << ", refusing to load" << std::endl;
            return false;
        }

        auto createFn = (CPluginHandle*(*)())dllHandle->getFunction("plugin_create_processor");
        auto destroyFn = (void(*)(CPluginHandle*))dllHandle->getFunction("plugin_destroy");

        CPluginHandle* handle = createFn();
        if(!handle){
            std::cerr << "plugin_create_processor returned nullptr for DLL: " << pathStr << std::endl;
            return false;
        }
        if(handle->type != ::ImageProcessor || !handle->process){
            std::cerr << "Unsupported plugin handle (type " << handle->type << ") in DLL: " << pathStr << std::endl;
            destroyFn(handle);
            return false;
        }

        auto plugin = std::make_shared<ImageProcessorInstance>(dllHandle, handle, destroyFn);
        m_plugins[plugin->getType()].push_back(plugin);
        std::cout << "Successfully loaded plugin: " << plugin->getName()
                  << " (abi " << abi << ")" << std::endl;
    }
    catch(const std::exception& e){
        std::cerr << "Failed to load plugin from DLL: " << pathStr << ", error: " << e.what() << std::endl;
        return false;
    }
    return true;
}

void PluginManager::unloadAll(){
    m_plugins.clear();
    m_dllCache.clear();
}
