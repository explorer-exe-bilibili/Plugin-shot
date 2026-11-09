#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "plugin_api.h"
#include "pluginManager.h"

// ============================================================================
//  Host::HostPluginRegistry —— 插件注册表（shot.host.plugin_registry）
//
//  设置页用它列"已安装插件"（含被禁用与加载失败的）并发起异步启停：
//  服务函数只登记请求（pending）就立刻返回，真正的热卸载/重新加载由宿主 main
//  线程在 HostRuntime::tick 里做，完成后发布 `plugins.changed`。
//
//  数据分两层：本地快照（服务函数只读它，不直接摸 PluginManager 内部）与
//  pending 变更表。快照由 refresh() 在 main 线程重建。
// ============================================================================

namespace Host {

/**
 * @brief 插件注册表：ShotHostPluginRegistryV1 的实现。
 *
 * 进程内只应有一个实例（HostRuntime 持有）；构造时注册为服务表回调目标，
 * 析构时注销，没有 HostRuntime 时 service() 返回 nullptr。
 *
 * @warning 服务函数可能在设置页的 UI 线程上被调用：它们绝不直接触发卸载，
 *          只往 pending 里放一条；宿主 main 线程下一轮 tick 才真正执行。
 */
class HostPluginRegistry {
public:
    /** @brief 注册表快照：一份"已安装插件"的拷贝，服务函数只读它。 */
    using Snapshot = std::vector<Plugin::InstalledPlugin>;

    /** @brief 构造并注册为进程内唯一的插件注册表。 */
    HostPluginRegistry();
    /** @brief 注销服务表回调目标。 */
    ~HostPluginRegistry();

    /** @brief 禁止拷贝：一个进程只有一个注册表。 */
    HostPluginRegistry(const HostPluginRegistry&) = delete;
    /** @brief 禁止赋值：一个进程只有一个注册表。 */
    HostPluginRegistry& operator=(const HostPluginRegistry&) = delete;

    /**
     * @brief 绑定插件管理器：refresh() 从这里取插件清单。
     * @param[in] manager 管理器；为空时刷新是空操作。
     */
    void bind(Plugin::PluginManager* manager);

    /** @brief 解绑插件管理器并清空快照。 */
    void unbind();

    /**
     * @brief 取服务表指针；没有存活的 HostPluginRegistry 时返回 nullptr。
     * @return 静态服务表，调用方只读、不得释放。
     */
    static const ShotHostPluginRegistryV1* service();

    /**
     * @brief 从 PluginManager 重建本地快照（只允许在 main 线程调用）。
     *
     * 保留上一份快照一代：刚返回给调用方的字符串指针要活到"下一次注册表调用"，
     * 两代快照能覆盖并发读取的那一次。
     */
    void refresh();

    /** @brief 是否有等待执行的启停请求（任何线程可调）。 */
    bool hasPending() const;

    /**
     * @brief 执行 pending 里的全部启停请求（只允许在 main 线程、持生命周期锁调用）。
     *
     * 逐条调用 PluginManager::setPluginEnabled（启用会做依赖级联）；重建快照后
     * **对比操作前后的状态**，为每个状态变化各发布一条 `plugins.changed` ——
     * 包括被级联启用的 provider。被拒绝/失败的请求即使状态没变也会发一条
     * （带 reason），否则设置页拿不到"为什么没生效"。
     *
     * @param[in] manager 插件管理器，执行真正的卸载/加载。
     * @return true = 本轮至少有一个插件从"未加载"变成"已加载"（含依赖级联启用），
     *         调用方需要为它们回灌已存配置；false = 没有 pending、或只有禁用/拒绝。
     * @note 注册表刻意不碰配置目录/列表目录：启用后回灌什么、列表怎么重建，是
     *       HostRuntime 的装配职责（STYLE.md §6.6 一个关注点一个类）。
     * @warning 调用方（HostRuntime）必须已持有生命周期锁，否则卸载可能与配置
     *          目录的插件调用并发。
     */
    bool applyPending(Plugin::PluginManager& manager);

private:
    /**
     * @brief 发布一条 `plugins.changed`（JSON 负载，格式见 SETTINGS.md §3.2）。
     * @param[in] pluginId 插件 id。
     * @param[in] enabled  处理后的状态（true = 已加载）。
     * @param[in] state    SHOT_PLUGIN_STATE_* 的数值。
     * @param[in] reason   拒绝/失败原因；成功时为空串。
     */
    void publishChanged(const std::string& pluginId, bool enabled, uint32_t state,
                        const std::string& reason);

    /** @brief 已安装插件条数。 */
    static uint32_t installedCount(void* ctx);
    /** @brief 取第 index 条插件状态。 */
    static int32_t installedInfo(void* ctx, uint32_t index, ShotPluginStatus* out);
    /** @brief 查询插件是否启用；没有这个 id 返回 -1。 */
    static int32_t isEnabled(void* ctx, const char* pluginId);
    /** @brief 异步登记启用/禁用请求。 */
    static int32_t setEnabled(void* ctx, const char* pluginId, int32_t enabled);

    mutable std::mutex m_mutex;                  /**< 保护快照与 pending */
    Plugin::PluginManager* m_manager = nullptr;  /**< 只在 main 线程读写 */
    std::shared_ptr<const Snapshot> m_current;   /**< 最新快照 */
    std::shared_ptr<const Snapshot> m_previous;  /**< 上一份快照，保活字符串指针 */
    std::map<std::string, bool> m_pending;       /**< id -> 目标状态；同 id 后写覆盖 */
};

}  // namespace Host
