#pragma once

// ============================================================================
//  include/hotkey_config.h —— 热键的动态配置项（shot.configurable）
//
//  列表驱动之后设置页看到的不再是写死的三项，而是：
//    · enabled：唯一固定项（BOOL，总开关）；
//    · 每条 hotkey_list 条目一项：kind=STRING、key=<entry.id>、
//      label=<entry.label>（缺省 id）、default_value=<entry.combo>、options="hotkey"。
//
//  get_config_count / get_config_desc 每次调用都按当前快照现算，不缓存声明：
//  列表变化（lists.changed）之后宿主下一次枚举就能看到新条目。值仍然按旧机制
//  存在宿主配置 hotkey.main/<id>，本类只负责读写与声明，不自己落盘。
//
//  线程模型：所有回调都可能在宿主任意线程被调用；快照由 HotkeyCatalog 的互斥
//  保护，声明用的字符串放成员里（返回给宿主的指针在下一次调用前保持有效）。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "plugin_api.h"

namespace hotkey {

class HotkeyCatalog;

/**
 * @brief 全局热键的配置项：一个总开关 + hotkey_list 动态条目的组合键。
 *
 * 为什么把动态声明单独成类：配置声明与值读写是一件独立职责，和"从列表读条目"、
 * "在热键线程注册"分开后每个类都能一句话说清（STYLE.md §6.6 / §6.8）。
 */
class HotkeyConfig {
public:
    /**
     * @brief 构造：enabled = true，动态项从 catalog 现取。
     * @param[in] catalog 热键目录；必须活到本对象析构。
     */
    HotkeyConfig(HotkeyCatalog& catalog);

    /** @brief 不可拷贝：值是跨线程共享的状态，复制没有意义。 */
    HotkeyConfig(const HotkeyConfig&) = delete;
    /** @brief 不可赋值。 */
    HotkeyConfig& operator=(const HotkeyConfig&) = delete;

    /**
     * @brief 配置项个数：1（enabled）+ 当前热键条目数。
     *
     * 枚举前先让 HotkeyCatalog 重读一次 hotkey_list 快照：宿主在同一轮 tick 里
     * 先处理 lists.add、再重建 config_list，而 lists.changed 要等本轮 flush 后才
     * 送达；不在这里主动刷新，新贡献的热键条目会晚一整轮才出现在设置页。
     *
     * @return 动态计算的个数。
     */
    std::uint32_t configCount() const noexcept;

    /**
     * @brief 声明第 index 个配置项（0 = enabled，其余 = 当前列表条目）。
     * @param[in]  index 从 0 开始。
     * @param[out] out   由调用方提供并填好 struct_size；结构太短返回 2。
     * @return 0 = 已填入；1 = index 越界或 out 为空；2 = out 结构太短。
     */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const;

    /**
     * @brief 读一个配置项的当前值（值一律是字符串）。
     * @param[in]  key      配置键：enabled 或条目 id。
     * @param[out] buf      接收缓冲区，可为 nullptr（此时只查长度）。
     * @param[in]  buf_size buf 的字节数；大于返回值时才写入（含结尾 NUL）。
     * @return >= 0 = 值的字节数（不含结尾 NUL）；< 0 = 不认识的 key 或参数非法。
     */
    std::int32_t getValue(const char* key, char* buf, std::uint32_t bufSize) const;

    /**
     * @brief 写一个配置项；任意线程可调。
     *
     * 只保存字符串，不在这里校验热键语法：注册路径拿到值后统一解析，非法值会
     * 回落到列表默认组合键（见 hotkey_window_register.cpp）。
     *
     * @param[in] key   配置键：enabled 或条目 id。
     * @param[in] value 字符串形式的新值；enabled 的 "0"/"false" 视为关。
     * @return 0 = 已生效；< 0 = key 不认识或参数非法。
     */
    std::int32_t setValue(const char* key, const char* value);

    /** @brief 全局热键是否启用（`enabled` 的当前值）。 */
    bool enabled() const noexcept;

private:
    /**
     * @brief 按契约把字符串值写进宿主缓冲区并报告值的长度。
     * @param[in]  value   要返回的字符串；不能为 nullptr。
     * @param[out] buf     宿主缓冲区，可为 nullptr（只查询长度）。
     * @param[in]  buf_size buf 的字节数。
     * @return >= 0 = 值的字节数（不含结尾 NUL）；-1 = value 为空。
     */
    static std::int32_t copyValue(const char* value, char* buf, std::uint32_t bufSize);

    /**
     * @brief 把 ShotConfigDesc 整段拷进宿主给的结构。
     * @param[in]  src 插件内的声明。
     * @param[out] out 宿主提供的输出结构；进入时读 struct_size。
     * @return 0 = 已填入；1 = 参数为空；2 = out->struct_size 装不下声明。
     */
    static std::int32_t copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out);

    HotkeyCatalog& m_catalog;          /**< 条目与值都来自这里（不拥有）。 */
    std::atomic<bool> m_enabled{true}; /**< enabled：关闭时热键线程撤掉全部注册。 */

    // configDesc 返回的指针必须在宿主读完之前有效；宿主一次只问一项，所以用
    // 一组成员保存"上一条动态声明"的字符串（受 m_descMutex 保护）。
    mutable std::mutex m_descMutex;
    mutable std::string m_descKey;
    mutable std::string m_descLabel;
    mutable std::string m_descDefault;
};

}  // namespace hotkey
