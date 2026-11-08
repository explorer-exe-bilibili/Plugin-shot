#pragma once

// ============================================================================
//  include/shot_plugin_helpers/shot_plugin_registry.h —— 插件侧插件注册表封装 shot::PluginStatus / shot::PluginRegistry
//
//  包装 shot.host.plugin_registry：列出已安装插件（含禁用/失败）并异步请求启停。
//  本头文件只声明 PluginRegistry 与它的数据 struct PluginStatus（同一关注点）；
//  插件通常只包含汇总头 shot_plugin_helpers.h。
// ============================================================================

#include <cstdint>
#include <string>

#include "plugin_api.h"

namespace shot {

/**
 * @brief 插件注册表里的一条记录：字符串已拷成本地副本。
 *
 * 与 ShotPluginStatus 的区别同 ConfigDesc：注册表返回的指针由宿主持有，
 * 下一次调用就可能失效，所以列清单时当场拷走。
 */
struct PluginStatus {
    std::string id;       /**< 插件 id，如 "example.invert" */
    std::string name;     /**< 界面显示名 */
    std::string version;  /**< 插件版本，可能为空串 */
    std::string path;     /**< 插件 DLL/SO 路径 */
    std::string reason;   /**< 禁用/加载失败原因，没有时为空串 */
    uint32_t state = 0;   /**< SHOT_PLUGIN_STATE_LOADED / DISABLED / FAILED */
};

/**
 * @brief 插件侧插件注册表封装：向宿主 query_interface 取 ShotHostPluginRegistryV1。
 *
 * 启停是异步的：setEnabled 只登记请求并立即返回，宿主的 main 线程随后完成
 * 卸载/加载，再发布 `plugins.changed`。收到该事件后重新 installedCount /
 * installedInfo 一遍才是最新状态。
 */
class PluginRegistry {
public:
    /**
     * @brief 构造时向宿主索要注册表服务。
     * @param[in] host 宿主传给 create()/init() 的服务表指针；nullptr 时 available() 为 false。
     */
    explicit PluginRegistry(const ShotHostServices* host);

    /** @brief 宿主是否提供了可用的注册表服务（老宿主可能没有）。 */
    bool available() const noexcept { return m_host != nullptr && m_service != nullptr; }

    /** @brief 已安装插件（含禁用与加载失败）的条数。 */
    uint32_t installedCount() const noexcept;

    /**
     * @brief 取第 index 条插件记录。
     * @param[in]  index 0 .. installedCount()-1。
     * @param[out] out   成功时填入本地副本。
     * @return true = 成功；false = 服务不可用、index 越界或记录不完整。
     */
    bool installedInfo(uint32_t index, PluginStatus& out) const;

    /**
     * @brief 查询插件是否启用。
     * @return 1 = 启用；0 = 禁用；-1 = 服务不可用或没有这个 id。
     */
    int isEnabled(const char* pluginId) const noexcept;

    /**
     * @brief 请求启用/禁用插件（异步，登记后立即返回）。
     * @param[in] pluginId 插件 id。
     * @param[in] enabled  true = 启用，false = 禁用。
     * @return true = 已登记；false = 服务不可用、id 不认识或宿主拒绝。
     */
    bool setEnabled(const char* pluginId, bool enabled) const noexcept;

private:
    const ShotHostServices* m_host = nullptr;
    const ShotHostPluginRegistryV1* m_service = nullptr;
};

inline PluginRegistry::PluginRegistry(const ShotHostServices* host) : m_host(host) {
    if (host == nullptr) {
        return;
    }
    if (!SHOT_HAS_FIELD(host, ShotHostServices, query_interface) ||
        host->query_interface == nullptr) {
        return;
    }
    void* iface = host->query_interface(host->host_context, SHOT_HOST_IFACE_PLUGIN_REGISTRY, 1);
    if (iface == nullptr) {
        return;
    }
    const auto* service = static_cast<const ShotHostPluginRegistryV1*>(iface);
    if (!SHOT_HAS_FIELD(service, ShotHostPluginRegistryV1, set_enabled) ||
        service->installed_count == nullptr || service->installed_info == nullptr ||
        service->is_enabled == nullptr || service->set_enabled == nullptr) {
        return;
    }
    m_service = service;
}

inline uint32_t PluginRegistry::installedCount() const noexcept {
    if (!available()) {
        return 0;
    }
    return m_service->installed_count(m_host->host_context);
}

inline bool PluginRegistry::installedInfo(uint32_t index, PluginStatus& out) const {
    if (!available()) {
        return false;
    }
    ShotPluginStatus raw{};
    raw.struct_size = sizeof(ShotPluginStatus);
    if (m_service->installed_info(m_host->host_context, index, &raw) != 0) {
        return false;
    }
    if (raw.id == nullptr) {
        return false;
    }
    out.id = raw.id;
    out.name = (raw.name != nullptr) ? raw.name : "";
    out.version = (raw.version != nullptr) ? raw.version : "";
    out.path = (raw.path != nullptr) ? raw.path : "";
    out.reason = (raw.reason != nullptr) ? raw.reason : "";
    out.state = raw.state;
    return true;
}

inline int PluginRegistry::isEnabled(const char* pluginId) const noexcept {
    if (!available() || pluginId == nullptr) {
        return -1;
    }
    return m_service->is_enabled(m_host->host_context, pluginId);
}

inline bool PluginRegistry::setEnabled(const char* pluginId, bool enabled) const noexcept {
    if (!available() || pluginId == nullptr) {
        return false;
    }
    return m_service->set_enabled(m_host->host_context, pluginId, enabled ? 1 : 0) == 0;
}

}  // namespace shot

