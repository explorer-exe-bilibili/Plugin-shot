#pragma once

#include <mutex>
#include <string>

#include "hostConfigCatalog.h"
#include "hostPluginRegistry.h"
#include "pluginManager.h"

// ============================================================================
//  Host::HostRuntime —— 宿主的运行期装配
//
//  把三件事装到一起：插件管理器（生命周期）、配置目录服务、插件注册表服务，
//  并提供一把生命周期锁：配置目录在调用插件期间持锁，宿主在卸载/加载插件前
//  持同一把锁，于是"设置页正在读值"和"插件被热卸载"不会撞在一起。
//
//  线程模型：init / tick 只在宿主 main 线程调用（与事件分发同线程）；
//  两个服务表的函数可以在任意插件线程调用。
// ============================================================================

namespace Host {

/**
 * @brief 宿主运行期：持有插件管理器与两个宿主服务，并在 tick 里执行异步启停。
 *
 * 构造即注册两个服务表（plugins 查询 host->query_interface 就能拿到）；
 * 析构时先卸载全部插件再注销服务——反过来会让还在跑的插件线程调到已销毁的对象。
 */
class HostRuntime {
public:
    HostRuntime();
    ~HostRuntime();

    /** @brief 禁止拷贝：运行期对象持有唯一的服务注册。 */
    HostRuntime(const HostRuntime&) = delete;
    /** @brief 禁止赋值：运行期对象持有唯一的服务注册。 */
    HostRuntime& operator=(const HostRuntime&) = delete;

    /**
     * @brief 启动装配：加载插件 → 绑定配置目录 → 回灌已存配置 → 绑定注册表。
     *
     * @param[in] pluginDir 插件目录（相对路径按宿主进程当前目录解析）。
     * @return true = 装配完成（个别插件失败只记录，不算失败）；false = 目录不可用。
     */
    bool init(const std::string& pluginDir);

    /**
     * @brief 主循环每轮调用一次：执行 pending 的插件启停，并把配置变更落盘。
     *
     * 启停整段持有生命周期锁；配置落盘只在有脏数据时做。
     * @note 只允许在宿主 main 线程调用。
     */
    void tick();

    /** @brief 插件管理器（宿主内部用；调用方自己保证只在 main 线程做生命周期操作）。 */
    Plugin::PluginManager& manager();
    /** @brief 配置目录服务对象。 */
    HostConfigCatalog& configCatalog();
    /** @brief 插件注册表服务对象。 */
    HostPluginRegistry& pluginRegistry();

private:
    std::mutex m_lifecycleMutex;        /**< 生命周期锁；声明在最前，配置目录要引用它 */
    Plugin::PluginManager m_manager;    /**< 插件生命周期与已安装清单 */
    HostConfigCatalog m_configCatalog;  /**< shot.host.plugin_config 服务 */
    HostPluginRegistry m_pluginRegistry;/**< shot.host.plugin_registry 服务 */
    bool m_configDirty = false;         /**< 配置内存有变更、等 tick 落盘 */
};

}  // namespace Host
