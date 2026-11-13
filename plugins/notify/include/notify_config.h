#pragma once

// ============================================================================
//  include/notify_config.h —— 消息提示插件的配置项（shot.configurable）
//
//  · enabled         BOOL：消息提示总开关（默认开）。
//  · show_on_success BOOL：截屏成功时提示"已保存到 <path>"（默认开）。
//  · show_on_error   BOOL：截屏失败时提示原因（默认开）。
//  · sound           BOOL：气泡是否播放系统提示音（默认开）。
//  · title           STRING：通知标题模板（默认空串 = 用默认多语言标题）。
//  · click_action    ENUM：点击成功通知时打开图片 / 打开所在文件夹（默认打开图片）。
//
//  配置值必须能在任意线程被读写：宿主 init 的 apply_stored、设置页"应用"都会调
//  set_config，而通知后端在显示气泡前读当前值。四个布尔与枚举用 std::atomic 保存；
//  标题是字符串，用互斥量保护，读取侧拿到的是一份完整拷贝。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "plugin_api.h"

namespace notify {

/**
 * @brief 点击成功通知时的行为（配置项 `click_action` 的取值）。
 *
 * 内部用枚举而不是字符串：通知后端在点击回调里要做一个二选一分支，比较字符串
 * 既慢又容易拼错。跨边界（配置值、config_list）仍然是字符串键（带命名空间，
 * 同时是 i18n key），见 clickActionKey()。
 */
enum class ClickAction : std::uint8_t {
    OpenImage = 0,   /**< 用系统默认程序打开图片（默认值）。 */
    OpenFolder = 1,  /**< 在文件管理器里定位图片（Windows 资源管理器 /select）。 */
};

/**
 * @brief 消息提示插件的配置项：`enabled` / `show_on_success` / `show_on_error` /
 *        `sound` / `title` / `click_action`。
 *
 * 为什么单独成一个类：配置项的声明（ShotConfigDesc 表）、字符串值解析与"当前值"
 * 是一件独立职责，和窗口/线程/气泡（NotifyToast）混在一起会让头文件不再是单一
 * 契约（STYLE.md §6.6 / §6.8）。ABI 回调在 plugin_entry.cpp 里只做参数转发。
 *
 * 线程安全：get/set 可在任意线程调用；布尔与枚举用 std::atomic 保存，标题用
 * 互斥量保护（读写都在锁内，读返回拷贝）。
 */
class NotifyConfig {
public:
    /** @brief 构造：四个开关默认开、标题为空（= 用默认多语言标题）、点击打开图片。 */
    NotifyConfig();

    /** @brief 不可拷贝：值是跨线程共享的状态，复制没有意义。 */
    NotifyConfig(const NotifyConfig&) = delete;
    /** @brief 不可赋值。 */
    NotifyConfig& operator=(const NotifyConfig&) = delete;

    /**
     * @brief 配置项个数（供 shot.configurable 的 get_config_count 转发）。
     * @return 固定为 6。
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
    std::int32_t getValue(const char* key, char* buf, std::uint32_t buf_size) const;

    /**
     * @brief 写一个配置项；任意线程可调。
     * @param[in] key   配置键，不能为空；不认识的键返回负值且不改变任何状态。
     * @param[in] value 字符串形式的新值：BOOL 的 "0" / "false" 视为关，其余视为开；
     *                  ENUM 同时接受新键与旧值，非法取值返回 -1 并保留原值。
     * @return 0 = 已生效；< 0 = key 不认识、参数非法或取值非法。
     */
    std::int32_t setValue(const char* key, const char* value);

    /** @brief 消息提示总开关（`enabled`）：关掉后成功/失败都不弹。 */
    bool enabled() const noexcept;

    /** @brief 截屏成功时是否提示（`show_on_success`）。 */
    bool showOnSuccess() const noexcept;

    /** @brief 截屏失败时是否提示（`show_on_error`）。 */
    bool showOnError() const noexcept;

    /** @brief 气泡是否播放系统提示音（`sound`）：false 时用 NIIF_NOSOUND 静音。 */
    bool soundEnabled() const noexcept;

    /**
     * @brief 通知标题模板（`title`）。
     * @return 用户配置的标题模板（UTF-8）；空串表示"用默认多语言标题"。
     * @note 返回拷贝：值可能被设置页线程同时改写，不能返回内部引用。
     */
    std::string title() const;

    /** @brief 点击成功通知时的行为（`click_action`）；默认打开图片。 */
    ClickAction clickAction() const noexcept;

    /**
     * @brief 把点击行为映射成跨边界的配置值（同时是 i18n key）。
     * @param[in] action 点击行为。
     * @return "click.open_image" 或 "click.open_folder"；永远不为 nullptr。
     * @note 通知后端点击回调里要按当前动作二选一，平台代码也会用它取动作文案。
     */
    static const char* clickActionKey(ClickAction action) noexcept;

private:
    /**
     * @brief 按契约把字符串值写进宿主缓冲区并报告值的长度。
     * @param[in]  value   要返回的字符串；不能为 nullptr。
     * @param[out] buf     宿主缓冲区，可为 nullptr（只查询长度）。
     * @param[in]  buf_size buf 的字节数。
     * @return >= 0 = 值的字节数（不含结尾 NUL）；-1 = value 为空。
     */
    static std::int32_t copyValue(const char* value, char* buf, std::uint32_t buf_size);

    /**
     * @brief 把静态配置声明整段拷进宿主给的 ShotConfigDesc。
     * @param[in]  src 插件内的静态声明，不能为空。
     * @param[out] out 宿主提供的输出结构；进入时读 struct_size。
     * @return 0 = 已填入；1 = 参数为空；2 = out->struct_size 装不下声明。
     */
    static std::int32_t copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out);

    std::atomic<bool> m_enabled{true};        /**< enabled：消息提示总开关。 */
    std::atomic<bool> m_showOnSuccess{true};  /**< show_on_success：成功提示。 */
    std::atomic<bool> m_showOnError{true};    /**< show_on_error：失败提示。 */
    std::atomic<bool> m_sound{true};          /**< sound：是否播放提示音。 */

    // 点击行为用 int 存（std::atomic<enum> 合法但没优势）：读侧只做一次 load。
    std::atomic<int> m_clickAction{static_cast<int>(ClickAction::OpenImage)};

    mutable std::mutex m_titleMutex; /**< 保护 m_title（读侧拿拷贝，写侧来自任意线程）。 */
    std::string m_title;             /**< title：标题模板；空串 = 默认标题。 */
};

}  // namespace notify
