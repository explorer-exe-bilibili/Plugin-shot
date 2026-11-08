#pragma once

#include <string>
#include <utility>

#include "base.h"
#include "plugin_api.h"

namespace Plugin {

/**
 * @brief "shot.lib_provider" 的宿主侧包装：只把运行库带进进程，不提供可调用算法。
 *
 * 生命周期规则与其他插件一致——实例由插件创建、由插件 destroy；
 * 这件事 PluginBase 已经做了，这里绝不重复一遍（重复 = 两次 destroy）。
 */
class LibProvider : public PluginBase {
    const ShotLibProviderV1* m_ops = nullptr;

public:
    /**
     * @brief 包装一个运行库提供者实例。
     * @param[in] dllHandle 插件 DLL 句柄。
     * @param[in] module 插件模块表。
     * @param[in] instance 插件分配的不透明实例。
     * @param[in] id 插件 id。
     * @param[in] name 插件显示名。
     * @param[in] version 插件版本。
     * @param[in] ops 能力函数表；可为空。
     */
    LibProvider(std::shared_ptr<DllHandler> dllHandle, const ShotModule* module, void* instance,
                std::string id, std::string name, std::string version,
                const ShotLibProviderV1* ops = nullptr)
        : PluginBase(std::move(dllHandle), module, instance, std::move(id), std::move(name),
                     std::move(version), PluginType::Lib),
          m_ops(ops) {}

    /**
     * @brief 取能力函数表原始指针。
     * @return 函数表；插件没有该能力时为空。
     * @note 读库信息优先用 getLibraryVersion()，不必自己拼 self。
     */
    const ShotLibProviderV1* getLibOps() const { return m_ops; }

    /**
     * @brief 询问插件报告的库版本。
     *
     * 能力的函数表要把 m_instance 当 self 传进去，宿主手上只有包装对象，
     * 所以这类"顺带读一下"的访问器写在这里，而不是让调用方自己拼参数。
     * @return 库版本字符串；插件没提供能力或没返回值时返回空串。
     * @note 返回的字符缓冲区属于插件静态存储，宿主只读、不释放。
     */
    std::string getLibraryVersion() const {
        if (m_ops != nullptr && m_instance != nullptr &&
            SHOT_HAS_FIELD(m_ops, ShotLibProviderV1, get_library_version) &&
            m_ops->get_library_version != nullptr) {
            const char* version = m_ops->get_library_version(m_instance);
            if (version != nullptr) {
                return version;
            }
        }
        return {};
    }
};

}  // namespace Plugin
