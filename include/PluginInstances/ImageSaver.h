#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "base.h"
#include "plugin_api.h"

namespace Plugin {

/**
 * @brief "shot.image_saver" 的宿主侧包装。
 *
 * 保存策略（格式、目录布局、编码参数）全部来自插件自己的 shot.configurable
 * 配置项；宿主只传一张处理完的图像和可选的覆盖参数，换一个保存插件不需要改宿主。
 *
 * @warning save 只在宿主 main 线程上调用，同一实例不会被并发调用；实现无需考虑重入。
 */
class ImageSaverInstance : public PluginBase {
    const ShotImageSaverV1* m_ops = nullptr;

public:
    /**
     * @brief 包装一个图像保存实例。
     * @param[in] dllHandle 插件 DLL 句柄。
     * @param[in] module 插件模块表。
     * @param[in] instance 插件分配的不透明实例。
     * @param[in] id 插件 id。
     * @param[in] name 插件显示名。
     * @param[in] version 插件版本。
     * @param[in] ops 能力函数表；可为空，此时 save 按失败处理。
     */
    ImageSaverInstance(std::shared_ptr<DllHandler> dllHandle, const ShotModule* module,
                       void* instance, std::string id, std::string name, std::string version,
                       const ShotImageSaverV1* ops)
        : PluginBase(std::move(dllHandle), module, instance, std::move(id), std::move(name),
                     std::move(version), PluginType::ImageSaver),
          m_ops(ops) {}

    /**
     * @brief 把图像交给插件保存到文件。
     * @param[in]  image     待保存图像，只读；插件不得修改像素或结构体字段。
     * @param[in]  directory 覆盖输出目录（UTF-8）；nullptr = 用插件自己的配置。
     * @param[in]  basename  覆盖文件名（不含扩展名，UTF-8）；nullptr = 用插件配置。
     * @param[out] path      接收最终文件路径（含扩展名，NUL 结尾）的缓冲；不能为空。
     * @param[in]  pathSize  path 的字节数；建议 1024，返回 2 表示缓冲不足。
     * @return 0 = 已落盘；1 = 参数非法；2 = path 缓冲不足；3 = 保存失败。
     * @note 回调在宿主 main 线程同步执行，返回时文件已经写完。
     */
    int32_t save(const ShotImage& image, const char* directory, const char* basename,
                 char* path, uint32_t pathSize) const {
        if (m_ops == nullptr || m_ops->save == nullptr || path == nullptr) {
            return 1;
        }
        return m_ops->save(m_instance, &image, directory, basename, path, pathSize);
    }
};

}  // namespace Plugin
