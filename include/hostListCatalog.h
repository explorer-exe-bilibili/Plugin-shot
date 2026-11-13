#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "plugin_api.h"

namespace Plugin {
class PluginManager;
class PluginBase;
}  // namespace Plugin

// ============================================================================
//  Host::HostListCatalog —— 宿主通用列表目录（shot.host.plugin_lists）
//
//  旧版是"配置目录"（只服务设置页，形状固定为 ShotConfigDesc）。插件生态长大
//  以后，托盘菜单、热键、目标列表……每一种新数据都想要一个 ABI 结构体，契约就
//  再也冻结不了。ABI 2.0 起换成一个通用容器：
//
//    · 数据结构 = 某个 list id 下的一组 JSON 条目。条目的 schema 由 list id
//      决定，不跨边界传结构体，所以"新增一种列表"只改 JSON，不改 ABI。
//    · 读 = shot.host.plugin_lists 的 list_count / list_id / list_json：
//      两段式只读快照，序列化结果按列表缓存，任意插件线程可调。
//    · 写 = lists.add / lists.remove / lists.update / lists.commit 事件。
//      宿主只在 main 线程分发事件，所以写路径天然单线程、不需要数据锁竞争。
//    · 变化通知 = lists.changed（宿主 → 所有，负载只有 list id）。同一轮分发里
//      多次变化合并成一个集合，由 HostRuntime::tick 统一 flush，每 list 只发一条。
//
//  两类列表：
//    · 宿主生成（generated）：config_list 由 core + 所有实现了 shot.configurable
//      的已加载实例实时生成（相当于旧配置目录的只读投影）；i18n_list 由宿主把
//      core 文案 + 各插件贡献的 i18n_source_list 条目按当前语言投影出来；
//      plugin_list 由宿主把 PluginManager 的已安装记录 + metadata 投影出来
//      （设置页插件管理页的数据源）。插件不能 add/remove 它们；写值走
//      lists.update、落盘走 lists.commit。
//    · 插件贡献：任意 list id 在第一次 lists.add 时自动注册；宿主按 owner
//      （= 贡献者插件 id）记账，插件被禁用/卸载时 removeOwner 清掉它的全部贡献。
//      tray_command_list 由宿主预置 core 三项（截屏 / 打开设置 / 退出），
//      插件贡献的条目按 order 插进来。
//
//  线程模型（契约要求 list_json 任意线程可调，而列表变更只在 main 线程）：
//    · m_mutex 只保护数据本身：list_* 服务函数持它读取/序列化，返回调用方缓冲。
//    · 列表变更（事件 / 生命周期清理 / config_list 重建）只允许在宿主 main
//      线程做；构造时记下 main 线程 id，非 main 线程的写入口直接拒绝并警告。
//      这把"约定"变成了运行期检查，而不是只写在注释里。
//    · 调用插件能力（query_interface / get_config / set_config）一律在生命周期锁
//      内、m_mutex 外：插件代码不会在宿主数据锁里执行，插件的回调也不会因为
//      "回调里读列表"而在同一把锁上重入死锁。
// ============================================================================

namespace Host {

/**
 * @brief 通用列表目录：ShotHostPluginListsV1 的实现 + 列表事件协议的宿主端。
 *
 * 进程内只应有一个实例（HostRuntime 持有）；构造时把它登记为服务表回调的目标，
 * 析构时注销。没有实例时 service() 返回 nullptr，插件按"宿主没有这个服务"安静降级。
 *
 * @warning 生命周期锁的纪律：applyStoredLocked / rebuildConfigListLocked 要求调用方
 *          已经持有构造时传入的生命周期锁（HostRuntime 负责），因为里面会调用插件
 *          能力表；handleEvent 在需要回调插件时会自己临时加锁（事件分发与 tick
 *          同在 main 线程，不会嵌套持有）。
 */
class HostListCatalog {
    /**
     * @brief 列表里的一条条目。
     *
     * 对外内容直接保存成【序列化后的 JSON 对象字符串】而不是 json 值，有两个好处：
     * 头文件不必把 nlohmann/json 拖进每个包含 hostListCatalog.h 的 TU；
     * 列表快照本身就是一个字符串缓存，拼装时直接抄，不重复序列化。
     */
    struct Entry {
        std::string owner;         /**< 贡献者插件 id；"core" 表示宿主预置（插件不可删） */
        std::string id;            /**< 条目 id，同一 owner 内唯一（upsert 的键） */
        std::int32_t order = 1000; /**< 排序权重，小的在前；缺省 1000 */
        std::string valueJson;     /**< 对外 JSON 对象的序列化结果，必须是合法 JSON 对象 */
    };

