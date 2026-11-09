#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "capturePipeline.h"
#include "hostListCatalog.h"
#include "hostPluginRegistry.h"
#include "pluginManager.h"

// ============================================================================
//  Host::HostRuntime —— 宿主的运行期装配
//
//  把四件事装到一起：插件管理器（生命周期）、通用列表目录、插件注册表、采集
//  流水线，并提供一把生命周期锁：列表目录在调用插件能力期间持锁，宿主在卸载/
//  加载插件前持同一把锁，于是"设置页正在读值"和"插件被热卸载"不会撞在一起。
//
//  它还负责把列表协议的时间点串起来（一个关注点一个类，事件处理在目录里，
//  装配时间点在运行期里）：
//   · init：加载插件 → 回灌已存配置 → 重建 config_list → 订阅 lists.* 与 app.quit；
//   · tick：执行 pending 启停 → 清理被卸载插件的列表贡献 → 回灌重新启用的配置
//           → 重建 config_list → 统一 flush lists.changed → 脏配置落盘。
//
//  线程模型：init / tick 只在宿主 main 线程调用（与事件分发同线程）；列表服务与
//  注册表服务的函数可以在任意插件线程调用；采集流水线由 CapturePipeline 自己的工作
//  线程执行（main 线程只负责把 capture.request 入队），执行期间持有本对象的生命周期锁。
// ============================================================================

namespace Host {

/**
 * @brief 宿主运行期：持有插件管理器、两个宿主服务与采集流水线。
 *
 * 构造即注册两个服务表（plugins 查询 host->query_interface 就能拿到）；
 * init 里订阅 lists.*、capture.request 与 app.quit；析构时先退订并卸载全部插件、
 * 再注销服务——反过来会让还在跑的插件线程调到已销毁的对象。
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
     * @brief 启动装配：加载插件 → 绑定列表目录 → 回灌已存配置 → 重建 config_list
     *        → 绑定注册表 → 订阅 lists.* 与 app.quit → 启动采集流水线。
     *
     * @param[in] pluginDir 插件目录（相对路径按宿主进程当前目录解析）。
     * @return true = 装配完成（个别插件失败只记录，不算失败）；false = 目录不可用。
     */
    bool init(const std::string& pluginDir);

    /**
     * @brief 主循环每轮调用一次：执行 pending 的插件启停、flush lists.changed、
     *        并把配置变更落盘。
     *
     * 启停整段持有生命周期锁；列表变更的清理与重建也在这段锁内做（因为要读插件
     * 配置）；配置落盘只在有脏数据时做。
     * @note 只允许在宿主 main 线程调用。
     */
    void tick();

    /**
     * @brief app.quit 是否已收到（收到后主循环应退出）。
     * @return true = 已收到退出请求。
     * @note 只由 main 线程读写：事件在分发线程（main）里才被置位。
     */
    bool quitRequested() const;

    /** @brief 插件管理器（宿主内部用；调用方自己保证只在 main 线程做生命周期操作）。 */
    Plugin::PluginManager& manager();
    /** @brief 通用列表目录服务对象（shot.host.plugin_lists）。 */
    HostListCatalog& listCatalog();
    /** @brief 插件注册表服务对象。 */
    HostPluginRegistry& pluginRegistry();
    /** @brief 采集流水线（主循环的 capture.request 与 --capture 共用同一个实例；内部工作线程）。 */
    CapturePipeline& capturePipeline();

private:
    /**
     * @brief 事件总线回调：收到 app.quit 后置退出标志。
     * @param[in] user this 指针。
     * @note 回调在宿主分发线程（main）执行，所以标志不需要原子量。
     */
    static void onAppQuit(void* user, const char* topic, const void* data, uint32_t len);

    /**
     * @brief 事件总线回调：把 lists.* 事件转给列表目录。
     *
     * 只做"拷贝负载 + 转发"：解析与写库都在 HostListCatalog::handleEvent 里，
     * 且一定运行在宿主 main 线程（事件总线的分发线程）。
     * @param[in] user  this 指针。
     * @param[in] topic 主题（lists.add / lists.remove / lists.update / lists.commit）。
     * @param[in] data  负载字节；可为 nullptr。
     * @param[in] len   负载字节数。
     */
    static void onListEvent(void* user, const char* topic, const void* data, uint32_t len);

    std::mutex m_lifecycleMutex;        /**< 生命周期锁；声明在最前，列表目录要引用它 */
    Plugin::PluginManager m_manager;    /**< 插件生命周期与已安装清单 */
    HostListCatalog m_listCatalog;      /**< shot.host.plugin_lists 服务 + lists.* 协议 */
    HostPluginRegistry m_pluginRegistry;/**< shot.host.plugin_registry 服务 */
    CapturePipeline m_capturePipeline;  /**< 采集 → 处理 → 保存流水线 */
    bool m_quitRequested = false;       /**< 是否收到 app.quit；只由 main 线程读写 */
    bool m_quitSubscribed = false;      /**< 是否已订阅 app.quit；析构时据此退订 */
    bool m_listsSubscribed = false;     /**< 是否已订阅 lists.*；与 app.quit 共用 user */
    bool m_configDirty = false;         /**< 配置内存有变更、等 tick 落盘 */
};

}  // namespace Host
