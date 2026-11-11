#pragma once

// ============================================================================
//  include/hotkey_config.h —— 全局热键的四个配置项（shot.configurable）
//
//  · enabled         BOOL   是否启用全局热键（默认 "1"）。
//  · capture_hotkey  STRING 截屏热键（默认 "Ctrl+Alt+S"）→ 发布 capture.request。
//  · region_hotkey   STRING 区域截屏热键（默认 "Ctrl+Alt+A"）→ 发布
//                           capture.request，负载 {"mode":"region"}。
//  · settings_hotkey STRING 打开设置热键（默认 "Ctrl+Alt+O"）→ 发布 settings.open。
//
//  热键字符串的语法与解析在 hotkey_binding.h；本类只负责"声明 + 存字符串值"，
//  不解释语法（解析失败发生在注册时，按"warn + 跳过这一项"处理）。
//
//  ShotConfigDesc 的 label/tooltip 是文案 key（config.<配置 key>.label / .tooltip），
//  由设置页用 i18n_list 解析成当前语言；语言表在 plugin_entry.cpp 的 kLangTables。
//
//  线程安全：值可能在宿主任意线程被读写（apply_stored、设置页应用、热键线程读），
//  所以三个字符串用 mutex 保护、enabled 用 std::atomic；读侧永远拿到完整值。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "plugin_api.h"

namespace hotkey {

/**
 * @brief 全局热键的配置项：一个总开关 + 三个热键字符串。
 *
 * 为什么单独成一个类：配置声明（ShotConfigDesc 表）、字符串解析后的"当前值"
 * 与值读写是热键插件的一件独立职责，和窗口/线程（HotkeyWindow）混在一起会让
 * 头文件不再是单一契约（STYLE.md §6.6 / §6.8）。ABI 回调在 plugin_entry.cpp
 * 里只做参数转发。
 *
 * @note 构造函数给出默认值（启用、Ctrl+Alt+S / Ctrl+Alt+A / Ctrl+Alt+O）；宿主
 *       启动的 apply_stored 会用已存值覆盖，第一次运行则把默认值写进配置。
 */
class HotkeyConfig {
public:
    /** @brief 构造：enabled = true，三个热键取默认值。 */
    HotkeyConfig();

    /** @brief 不可拷贝：值是跨线程共享的状态，复制没有意义。 */
    HotkeyConfig(const HotkeyConfig&) = delete;
    /** @brief 不可赋值。 */
    HotkeyConfig& operator=(const HotkeyConfig&) = delete;

    /**
     * @brief 配置项个数（供 shot.configurable 的 get_config_count 转发）。
     * @return 固定为 4。
     */
    std::uint32_t configCount() const noexcept;

    /**
     * @brief 声明第 index 个配置项。
     * @param[in]  index 从 0 开始；越界返回 1。
     * @param[out] out   由调用方提供并填好 struct_size；结构太短返回 2。
     * @return 0 = 已填入；1 = index 越界或 out 为空；2 = out 结构太短。
     */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const;

    /**
     * @brief 读一个配置项的当前值（值一律是字符串）。
     * @param[in]  key      配置键，不能为空。
     * @param[out] buf      接收缓冲区，可为 nullptr（此时只查长度）。
     * @param[in]  buf_size buf 的字节数；大于返回值时才写入（含结尾 NUL）。
     * @return >= 0 = 值的字节数（不含结尾 NUL）；< 0 = key 不认识或参数非法。
     */
    std::int32_t getValue(const char* key, char* buf, std::uint32_t bufSize) const;

    /**
     * @brief 写一个配置项；任意线程可调。
     *
     * 只保存字符串，不在这里校验热键语法：注册路径拿到 Value 后统一解析，
     * 非法值只 warn 跳过（见 hotkey_window_message.cpp），不会让宿主或本插件崩。
     *
     * @param[in] key   配置键，不能为空；不认识的键返回负值且不改变任何状态。
     * @param[in] value 字符串形式的新值；enabled 的 "0" / "false" 视为关、其余视为开。
     * @return 0 = 已生效；< 0 = key 不认识或参数非法。
     */
    std::int32_t setValue(const char* key, const char* value);

    /** @brief 全局热键是否启用（`enabled` 的当前值）。 */
    bool enabled() const noexcept;

    /** @brief 截屏热键的当前文本（`capture_hotkey`）；返回本地副本。 */
    std::string captureHotkey() const;

    /** @brief 区域截屏热键的当前文本（`region_hotkey`）；返回本地副本。 */
    std::string regionHotkey() const;

    /** @brief 打开设置热键的当前文本（`settings_hotkey`）；返回本地副本。 */
    std::string settingsHotkey() const;

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
     * @brief 把静态配置声明整段拷进宿主给的 ShotConfigDesc。
     * @param[in]  src 插件内的静态声明，不能为空。
     * @param[out] out 宿主提供的输出结构；进入时读 struct_size。
     * @return 0 = 已填入；1 = 参数为空；2 = out->struct_size 装不下声明。
     */
    static std::int32_t copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out);

    mutable std::mutex m_hotkeyMutex; /**< 保护三个热键字符串。 */
    std::string m_captureHotkey;      /**< capture_hotkey 的当前值。 */
    std::string m_regionHotkey;       /**< region_hotkey 的当前值。 */
    std::string m_settingsHotkey;     /**< settings_hotkey 的当前值。 */
    std::atomic<bool> m_enabled{true}; /**< enabled：关闭时热键线程撤掉全部注册。 */
};

}  // namespace hotkey
