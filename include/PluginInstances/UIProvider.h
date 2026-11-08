#pragma once

#include <memory>
#include <string>
#include <utility>

#include "base.h"
#include "plugin_api.h"

namespace Plugin {

/**
 * @brief "shot.ui_provider" 的宿主侧包装。
 *
 * 窗口/托盘归插件自己：它开自己的线程、跑自己的消息循环，宿主只调
 * init / deinit / postToPlugin 三个函数，从不接触 HWND，也不知道插件开了几个线程。
 */
class UIProvider : public PluginBase {
    const ShotUIProviderV1* m_ops = nullptr;
    bool m_initialized = false;

public:
    /**
     * @brief 包装一个 UI 提供者实例。
     * @param[in] dllHandle 插件 DLL 句柄。
     * @param[in] module 插件模块表。
     * @param[in] instance 插件分配的不透明实例。
     * @param[in] id 插件 id。
     * @param[in] name 插件显示名。
     * @param[in] version 插件版本。
     * @param[in] ops 能力函数表；可为空。
     */
    UIProvider(std::shared_ptr<DllHandler> dllHandle, const ShotModule* module, void* instance,
               std::string id, std::string name, std::string version,
               const ShotUIProviderV1* ops)
        : PluginBase(std::move(dllHandle), module, instance, std::move(id), std::move(name),
                     std::move(version), PluginType::UIProvider),
          m_ops(ops) {}

    /**
     * @brief 初始化插件 UI（幂等）。
     *
     * 重复调用是空操作，所以"禁用 -> 启用"直接 deinit(); init(); 即可；
     * 插件没提供 init 时按"没有要做的初始化"处理，不算失败。
     * @param[in] host 宿主服务表，转交插件 init；生命周期覆盖插件实例。
     * @return true = 已初始化（或无需初始化）；false = 插件初始化失败。
     * @note 返回 true 后需先 deinit() 才能重新 init()。
     */
    bool init(const ShotHostServices* host) {
        if (m_initialized) {
            return true;
        }
        if (m_ops == nullptr || m_ops->init == nullptr) {
            return true;
        }
        if (m_ops->init(m_instance, host) != 0) {
            return false;
        }
        m_initialized = true;
        return true;
    }

    /**
     * @brief 关闭插件 UI（幂等）。
     *
     * 返回时插件必须已经停掉线程、销毁窗口；返回值是宿主唯一能知道
     * "插件没关干净"的渠道，所以这个调用不能只放在析构里。
     * @return true = 正常关闭（或本来未初始化）；false = 插件关闭失败。
     */
    bool deinit() {
        if (!m_initialized) {
            return true;
        }
        m_initialized = false;
        if (m_ops == nullptr || m_ops->deinit == nullptr) {
            return true;
        }
        return m_ops->deinit(m_instance) == 0;
    }

    /**
     * @brief 向插件的 UI 线程投递事件，立即返回。
     *
     * 任意线程可调。
     * @param[in] ev 事件；name/data 按契约填写。
     * @return 0 = 已入队；-1 = 投递失败。
     * @note 插件没提供 post_to_plugin 时按"没有要投递的对象"处理，返回 0。
     */
    int32_t postToPlugin(const ShotEvent& ev) const override {
        if (m_ops == nullptr || m_ops->post_to_plugin == nullptr) {
            return 0;
        }
        return m_ops->post_to_plugin(m_instance, &ev) == 0 ? 0 : -1;
    }

    /**
     * @brief 析构兜底：没走 unloadAll 时补一次 deinit()。
     *
     * deinit() 幂等，重复调用是空操作。
     */
    ~UIProvider() override { deinit(); }
};

}  // namespace Plugin