    /** @brief 一个列表的状态：条目 + 序列化缓存。 */
    struct ListState {
        std::vector<Entry> entries; /**< 已按 order 稳定排序；generated 列表按生成顺序 */
        std::string cache;          /**< list_json 返回的内容（JSON 数组文本） */
        bool cacheValid = false;    /**< false = 下次读取时从 entries 重建缓存 */
        /** true = 宿主生成（config_list / i18n_list / plugin_list），不接受插件贡献 */
        bool generated = false;
    };

    /** @brief 一个可配置提供者：core（instance 为空）或某个插件实例。 */
    struct Provider {
        std::string id;                                /**< 提供者 id，如 "core" / "example.invert" */
        std::string name;                              /**< 界面显示名 */
        std::string version;                           /**< 提供者版本 */
        std::shared_ptr<Plugin::PluginBase> instance;  /**< 插件实例；core 时为空 */
        void* self = nullptr;                          /**< 传给能力函数表的 self */
        const ShotConfigurableV1* config = nullptr;    /**< 能力函数表；不提供时为 nullptr */
    };

public:
    /**
     * @brief 构造并登记为进程内唯一的列表目录，同时预置 core 的托盘命令。
     * @param[in] lifecycleMutex 宿主生命周期锁；调用插件能力期间由调用方持有它。
     * @note 必须在宿主 main 线程构造：这里记下 main 线程 id，列表写入口据此拒绝
     *       其它线程的变更（读取不受影响）。
     */
    explicit HostListCatalog(std::mutex& lifecycleMutex);
    /** @brief 注销服务表回调目标。 */
    ~HostListCatalog();

    /** @brief 禁止拷贝：一个进程只有一个列表目录。 */
    HostListCatalog(const HostListCatalog&) = delete;
    /** @brief 禁止赋值：一个进程只有一个列表目录。 */
    HostListCatalog& operator=(const HostListCatalog&) = delete;

    /**
     * @brief 绑定插件管理器（启动加载完成后调用）。
     * @param[in] manager 管理器；为空表示只剩 core 与宿主预置列表。
     */
    void bind(Plugin::PluginManager* manager);

    /** @brief 解绑插件管理器并放掉两份提供者快照；之后不再触碰任何插件实例。 */
    void unbind();

    /**
     * @brief 取服务表指针；没有存活的 HostListCatalog 时返回 nullptr。
     * @return 静态服务表，调用方只读、不得释放。
     */
    static const ShotHostPluginListsV1* service();

    /**
     * @brief 处理一条列表事件（只允许在宿主 main 线程调用）。
     *
     * 支持 lists.add / lists.remove / lists.update / lists.commit；宿主自己发布的
     * lists.changed / lists.updated 再被插件回灌时忽略。负载按任务约定解析，非法
     * 负载只 warn 丢弃、绝不抛异常。
     *
     * @param[in] topic   事件主题（如 "lists.add"）。
     * @param[in] payload 事件负载字节（JSON 文本，可以没有结尾 NUL）。
     * @note 非 main 线程调用直接拒绝并警告：列表变更不允许来自插件线程。
     */
    void handleEvent(const std::string& topic, const std::string& payload);

