#pragma once

// ============================================================================
//  include/hotkey_catalog.h —— 列表驱动的热键目录（hotkey_list 的消费方）
//
//  热键不再写死在插件里：任何插件（包括 hotkey.main 自己）都可以贡献条目
//  {"list":"hotkey_list","owner":"<插件 id>","items":[
//     {"id","combo","event","data","label"}]}。
//  本类负责：
//    · create 时订阅 lists.changed、发布 hotkey.main 自己的三个内置条目；
//    · 读 shot.host.plugin_lists 的 hotkey_list 快照（失败/不存在按内置条目降级）；
//    · 把条目解析成注册表要用的模型（默认组合键、事件、负载、显示 key）；
//    · 动态配置项的值：实际组合键存在宿主配置 hotkey.main/<id>（与旧配置机制一致）。
//
//  线程模型：start / reload / set_config 都只在宿主 main 线程发生；热键线程通过
//  entries() 拿一份副本，不直接碰宿主服务。m_mutex 保护快照与变更回调。
// ============================================================================

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "plugin_api.h"

namespace hotkey {

/**
 * @brief 一条列表驱动的热键：默认组合键、事件与当前实际值。
 */
struct HotkeyEntry {
    std::string id;            /**< 条目 id，在 owner 内唯一；同时是配置键。 */
    std::string owner;         /**< 贡献者插件 id（"hotkey.main" 表示内置条目）。 */
    std::string label;         /**< 设置页显示用文本 key；缺省 = id。 */
    std::string defaultCombo;  /**< 列表里的默认组合键，如 "Ctrl+Alt+S"。 */
    std::string event;         /**< 按下时要发布的事件主题。 */
    std::string data;          /**< 事件负载；空串 = 空负载。 */
    std::string value;         /**< 当前实际值：配置存在时是配置原值，否则等于默认组合键。 */
    bool hasValue = false;     /**< true = hotkey.main/<id> 在宿主配置里存在（空串也是有效值）。 */
};

/**
 * @brief hotkey_list 的读取、内置贡献与动态配置值仓库。
 *
 * 为什么单独成一个类：列表快照、宿主配置读写、变更通知是"热键从哪来"的一件
 * 独立职责；HotkeyWindow 只负责"怎么注册"，HotkeyConfig 只负责"怎么暴露给设置页"
 * （STYLE.md §6.6 / §6.8）。
 */
class HotkeyCatalog {
public:
    /** @brief 构造空目录；此时还没有宿主服务与快照。 */
    HotkeyCatalog() = default;
    /** @brief 析构：等价于 stop()，不留下事件订阅。 */
    ~HotkeyCatalog();

    /** @brief 不可拷贝：持有事件总线订阅与共享快照。 */
    HotkeyCatalog(const HotkeyCatalog&) = delete;
    /** @brief 不可赋值。 */
    HotkeyCatalog& operator=(const HotkeyCatalog&) = delete;

    /**
     * @brief 在 create 阶段启动目录：迁移旧配置 → 发布内置条目 → 订阅 lists.changed → 读快照。
     *
     * 宿主没有列表服务 / 事件总线时安静降级：仍然给出内置三个条目，热键照常注册。
     *
     * @param[in] host 宿主服务表；可为 nullptr（老宿主：只用内置默认值）。
     * @return true = 至少订阅成功并发布过内置条目；false = 纯降级模式。
     */
    bool start(const ShotHostServices* host);

    /**
     * @brief 退订 lists.changed 并清掉变更回调；deinit / destroy 返回前必须调用。
     */
    void stop();

    /**
     * @brief 重新读一次 hotkey_list 快照并刷新动态配置值。
     *
     * 列表不存在、JSON 非法或单条字段不合法都只影响对应条目：整体按"内置条目 +
     * 能解析的贡献条目"降级，绝不抛异常、也不让插件失败。
     */
    void reload();

    /**
     * @brief 设置列表变化后的回调（通常让 HotkeyWindow 重注册）。
     * @param[in] handler 在宿主 main 线程执行；传空函数清除。
     */
    void setChangeHandler(std::function<void()> handler);

    /** @brief 当前条目个数（含内置条目）。 */
    std::uint32_t entryCount() const;

    /** @brief 当前条目副本；注册路径按这个顺序注册（内置条目在前）。 */
    std::vector<HotkeyEntry> entries() const;

    /**
     * @brief 读一条动态配置项给 shot.configurable 用。
     * @param[in]  id  条目 id。
     * @param[out] out 成功时写入（value = 配置原值或默认组合键）。
     * @return true = 存在这个条目；false = 不认识（宿主升级后多问一项也不会踩空）。
     */
    bool readConfigValue(const std::string& id, HotkeyEntry& out) const;

    /**
     * @brief 写一条动态配置项：更新内存快照并把值写进宿主配置 hotkey.main/<id>。
     *
     * 宿主自身也会在 lists.update 成功后写同一个键；这里先写一次是为了让注册路径
     * 立刻看到新值（宿主写盘在 set_config 返回之后）。
     *
     * @param[in] id    条目 id。
     * @param[in] value 新值（空串 = 该项不注册，沿用旧设置页约定）。
     * @return true = 已接受；false = id 不认识或宿主拒绝。
     */
    bool writeConfigValue(const std::string& id, const std::string& value);

    /**
     * @brief 事件总线回调：只处理 lists.changed 里的 hotkey_list。
     * @param[in] user  HotkeyCatalog 实例。
     * @param[in] topic 主题。
     * @param[in] data  负载。
     * @param[in] len   负载字节数。
     */
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);

private:
    /**
     * @brief hotkey.main 自己贡献的三个内置条目（catalog 的最终兜底）。
     * @return 固定三项，顺序 capture / region / settings。
     */
    static std::vector<HotkeyEntry> builtinEntries();

    /**
     * @brief 把旧版静态配置键一次性映射到新条目 id。
     *
     * 旧版（列表驱动之前）把三个热键存在 hotkey.main/capture_hotkey 等键上，
     * 新版按条目 id 存在 hotkey.main/capture 等键上。只有"新键不存在、旧键存在"
     * 时才写，所以重复 create（禁用→启用）是幂等的，不需要额外的"已迁移"标记。
     */
    void migrateLegacyConfig();

    /** @brief 发布 hotkey.main 自己的 hotkey_list 贡献（失败只 WARN，不阻止启动）。 */
    void publishContribution();

    /**
     * @brief 读宿主配置里的当前值。
     * @param[in]  id    条目 id（拼成 hotkey.main/<id>）。
     * @param[out] value 成功时写入；允许是空串。
     * @return true = 键存在（不存在或宿主没有配置服务时 false）。
     */
    bool readHostValue(const std::string& id, std::string& value) const;

    /** @brief 写宿主配置里的当前值；失败只 WARN。 */
    bool writeHostValue(const std::string& id, const std::string& value) const;

    const ShotHostServices* m_host = nullptr; /**< 宿主服务表；生命周期覆盖插件实例。 */
    bool m_subscribed = false;                /**< 是否成功订阅过 lists.changed。 */
    mutable std::mutex m_mutex;               /**< 保护 m_entries 与 m_changeHandler。 */
    std::vector<HotkeyEntry> m_entries;       /**< 当前快照（内置条目在前）。 */
    std::function<void()> m_changeHandler;    /**< 列表变化后的重注册回调。 */
};

}  // namespace hotkey
