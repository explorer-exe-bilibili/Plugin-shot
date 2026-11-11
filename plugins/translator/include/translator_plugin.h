#pragma once

// ============================================================================
//  plugins/translator/include/translator_plugin.h —— 插件主体：配置 + 重扫描编排
//
//  生命周期（Generic 插件没有 ui_provider 的 init/deinit）：
//    create() → start(host) 订阅 lists.changed、发布自己的配置项文案、登记"待扫描"
//    set_config(...)        → 只改值 + 置脏标记（绝不在任意线程发事件）
//    lists.changed 回调      → 宿主 main 线程；脏标记为真才真正扫描 + 发布
//    destroy() → stop()     退订、撤回自己贡献的条目、释放语言表
//
//  为什么重扫描挂在 lists.changed 上：本插件是 Generic 插件，宿主不提供"每帧
//  tick"回调；而 lists.changed 是宿主 main 线程上必然反复出现的事件（启动时
//  config_list / i18n_list 都会变化一次），且"配置变了"这件事本身就会引起
//  config_list 变化。用它可以做到"set_config 只登记、main 线程再干活"。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include "plugin_api.h"
#include "translator_config.h"

namespace shot {
class I18n;  // 多语言封装：定义在 shot_plugin_helpers/shot_i18n.h，只在本类 .cpp 里用完整类型
}  // namespace shot

namespace translator {

/**
 * @brief 语言包插件的主体：把契约回调转成类方法，自己持有配置与重扫描状态。
 *
 * 完全无状态的部分（目录解析、包解析、条目发布）都在 PackDirs / PackScanner /
 * PackLoader / SourcePublisher 里；这里只做"什么时候扫、扫到后要不要撤回"的编排
 * （STYLE §6.6：一个关注点一个类）。
 *
 * @note 能力函数表里的 self 是 PluginInstance*，不是本类；回调里先 state_of(self)
 *       再 static_cast 回 TranslatorPlugin。
 */
class TranslatorPlugin {
public:
    /** @brief 构造：配置取默认值（关闭、空目录），不碰任何宿主服务。 */
    TranslatorPlugin();

    /** @brief 析构：兜底调用 stop()，保证订阅与贡献都被撤回。 */
    ~TranslatorPlugin();

    /** @brief 不可拷贝：内部有互斥量与订阅句柄。 */
    TranslatorPlugin(const TranslatorPlugin&) = delete;
    /** @brief 不可赋值。 */
    TranslatorPlugin& operator=(const TranslatorPlugin&) = delete;

    /**
     * @brief 挂上宿主服务：订阅 lists.changed、发布配置项文案、登记首次扫描。
     *
     * 本插件没有 ui_provider 的 init/deinit，create(index, host) 是唯一能拿到宿主
     * 服务表的时机，所以由 plugin_entry.cpp 的 create_instance 在状态建好后调用。
     * 失败不致命：配置项照常可读写，只是不加载任何语言包。
     *
     * @param[in] host 宿主服务表；老宿主可能为 nullptr（安静降级）。
     * @return true = 已尽力初始化（不代表宿主支持列表服务）。
     */
    bool start(const ShotHostServices* host);

    /** @brief 退订事件、撤回贡献、释放语言表；重复调用是空操作。 */
    void stop();

    /**
     * @brief 配置项个数，转调 TranslatorConfig。
     * @return 配置项个数。
     */
    std::uint32_t configCount() const noexcept;

    /**
     * @brief 声明第 index 个配置项，转调 TranslatorConfig。
     * @param[in]  index 配置项下标。
     * @param[out] out   宿主提供并填好 struct_size 的输出结构。
     * @return 契约返回码：0 成功 / 1 参数非法 / 2 out 结构太短。
     */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const noexcept;

    /**
     * @brief 读一个配置项，转调 TranslatorConfig。
     * @return >= 0 = 值长度（不含 NUL）；负值 = key 不认识或参数非法。
     */
    std::int32_t getConfig(const char* key, char* buf, std::uint32_t bufSize) const;

    /**
     * @brief 写一个配置项：改值并登记"下一轮 main 线程重扫描"。
     *
     * 这里**不**扫描也不发事件：set_config 可能来自任意线程，而 lists.add 只允许
     * 宿主 main 线程处理。真正的扫描在 onListsChanged 里做。
     *
     * @return 0 = 已生效；负值 = key 不认识或值非法。
     */
    std::int32_t setConfig(const char* key, const char* value);

private:
    /**
     * @brief lists.changed 的宿主回调：只做一次原子判位，脏了才扫描。
     * @param[in] user  this。
     * @param[in] topic 主题（固定 "lists.changed"）。
     * @param[in] data  负载字节；可为 nullptr（本插件不解释负载，只当"main 线程到了"）。
     * @param[in] len   负载字节数。
     */
    static void onListsChanged(void* user, const char* topic, const void* data, std::uint32_t len);

    /** @brief 脏标记为真时执行一次 reload()（宿主 main 线程）。 */
    void applyPendingReload();

    /**
     * @brief 按当前配置重新扫描并发布（或撤回）语言包贡献。
     *
     * 关闭时撤回；开着但一个可用包都没有时也撤回上一轮（旧译文不能残留——包被删
     * 掉后界面继续显示旧语言，比回退到内置文案更让人困惑）。全程只 WARN/INFO，
     * 不报 ERROR。
     */
    void reload();

    /**
     * @brief 撤回"上一轮贡献过、这一轮不再产出"的条目 id。
     *
     * 只按 id 精确撤回、不做整段 owner 撤回：`translator.main` 这个 owner 名下还有
     * shot::I18n 发布的插件自身文案（配置项 label/tooltip、plugin.name），整段撤回
     * 会把它们一起删掉，而它们只会在下次 create 时重新发布。自身文案的 key 永远
     * 不参与撤回（见 ownTextKeys），代价是"包用自己的 key 覆盖自身文案"时该 key
     * 会一直留到宿主重启——这种覆盖本来就罕见，优先保证文案不消失。
     *
     * @param[in] nextIds 本轮发布的 id 集合（空集 = 本轮什么都没发布）。
     */
    void removeObsolete(const std::set<std::string>& nextIds);

    /**
     * @brief 本插件自身文案的 key 集合（来自 kLangTables 的键，不含 @meta）。
     * @return key 集合；只读，首次调用时解析一次。
     */
    static const std::set<std::string>& ownTextKeys();

    /** @brief 撤回本插件在 i18n_source_list 的全部条目（卸载路径用）。 */
    void withdrawContribution();

    const ShotHostServices* m_host = nullptr; /**< 宿主服务表；stop() 后置空 */
    TranslatorConfig m_config;                /**< 两个配置项 + 线程安全读写 */
    std::unique_ptr<shot::I18n> m_i18n;       /**< 本插件配置项文案；start() 构造，stop() 释放 */
    std::atomic<bool> m_reloadPending{false}; /**< set_config 置位；main 线程消费 */
    std::set<std::string> m_publishedIds;     /**< 本轮已发布的条目 id（只在 main 线程读写） */
    bool m_subscribed = false;                /**< 是否订阅了 lists.changed */
    bool m_contributed = false;               /**< 本实例是否发布过条目（决定卸载时要不要撤回） */
};

}  // namespace translator