    /**
     * @brief 立刻释放两份提供者快照（调用方必须已持有生命周期锁）。
     *
     * 快照里存着插件实例的 shared_ptr：热卸载前必须先放掉，否则实例会活到下一次
     * 目录调用，导致 module->shutdown 早于实例 destroy（违反 ABI 生命周期约定），
     * 而且 DLL 的最后一个句柄也释放不掉。
     */
    void dropSnapshots();

    /**
     * @brief 把已存值 / default_value 灌回所有可配置提供者（启动与重新启用后调用）。
     *
     * 对每个提供者的每个配置项：宿主配置里有 `<id>/<key>` 就用已存值；没有就用
     * desc.default_value 并写进宿主配置（这样第一次运行后配置文件就是完整的）；
     * 两者都没有则跳过。随后把值通过 set_config 推给提供者。
     *
     * @warning 调用方必须已持有生命周期锁（HostRuntime 负责）。
     * @return 成功灌入的配置项数；未绑定管理器时只回灌 core（返回 core 的项数）。
     */
    std::int32_t applyStoredLocked();

    /**
     * @brief 从已加载实例 + core 重建 config_list（调用方必须已持有生命周期锁）。
     *
     * 只有内容真的变了才标记 lists.changed，避免每次插件启停都向所有订阅者发
     * 一份"什么都没变"的通知。插件没有 get_config / 声明了非法项时安静跳过并
     * warn，不让一个坏插件把整个列表带崩。
     *
     * 结尾顺带调用 rebuildPluginListLocked()：插件清单与配置清单的刷新时机完全
     * 相同（插件加载完成、启停状态变化），复用同一个入口就不必在 HostRuntime
     * 里再加一处调用点，也不会漏掉未来的刷新路径。
     */
    void rebuildConfigListLocked();

    /**
     * @brief 请求重建 i18n_list（只置脏标记，只允许 main 线程调用）。
     *
     * 用于"语言被非 lists.update 路径改变"的场景（applyStored 回灌、host.config
     * 写入）：HostRuntime::tick 在重建之前对比当前语言，发现变化就调用这里。
     * 标记用原子量只是为了防御"将来有人在插件线程误调"时不留数据竞争；真正的
     * 重建仍然只在 main 线程做。
     */
    void requestI18nRebuild();

    /**
     * @brief 语言目录（`@language/` 声明）变化后，config_list 是否需要重建。
     *
     * 目录里出现/消失一个语言标签，core/language 的 ENUM 候选就要跟着变，而
     * options 是 config_list 的投影字段，只能整表重序列化。重建要读插件配置，
     * 调用方必须在 main 线程持有生命周期锁后调 rebuildConfigListLocked()；
     * 该函数结尾会清掉这个标记。
     *
     * @return true = 有待处理的目录变化；只允许 main 线程读。
     */
    bool configListStale() const;

    /**
     * @brief 需要时重建 i18n_list（当前语言快照，宿主生成、只读）。
     *
     * 触发点：启动（构造时标脏）、`core/language` 变化、`i18n_source_list` 贡献
     * 变化、插件启停/卸载清理。重建只读列表与事件，不调用插件能力，因此不需要
     * 生命周期锁；由 HostRuntime 在 init 与 tick 里 flushChanged 之前调用。
     *
     * @note 内部自己加 m_mutex，**调用方不得持有 m_mutex**（它不叫 Locked 就是为了
     *       和"调用方持生命周期锁"的 rebuildConfigListLocked 区分开）。
     */
    void rebuildI18nListIfDirty();

    /**
     * @brief 清掉某个 owner 在全部插件贡献列表里的条目，并标记 lists.changed。
     *
     * 插件被禁用 / 卸载 / 加载失败时由 HostRuntime 在 main 线程调用；generated
     * 列表不归这里管（config_list 由 rebuildConfigListLocked 按当前加载集合重建）。
     *
     * @param[in] owner 插件 id；"core" 是宿主自己的预置，忽略。
     */
    void removeOwner(const std::string& owner);

