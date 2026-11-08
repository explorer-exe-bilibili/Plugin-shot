#pragma once

#include <string>
#include <utility>

#include "base.h"

namespace Plugin {
/**
 * @brief "shot.image_processor" 的宿主侧包装。
 */
class ImageProcessorInstance : public PluginBase {
    const ShotImageProcessorV1* m_ops = nullptr;
public:
    /**
     * @brief 包装一个图像处理器实例。
     * @param[in] dllHandle 插件 DLL 句柄。
     * @param[in] module 插件模块表。
     * @param[in] instance 插件分配的不透明实例。
     * @param[in] id 插件 id。
     * @param[in] name 插件显示名。
     * @param[in] version 插件版本。
     * @param[in] ops 能力函数表；可为空，此时 process 按失败处理。
     */
    ImageProcessorInstance(std::shared_ptr<DllHandler> dllHandle,
                           const ShotModule* module,
                           void* instance,
                           std::string id,
                           std::string name,
                           std::string version,
                           const ShotImageProcessorV1* ops)
        : PluginBase(std::move(dllHandle), module, instance, std::move(id), std::move(name),
                     std::move(version), PluginType::ImageProcessor),
          m_ops(ops) {}

    /**
     * @brief 原地处理一张图。
     * @param[in,out] image 待处理图像；按 stride 逐行寻址，stride 可为负。
     * @return true = 插件返回 0；false = 没有 process 或插件返回非 0。
     * @note 回调在宿主线程同步执行。
     */
    bool process(ShotImage& image) {
        if (m_ops == nullptr || m_ops->process == nullptr) {
            return false;
        }
        return m_ops->process(m_instance, &image) == 0;
    }
};

}  // namespace Plugin
