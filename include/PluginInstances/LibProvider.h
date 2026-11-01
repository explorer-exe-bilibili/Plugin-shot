#pragma once

#include <utility>

#include "base.h"
#include "plugin_api.h"

namespace Plugin{
// "shot.lib_provider" 的宿主侧包装：一个不提供可调用算法、只负责把运行库带进
// 进程的插件实例。生命周期规则与其他插件一致——实例由插件建、由插件 destroy。
class LibProvider:public PluginBase{
    void* m_instance = nullptr;
    void (*m_destroy)(void*) = nullptr;
    const ShotLibProviderV1* m_ops = nullptr;
    std::string m_name;
public:
    LibProvider(std::shared_ptr<DllHandler> dllHandle, void* instance, void (*destroy)(void*),
                const std::string& name, const ShotLibProviderV1* ops = nullptr)
        :PluginBase(dllHandle), m_instance(instance), m_destroy(destroy), m_ops(ops), m_name(name){}
    ~LibProvider() override {
        if(m_destroy && m_instance){
            m_destroy(m_instance);
            m_instance = nullptr;
        }
    };

    // 库版本之类以后要用就直接问这里，不必再回到模块表。
    const ShotLibProviderV1* getLibOps() const {return m_ops;}

    // 注意：能力的函数表要把 m_instance 当 self 传进去，宿主手上只有包装对象，
    // 所以这类"顺带读一下"的访问器写在这里，而不是让调用方自己拼参数。
    std::string getLibraryVersion() const {
        if(m_ops != nullptr && m_instance != nullptr &&
           SHOT_HAS_FIELD(m_ops, ShotLibProviderV1, get_library_version) &&
           m_ops->get_library_version != nullptr){
            const char* version = m_ops->get_library_version(m_instance);
            if(version != nullptr){
                return version;
            }
        }
        return {};
    }

    PluginType getType() const override {return PluginType::Lib;}
    std::string getName() const override {return m_name;}
};

}