    /**
     * @brief 把本轮积累的 list 变更发布成 lists.changed（每 list 一条）。
     *
     * 只允许 main 线程调用。做的是"交换集合再发布"，同一轮里对同一个 list 的
     * 多次变更天然合并成一条；发布在 m_mutex 外做，不会和读取方抢锁。
     */
    void flushChanged();

private:
    /**
     * @brief lists.add 的实现：按 (owner, id) 覆盖并重排。
     * @param[in] payload 事件负载 JSON 文本。
     */
    void handleAdd(const std::string& payload);

    /**
     * @brief lists.remove 的实现：按 ids 删除；ids 省略或为空数组时删该 owner 全部。
     * @param[in] payload 事件负载 JSON 文本。
     */
    void handleRemove(const std::string& payload);

    /**
     * @brief lists.update 的实现：只支持 config_list，逐项转调 set_config 后回 lists.updated。
     * @param[in] payload 事件负载 JSON 文本。
     */
    void handleUpdate(const std::string& payload);

    /**
     * @brief lists.commit 的实现：config_list 落盘，成功发 config.applied、失败发 lists.updated。
     * @param[in] payload 事件负载 JSON 文本。
     */
    void handleCommit(const std::string& payload);

    /**
     * @brief owner 是否是当前已加载的插件实例（"core" 由调用方单独排除）。
     *
     * 事件总线不携带发送者身份（契约没有这个字段），所以宿主只能校验"这个 owner
     * 确实有一个已加载实例"，无法证明事件就是该插件发的。这条限制要写进文档：
     * 列表协议信任同一个进程里的插件，只防手滑、不防恶意。
     * @param[in] owner 候选 owner 插件 id。
     * @return true = 管理器里存在该 id 的已加载实例。
     */
    bool ownerLoaded(const std::string& owner) const;

    /**
     * @brief 取得当前管理器指针（持 m_mutex 读一次，避免与 unbind 竞争）。
     * @return 管理器指针；未绑定时为空。
     */
    Plugin::PluginManager* managerPointer() const;

    /**
     * @brief 重建提供者快照并保活（调用方必须已持有生命周期锁）。
     *
     * 会调用插件的 query_interface，所以必须在生命周期锁内做。旧快照保留一代：
     * 刚返回给调用方的字符串指针要活到下一次目录调用，两代快照能覆盖并发读取。
     * @return 最新快照；调用方持有 shared_ptr 期间其中的实例不会被销毁。
     */
    std::shared_ptr<const std::vector<Provider>> snapshotProvidersLocked();

    /**
     * @brief 按 id 在快照里找可配置提供者；找不到返回 nullptr。
     * @param[in] providers 提供者快照。
     * @param[in] id        提供者 id。
     */
    static const Provider* findProvider(const std::vector<Provider>& providers, const char* id);

    /** @brief 构造索引 0 的 core 提供者（宿主设置）。 */
    static Provider coreProvider();

    /**
     * @brief 把一个提供者的配置项灌上已存值 / 默认值。
     * @param[in] provider 目标提供者。
     * @return 成功灌入的项数。
     */
    std::int32_t applyStoredToProvider(const Provider& provider);

    /**
     * @brief 两段式读一个配置项的当前值。
     * @param[in]  provider 目标提供者。
     * @param[in]  key      配置键。
     * @param[out] out      成功时写入值；失败时清空。
     * @return true = 读到了（值可以为空串）；false = 插件不认识这个 key 或参数非法。
     */
    static bool readConfigValue(const Provider& provider, const char* key, std::string& out);

    /**
     * @brief 插入/覆盖一条条目并保持 order 升序（内部加 m_mutex）。
     * @param[in] list      列表 id。
     * @param[in] owner     贡献者 id。
     * @param[in] id        条目 id。
     * @param[in] order     排序权重。
     * @param[in] valueJson 对外 JSON 对象文本。
     */
    void upsertEntry(const std::string& list, const std::string& owner, const std::string& id,
                     std::int32_t order, const std::string& valueJson);

