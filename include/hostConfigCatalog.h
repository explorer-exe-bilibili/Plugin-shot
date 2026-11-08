#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "plugin_api.h"

namespace Plugin {
class PluginManager;
class PluginBase;
}  // namespace Plugin

// ============================================================================
//  Host::HostConfigCatalog —— 宿主配置目录（shot.host.plugin_config）
//
//  把"宿主自己（core）+ 所有实现了 shot.configurable 的插件实例"聚成一张可枚举
//  目录：索引 0 固定是 core，其余按加载顺序排列。设置页只面向这一张表，不需要
//  逐个去问插件；存盘键由这里统一拼成 <provider_id>/<key>。
//
//  线程模型：服务函数可能在任意插件线程上被调用。它们持有 HostRuntime 的生命
//  周期锁再调用插件，保证"读值/写值"期间插件不会被热卸载；枚举走实时快照，
//  每次都从 PluginManager 取最新的实例列表。
// ============================================================================

namespace Host {

/**
 * @brief 配置目录：ShotHostPluginConfigV1 的实现。
 *
 * 进程内只应有一个实例（HostRuntime 持有）；构造时把它注册为服务表回调的
 * 目标，析构时注销。没有 HostRuntime 时 service() 返回 nullptr，插件按
 * "宿主没有这个服务"安静降级。
 *
 * @warning 服务表里的函数指针指向插件代码；HostRuntime 必须保证卸载/加载
 *          期间持有同一把生命周期锁，否则设置页线程可能调用到已经销毁的实例。
 */
class HostConfigCatalog {
    /** @brief 一个可配置提供者：core（instance 为空）或某个插件实例。 */
    struct Provider {
        std::string id;                                /**< 提供者 id，如 "core" / "example.invert" */
        std::string name;                              /**< 界面显示名 */
        std::string version;                           /**< 提供者版本 */
        std::shared_ptr<Plugin::PluginBase> instance;  /**< 插件实例；core 时为空 */
        void* self = nullptr;                          /**< 传给能力函数表的 self */
        const ShotConfigurableV1* config = nullptr;    /**< 能力函数表 */
    };

public:
    /**
     * @brief 构造并注册为进程内唯一的配置目录。
     * @param[in] lifecycleMutex 宿主生命周期锁；服务函数在调用插件期间持有它。
     */
    explicit HostConfigCatalog(std::mutex& lifecycleMutex);
    /** @brief 注销服务表回调目标。 */
    ~HostConfigCatalog();

    /** @brief 禁止拷贝：一个进程只有一个配置目录。 */
    HostConfigCatalog(const HostConfigCatalog&) = delete;
    /** @brief 禁止赋值：一个进程只有一个配置目录。 */
    HostConfigCatalog& operator=(const HostConfigCatalog&) = delete;

    /**
     * @brief 绑定插件管理器（启动加载完成后调用）。
     * @param[in] manager 管理器；为空表示目录只剩 core 一项。
     */
    void bind(Plugin::PluginManager* manager);

    /** @brief 解绑插件管理器；之后目录只剩 core，且不再触碰任何插件实例。 */
    void unbind();

    /**
     * @brief 取服务表指针；没有存活的 HostConfigCatalog 时返回 nullptr。
     * @return 静态服务表，调用方只读、不得释放。
     */
    static const ShotHostPluginConfigV1* service();

    /**
     * @brief 通知目录"生命周期可能变了"。
     *
     * 目录是实时枚举的：每次服务调用都从 PluginManager 取最新实例快照，没有缓存
     * 需要重建。这个入口保留下来表达"目录可能变了"；以后若加入按 provider 缓存
     * 配置声明的优化，重建点就放这里，调用方不用改。
     */
    void refresh();

    /**
     * @brief 立刻释放两份提供者快照（调用方必须已持有生命周期锁）。
     *
     * 快照里存着插件实例的 shared_ptr：热卸载前必须先放掉，否则实例会活到下一次
     * 目录调用，导致 module->shutdown 早于实例 destroy（违反 ABI 生命周期约定），
     * 而且 DLL 的最后一个句柄也释放不掉。
     */
    void dropSnapshots();

    /**
     * @brief 把已存值 / default_value 灌回所有提供者（启动与重新启用后调用）。
     *
     * 对每个提供者的每个配置项：宿主配置里有 `<id>/<key>` 就用已存值；没有就用
     * desc.default_value 并写进宿主配置（这样第一次运行后配置文件就是完整的）；
     * 两者都没有则跳过。随后把值通过 set_config 推给提供者。
     *
     * @warning 调用方必须已持有构造时传入的生命周期锁（HostRuntime 负责）。
     * @return 成功灌入的配置项数；未绑定管理器时返回 -1。
     */
    int32_t applyStoredLocked();

private:
    /**
     * @brief 重建提供者快照并保活（调用方必须已持有生命周期锁）。
     *
     * 会调用插件的 query_interface，所以必须在生命周期锁内做。旧快照保留一代：
     * configurable_info 返回的字符串指针要活到调用方用完（契约要求"下一次目录
     * 调用前有效"），两代快照能覆盖并发读取的那一次。
     * @return 最新快照；调用方持有 shared_ptr 期间其中的实例不会被销毁。
     */
    std::shared_ptr<const std::vector<Provider>> snapshotProvidersLocked();

    /** @brief 按 id 找提供者；找不到返回 nullptr。 */
    static const Provider* findProvider(const std::vector<Provider>& providers, const char* id);

    /** @brief 构造索引 0 的 core 提供者（宿主设置）。 */
    static Provider coreProvider();

    /**
     * @brief 把一个提供者的配置项灌上已存值 / 默认值。
     * @param[in] provider 目标提供者。
     * @return 成功灌入的项数。
     */
    int32_t applyStoredToProvider(const Provider& provider);

    /** @brief 可配置提供者个数（含索引 0 的 core）。 */
    static uint32_t configurableCount(void* ctx);
    /** @brief 填提供者身份证；字符串指向当前快照。 */
    static int32_t configurableInfo(void* ctx, uint32_t index, ShotPluginInfo* out);
    /** @brief 按 provider id 取配置项个数。 */
    static uint32_t configCount(void* ctx, const char* providerId);
    /** @brief 取一项配置声明，并校验 key 非空、kind 已知。 */
    static int32_t configDesc(void* ctx, const char* providerId, uint32_t item,
                              ShotConfigDesc* out);
    /** @brief 转调提供者的 get_config。 */
    static int32_t getValue(void* ctx, const char* providerId, const char* key, char* buf,
                            uint32_t bufSize);
    /** @brief 转调提供者的 set_config，成功后记入宿主配置。 */
    static int32_t setValue(void* ctx, const char* providerId, const char* key, const char* value);
    /** @brief 服务表 apply_stored 的实现：加锁后调用 applyStoredLocked。 */
    static int32_t applyStored(void* ctx);
    /** @brief 配置落盘，成功后发布 `config.applied`。 */
    static int32_t save(void* ctx);

    std::mutex* m_lifecycleMutex = nullptr;  /**< 宿主生命周期锁；构造时绑定，进程内有效 */
    mutable std::mutex m_stateMutex;         /**< 保护 m_manager 与两份快照 */
    Plugin::PluginManager* m_manager = nullptr;/**< 插件实例来源；未绑定时为空 */
    std::shared_ptr<const std::vector<Provider>> m_current;   /**< 最新提供者快照 */
    std::shared_ptr<const std::vector<Provider>> m_previous;  /**< 上一份快照，保活字符串指针 */
};

}  // namespace Host
