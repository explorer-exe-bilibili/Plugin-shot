#pragma once

#include <memory>
#include <string>
#include <utility>

#include "base.h"
#include "plugin_api.h"

namespace Plugin {

// "shot.ui_provider" 的宿主侧包装。
//
// 窗口/托盘归插件自己：它开自己的线程、跑自己的消息循环，宿主只调
// init / deinit / postToPlugin 三个函数，从不接触 HWND，也不知道插件开了几个线程。
class UIProvider : public PluginBase {
    const ShotUIProviderV1* m_ops = nullptr;
    bool m_initialized = false;

public:
    UIProvider(std::shared_ptr<DllHandler> dllHandle, const ShotModule* module, void* instance,
               const ShotUIProviderV1* ops, std::string name)
        : PluginBase(std::move(dllHandle), module, instance, std::move(name), PluginType::UIProvider),
          m_ops(ops) {}

    // 0 = 成功。幂等：重复调用是空操作，所以"禁用 -> 启用"直接 deinit(); init(); 即可。
    // 插件没提供 init 时按"没有要做的初始化"处理，不算失败。
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

    // 关闭协议：返回时插件必须已经停掉线程、销毁窗口。幂等。
    // 返回值是宿主唯一能知道"插件没关干净"的渠道，所以这个调用不能只有析构。
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

    // 任意线程可调：宿主把事件投给插件的 UI 线程，本函数立即返回。
    // 0 = 已入队；-1 = 投递失败。插件没提供 post_to_plugin 时按"没有要投递的对象"
    // 处理，算成功（与 init / deinit 的"没提供就不算失败"一致）。
    int32_t postToPlugin(const ShotEvent& ev) const override {
        if (m_ops == nullptr || m_ops->post_to_plugin == nullptr) {
            return 0;
        }
        return m_ops->post_to_plugin(m_instance, &ev) == 0 ? 0 : -1;
    }

    // 兜底：万一有人没走 unloadAll 就把实例放了，这里补一次 deinit。
    // 因为 m_initialized 已被 deinit() 清掉，重复调用是空操作。
    ~UIProvider() override { deinit(); }
};

}  // namespace Plugin
