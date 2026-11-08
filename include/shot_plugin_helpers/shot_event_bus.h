#pragma once

// ============================================================================
//  include/shot_plugin_helpers/shot_event_bus.h —— 插件侧事件总线封装 shot::EventBus
//
//  包装 shot.host.event_bus：主题发布/订阅、退订；老宿主没有总线时安静降级。
//  本头文件只声明一个类；插件通常只包含汇总头 shot_plugin_helpers.h。
// ============================================================================

#include <cstdint>
#include <string_view>

#include "plugin_api.h"

namespace shot {

/**
 * @brief 插件侧事件总线封装：向宿主 query_interface 取 ShotHostEventBusV1。
 *
 * 老宿主没有总线时 available() 为 false，publish/subscribe 返回 false，不会崩。
 * 订阅回调运行在宿主分发线程上：里面只做入队/拷贝，耗时任务转到自己的线程。
 *
 * 用法（三行）：
 *   shot::EventBus bus(host);
 *   bus.subscribe("capture.done", &on_capture_done, this);
 *   bus.publish("capture.request", payload);
 */
class EventBus {
public:
    /** @brief 订阅回调类型：user 是 subscribe 时传入的上下文指针。 */
    using Callback = void (*)(void* user, const char* topic, const void* data, uint32_t len);

    /**
     * @brief 构造时向宿主索要事件总线。
     * @param[in] host 宿主传给 create()/init() 的服务表指针；nullptr 时 available() 为 false。
     */
    explicit EventBus(const ShotHostServices* host);

    /** @brief 宿主是否提供了可用的事件总线（老宿主可能没有）。 */
    bool available() const noexcept;

    /**
     * @brief 发布一条事件给所有匹配的订阅者（非阻塞，负载会被深拷贝）。
     * @param[in] topic   主题；不能为 nullptr。
     * @param[in] payload 负载字节；空串表示无负载。
     * @return true = 已入队；宿主不支持、参数非法、负载超限或队列满时 false。
     */
    bool publish(const char* topic, std::string_view payload = {}) const;

    /**
     * @brief 订阅主题（支持 "capture.request" / "capture.*" / "*"）。
     * @param[in] topic    主题模式；不能为 nullptr。
     * @param[in] callback 匹配时的回调；不能为 nullptr。
     * @param[in] user     回调上下文，也是退订句柄；可为 nullptr。
     * @return true = 订阅成功。
     */
    bool subscribe(const char* topic, Callback callback, void* user) const;

    /**
     * @brief 退订该 user 的全部订阅；deinit 返回前必须调用。
     * @param[in] user subscribe 时传入的上下文指针。
     * @return true = 至少退订了一条。
     */
    bool unsubscribe(void* user) const;

private:
    const ShotHostServices* m_host = nullptr;
    const ShotHostEventBusV1* m_bus = nullptr;
};

inline EventBus::EventBus(const ShotHostServices* host) : m_host(host) {
    if (host == nullptr) {
        return;
    }
    if (!SHOT_HAS_FIELD(host, ShotHostServices, query_interface) ||
        host->query_interface == nullptr) {
        return;
    }
    void* iface = host->query_interface(host->host_context, SHOT_HOST_IFACE_EVENT_BUS, 1);
    if (iface == nullptr) {
        return;
    }
    const auto* bus = static_cast<const ShotHostEventBusV1*>(iface);
    if (!SHOT_HAS_FIELD(bus, ShotHostEventBusV1, publish) || bus->publish == nullptr) {
        return;
    }
    m_bus = bus;
}

inline bool EventBus::available() const noexcept {
    return m_host != nullptr && m_bus != nullptr && m_bus->publish != nullptr;
}

inline bool EventBus::publish(const char* topic, std::string_view payload) const {
    if (!available() || topic == nullptr) {
        return false;
    }
    const void* data = payload.empty() ? nullptr : payload.data();
    return m_bus->publish(m_host->host_context, topic, data,
                          static_cast<uint32_t>(payload.size())) == 0;
}

inline bool EventBus::subscribe(const char* topic, Callback callback, void* user) const {
    if (!available() || topic == nullptr || callback == nullptr) {
        return false;
    }
    if (!SHOT_HAS_FIELD(m_bus, ShotHostEventBusV1, subscribe) || m_bus->subscribe == nullptr) {
        return false;
    }
    return m_bus->subscribe(m_host->host_context, topic, callback, user) == 0;
}

inline bool EventBus::unsubscribe(void* user) const {
    if (!available()) {
        return false;
    }
    if (!SHOT_HAS_FIELD(m_bus, ShotHostEventBusV1, unsubscribe) || m_bus->unsubscribe == nullptr) {
        return false;
    }
    return m_bus->unsubscribe(m_host->host_context, user) == 0;
}

}  // namespace shot

