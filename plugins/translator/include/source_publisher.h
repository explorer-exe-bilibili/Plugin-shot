#pragma once

// ============================================================================
//  plugins/translator/include/source_publisher.h —— 把语言包贡献进 i18n_source_list
//
//  条目协议（document/I18N.md §2.1、src/hostListCatalog.cpp 的 parseI18nSourceItem）：
//    文本条目：{"id":"<key>","target":"<owner>","priority":200|300,
//               "texts":{"<tag>":"<文本>"}}
//    语言声明：{"id":"@language/<tag>","texts":{"<tag>":"<显示名>"},"fallback":"..."}
//
//  一个关键限制：宿主按 (list, owner, id) 存条目，而本插件只能用一个 owner 身份
//  （translator.main）贡献。所以同一个 (target, key) 在官方包与用户包里各出现一次时，
//  不可能靠"发两条、让宿主按 priority 合并"——后发的会把先发的整条覆盖掉。这里改成
//  **先在插件内按 (target, key, tag) 做一次与宿主相同的合并**（priority 大者胜、
//  同 priority 后到者胜），再把合并结果作为一条条目发布；条目的 priority 取该
//  (target,key) 下胜出候选的最高值（200/300 之一）。这样：
//    · 同一 owner/key/tag：用户包（300）覆盖官方包（200）；
//    · 同一 key 的不同语言可以来自不同包（不会被整条覆盖吃掉）；
//    · 与其它贡献者（插件内置表 100）的优先级比较仍由宿主完成，包始终赢。
//
//  分块：lists.add / lists.remove 单条事件负载上限 64 KiB（document/EVENT_BUS.md §5）。
//  按 56 KiB 预算分块，单条条目本身就超限时跳过 + WARN——发出去也只会被宿主整条丢弃。
//
//  撤回分两种：removeIds() 精确撤掉"上一轮有、这一轮没有"的 id（重扫描用，不会误删
//  本插件自身文案）；withdraw() 按 owner 整段撤回（卸载用，此时连自身文案一起消失）。
// ============================================================================

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "pack_types.h"
#include "plugin_api.h"

namespace translator {

/**
 * @brief 语言包 → i18n_source_list 条目的发布器。
 *
 * 无状态（除宿主指针与自己的 owner）：一次 publish 对应一次"全量重新贡献"，
 * 调用方负责"是否该发布"的判断；重复发布同内容对宿主是幂等覆盖。
 */
class SourcePublisher {
public:
    /**
     * @brief 绑定宿主服务与贡献者身份。
     * @param[in] host    宿主服务表；nullptr 时所有发布安静失败。
     * @param[in] ownerId 本插件 id（lists.add 的 owner，必须等于宿主已加载的插件 id）。
     */
    SourcePublisher(const ShotHostServices* host, std::string ownerId);

    /**
     * @brief 合并全部包并按预算分块发布。
     *
     * 目标 owner 不是 core、也不是当前已加载插件时跳过并 WARN 一次：宿主会在
     * lists.add 入口丢弃这种条目，自己在发布前过滤可以少一条宿主 WARN，也避免
     * 把"包写错了 owner"伪装成宿主的问题。
     *
     * @param[in] packs 已解析的包（顺序 = 扫描顺序）；空列表等于不发布。
     * @return 本轮发布的条目 id 集合（宿主按 (owner,id) 存条目，这就是撤回时要用的键）。
     * @note 必须在宿主 main 线程调用（lists.add 只允许 main 线程处理）。
     */
    std::set<std::string> publish(const std::vector<ParsedPack>& packs);

    /**
     * @brief 按 id 精确撤回（重扫描后清理"不再产出的条目"）。
     * @param[in] ids 要撤回的 id；空集合是空操作。
     */
    void removeIds(const std::set<std::string>& ids);

    /**
     * @brief 按 owner 整段撤回本插件在 i18n_source_list 的全部条目。
     *
     * 省略 ids = 按 owner 全撤：卸载时连插件自身文案一起消失正是想要的，
     * 也不存在 id 数组撑爆单条事件的问题。
     */
    void withdraw();

private:
    /**
     * @brief 按 56 KiB 预算把已序列化的条目分块发 lists.add。
     * @param[in] items 每条已经是可直接拼进 items 数组的 JSON 文本。
     */
    void publishItems(const std::vector<std::string>& items);

    /**
     * @brief 宿主当前已加载的插件 id 集合。
     * @return id 集合；注册表服务不可用时为空集（调用方据此不做 target 过滤，
     *         交给宿主校验，宁可多一条宿主 WARN 也不误伤）。
     */
    std::set<std::string> loadedOwners() const;

    const ShotHostServices* m_host = nullptr; /**< 宿主服务表 */
    std::string m_owner;                      /**< 本插件 id（lists.add owner） */
};

}  // namespace translator