    /**
     * @brief 安装一份宿主生成的列表内容，内容变化时记入 pending changed。
     * @param[in] list    列表 id（config_list / i18n_list / plugin_list）。
     * @param[in] entries 新条目（按生成顺序，不排序）。
     */
    void installGeneratedList(const std::string& list, std::vector<Entry> entries);

    /**
     * @brief 重建 i18n_list 的实现（标记脏之后由 rebuildI18nListIfDirty 调用）。
     *
     * 条目顺序固定：首条 `@meta`（language/fallback），然后 core 文案，然后语言
     * 目录的显示名，最后按 (target,id) 合并后的文本条目。投影规则见
     * document/I18N.md §2、§5.3：core 内置表是 priority=100 的候选，插件/语言包
     * 的 target/priority 条目按"priority 大者胜、同 priority 后到者胜"覆盖；
     * `@language/` 声明只扩展语言目录、不作文本条目。
     */
    void rebuildI18nList();

    /**
     * @brief 把条目数组序列化成 list_json 的返回文本（JSON 数组）。
     * @param[in] entries 已排好序的条目。
     * @return JSON 数组文本；条目为空时是 "[]"。
     */
    static std::string serializeEntries(const std::vector<Entry>& entries);

    /**
     * @brief 重建 plugin_list：把已安装插件记录 + metadata 投影成只读清单。
     *
     * 数据来源是 PluginManager::getInstalledPlugins()（含被禁用与加载失败的记录）
     * 以及每个包 DLL 尾部的 metadata（description / provides / requires 只在
     * metadata 里）。metadata 通过 PluginPackageMeta::readFromFile 读取，不缓存：
     * 重建只发生在插件加载/启停与配置提交这些低频路径上，而缓存会在重装/替换
     * DLL 后变旧；逐包读一次文件尾比维护失效规则更不容易出错。
     *
     * 插件没有 metadata（或 metadata 里找不到自己的 id）时按 id 兜底：name 用 id、
     * description 为空、provides/requires 为空数组——宁可少显示，不把别的插件
     * 的元数据张冠李戴。
     *
     * @note 只允许宿主 main 线程调用；由 rebuildConfigListLocked() 统一触发。
     */
    void rebuildPluginListLocked();

    /** @brief 服务表 list_count 的实现。 */
    static std::uint32_t listCount(void* ctx);
    /** @brief 服务表 list_id 的实现：两段式写调用方缓冲。 */
    static std::int32_t listId(void* ctx, std::uint32_t index, char* buf, std::uint32_t bufSize);
    /** @brief 服务表 list_json 的实现：两段式写调用方缓冲，按列表缓存序列化结果。 */
    static std::int32_t listJson(void* ctx, const char* listId, char* buf, std::uint32_t bufSize);

    std::mutex* m_lifecycleMutex = nullptr;   /**< 宿主生命周期锁；构造时绑定，进程内有效 */
    std::thread::id m_mainThread;             /**< 宿主 main 线程 id：列表写入口只认它 */
    mutable std::mutex m_mutex;               /**< 保护 m_lists / m_pendingChanged / m_manager / 快照 */
    Plugin::PluginManager* m_manager = nullptr;                 /**< 插件实例来源；未绑定时为空 */
    std::map<std::string, ListState> m_lists;                   /**< list id -> 列表状态 */
    std::set<std::string> m_pendingChanged;                     /**< 待 flush 的 list id 集合（main 线程） */
    std::shared_ptr<const std::vector<Provider>> m_current;     /**< 最新提供者快照 */
    std::shared_ptr<const std::vector<Provider>> m_previous;    /**< 上一份快照，保活字符串指针 */
    bool m_writeThreadLogged = false;                           /**< 是否已记录过写线程（一次性日志） */
    std::atomic<bool> m_i18nDirty{true};                        /**< i18n_list 需要重建（只在 main 线程重建） */
    bool m_configListStale = false;                             /**< 语言目录变了、config_list 需重建（main 线程） */
};

}  // namespace Host
