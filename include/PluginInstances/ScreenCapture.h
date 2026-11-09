#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "base.h"
#include "plugin_api.h"

namespace Plugin {

/**
 * @brief "shot.screen_capture" 的宿主侧包装。
 *
 * 采集是两段式：describe 只报告几何与像素格式，宿主据此自己分配一块 BGRA8
 * 缓冲，再调 capture 让插件往里写像素。插件因此从不分配图像内存，宿主也不
 * 需要跨 DLL 释放任何东西。
 *
 * @warning 两个能力函数都必须在宿主 main 线程上调用（契约把线程模型交给宿主：
 *          它需要为 GDI 抓屏保证线程可用性）；实现不得在返回后保存 dst/data。
 */
class ScreenCaptureInstance : public PluginBase {
    const ShotScreenCaptureV1* m_ops = nullptr;

public:
    /**
     * @brief 包装一个屏幕采集实例。
     * @param[in] dllHandle 插件 DLL 句柄。
     * @param[in] module 插件模块表。
     * @param[in] instance 插件分配的不透明实例。
     * @param[in] id 插件 id。
     * @param[in] name 插件显示名。
     * @param[in] version 插件版本。
     * @param[in] ops 能力函数表；可为空，此时 describe/capture 按失败处理。
     */
    ScreenCaptureInstance(std::shared_ptr<DllHandler> dllHandle, const ShotModule* module,
                          void* instance, std::string id, std::string name, std::string version,
                          const ShotScreenCaptureV1* ops)
        : PluginBase(std::move(dllHandle), module, instance, std::move(id), std::move(name),
                     std::move(version), PluginType::ScreenCapture),
          m_ops(ops) {}

    /**
     * @brief 报告采集目标的尺寸与像素格式。
     * @param[out] info 成功时填入；进入时 struct_size 由本函数填好。
     * @return 0 = 成功；1 = 当前没有可采集的屏幕；2 = 参数非法（含缺能力函）；
     *         3 = 系统调用失败。
     * @note 只读调用，不分配内存；回调在宿主 main 线程同步执行。
     */
    int32_t describe(ShotCaptureInfo& info) const {
        if (m_ops == nullptr || m_ops->describe == nullptr) {
            return 2;
        }
        info = ShotCaptureInfo{};
        info.struct_size = sizeof(ShotCaptureInfo);
        return m_ops->describe(m_instance, &info);
    }

    /**
     * @brief 让插件把屏幕像素写进宿主分配的图像。
     * @param[in,out] image 宿主已填好 data/width/height/stride/channels/pixel_format；
     *                      插件只能写像素，不得改结构体字段。
     * @return 0 = 成功；非 0 = 插件返回码（见 describe 的约定）。
     * @note 返回后插件不得保存 dst 或其 data 指针：下一张图宿主可能换内存。
     */
    int32_t capture(ShotImage& image) const {
        if (m_ops == nullptr || m_ops->capture == nullptr) {
            return 2;
        }
        return m_ops->capture(m_instance, &image);
    }
};

}  // namespace Plugin
