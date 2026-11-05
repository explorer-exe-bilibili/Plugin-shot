#pragma once

#include <string>
#include <utility>

#include "base.h"

namespace Plugin {
class ImageProcessorInstance : public PluginBase {
    const ShotImageProcessorV1* m_ops = nullptr;
public:
    ImageProcessorInstance(std::shared_ptr<DllHandler> dllHandle,
                           const ShotModule* module,
                           void* instance,
                           const ShotImageProcessorV1* ops,
                           std::string name)
        : PluginBase(std::move(dllHandle), module, instance, std::move(name), PluginType::ImageProcessor),
          m_ops(ops) {}

    // 返回 0 才算成功；插件没提供 process 时按失败处理。
    bool process(ShotImage& image) {
        if (m_ops == nullptr || m_ops->process == nullptr) {
            return false;
        }
        return m_ops->process(m_instance, &image) == 0;
    }
};

}  // namespace Plugin
