#pragma once

#include <string>
#include <utility>

#include "base.h"
#include "plugin_api.h"

namespace Plugin {
// 一个图像处理器实例。
//
// 三样东西要一起保管，缺一不可：
//   m_module   —— 插件包自己的模块表（destroy / query_interface 都在里面）
//   m_instance —— 插件自己分配、自己释放的不透明指针
//   m_ops      —— "shot.image_processor" 的函数表，可以直接调用
// m_ops 与 m_module 指向的都在插件 DLL 里，它们的有效性由 PluginBase 里的
// m_dllHandle 保证：实例活着，DLL 就不会被卸载。
class ImageProcessorInstance : public PluginBase {
    const ShotModule*           m_module = nullptr;
    void*                       m_instance = nullptr;
    const ShotImageProcessorV1* m_ops = nullptr;
    std::string                 m_name;

public:
    ImageProcessorInstance(std::shared_ptr<DllHandler> dllHandle,
                           const ShotModule* module,
                           void* instance,
                           const ShotImageProcessorV1* ops,
                           std::string name)
        : PluginBase(std::move(dllHandle)),
          m_module(module),
          m_instance(instance),
          m_ops(ops),
          m_name(std::move(name)) {}

    ~ImageProcessorInstance() override {
        // 插件分配的对象只能由插件自己释放，宿主绝不 delete。
        if (m_module != nullptr && m_module->destroy != nullptr && m_instance != nullptr) {
            m_module->destroy(m_instance);
            m_instance = nullptr;
        }
    }

    ImageProcessorInstance(const ImageProcessorInstance&) = delete;
    ImageProcessorInstance& operator=(const ImageProcessorInstance&) = delete;

    // 返回 0 才算成功；插件没提供 process 时按失败处理。
    bool process(ShotImage& image) {
        if (m_ops == nullptr || m_ops->process == nullptr) {
            return false;
        }
        return m_ops->process(m_instance, &image) == 0;
    }

    // 询问可选能力，例如 SHOT_IFACE_CONFIGURABLE。
    // 返回 nullptr 表示"这个插件没有这个能力"，属于正常情况。
    void* queryInterface(const char* ifaceId, uint32_t version) const {
        if (m_module == nullptr || m_module->query_interface == nullptr) {
            return nullptr;
        }
        return m_module->query_interface(m_instance, ifaceId, version);
    }

    // 插件实例的不透明指针。调用任何能力的函数表时都要把它作为 self 传进去。
    void* nativeHandle() const { return m_instance; }

    PluginType getType() const override { return PluginType::ImageProcessor; }
    std::string getName() const override { return m_name; }
};

}  // namespace Plugin
