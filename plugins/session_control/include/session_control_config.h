#pragma once

// ============================================================================
//  plugins/session_control/include/session_control_config.h —— 一个配置项
//
//  · enabled  BOOL  是否在会话总线上注册 org.pluginshot.Control（默认 "1" = 开）。
//              关掉时插件照常加载、只是不占用 D-Bus 名称；改回开时重新注册。
//
//  label / tooltip 写的是语言表 key（config.<key>.label / config.<key>.tooltip），
//  由设置页按 display("session_control.main", key) 解析；文案本体在
//  session_control_plugin.cpp 的 kLangTables 里。
//
//  线程安全：set_config / get_config 可能在宿主任意线程被调用（启动回灌、设置页
//  应用），所以当前值用 std::atomic<bool> 保存；注册/注销动作由插件主体串行化。
// ============================================================================

#include <atomic>
#include <cstdint>

#include "plugin_api.h"

namespace session_control {

/**
 * @brief 会话控制插件的配置项：是否启用 D-Bus 入口。
 *
 * 配置只负责“声明 + 存布尔值”：D-Bus 注册/注销由 ControlService 完成，什么时候
 * 做由 SessionControlPlugin 决定。这样这个类不需要知道任何宿主服务（STYLE §6.6）。
 */
class SessionControlConfig {
public:
    /** @brief 构造：默认开启（与 metadata / 描述符里的 "1" 保持一致）。 */
    SessionControlConfig();

    /** @brief 不可拷贝：值是跨线程共享状态，复制没有意义。 */
    SessionControlConfig(const SessionControlConfig&) = delete;
    /** @brief 不可赋值。 */
    SessionControlConfig& operator=(const SessionControlConfig&) = delete;

    /**
     * @brief 配置项个数（供 shot.configurable 的 get_config_count 转发）。
     * @return 固定为 1。
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
     * @brief 读 enabled 的当前值（值一律是字符串）。
     * @param[in]  key      配置键，不能为空。
     * @param[out] buf      接收缓冲区，可为 nullptr（此时只查长度）。
     * @param[in]  buf_size buf 的字节数；大于返回值时才写入（含结尾 NUL）。
     * @return >= 0 = 值的字节数（不含结尾 NUL）；< 0 = key 不认识或参数非法。
     */
    std::int32_t getValue(const char* key, char* buf, std::uint32_t buf_size) const;

    /**
     * @brief 写 enabled；任意线程可调。
     *
     * 只保存布尔值，不注册也不注销 D-Bus——那是插件主体的职责，因为
     * g_bus_own_name 需要一个真正被迭代的 GMainContext。
     *
     * @param[in] key   配置键，不能为空；不认识的键返回负值且不改变任何状态。
     * @param[in] value 字符串形式的新值："0" / "false" 视为关，其余视为开。
     * @return 0 = 已生效；< 0 = key 不认识或参数非法。
     */
    std::int32_t setValue(const char* key, const char* value);

    /** @brief 是否在会话总线上注册 D-Bus 名称（`enabled` 的当前值）。 */
    bool enabled() const noexcept;

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

    std::atomic<bool> m_enabled{true}; /**< enabled：关掉时注销 D-Bus 名称。 */
};

}  // namespace session_control
