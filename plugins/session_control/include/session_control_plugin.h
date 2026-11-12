#pragma once

// ============================================================================
//  plugins/session_control/include/session_control_plugin.h —— 插件主体
//
//  生命周期（Generic 插件没有 ui_provider 的 init/deinit）：
//    create()          → start(host)：发布自身文案、按默认值启动 D-Bus 服务
//    set_config(...)   → 存值，并按 enabled 的变化注册/注销 D-Bus 名称
//    destroy()         → stop()：注销名称、退订 i18n、join 服务线程
//
//  为什么 set_config 里直接注册/注销：这是本插件唯一的配置项，值一变就应该立刻
//  生效（不需要重启宿主）；g_bus_own_name / g_bus_unown_name 仍然在 ControlService
//  的专用线程上完成，set_config 只是触发者。宿主在启动时用 apply_stored 回灌
//  `enabled`，所以"存盘为关"会在 create 的短暂注册之后立刻注销。
// ============================================================================

#include <cstdint>
#include <memory>
#include <mutex>

#include "control_service.h"
#include "plugin_api.h"
#include "session_control_config.h"

namespace shot {
class I18n;  // 多语言封装：定义在 shot_plugin_helpers/shot_i18n.h，只在本类 .cpp 里用完整类型
}  // namespace shot

namespace session_control {

/**
 * @brief 会话控制插件的主体：配置回调 + D-Bus 服务的启停编排。
 *
 * 具体职责按 STYLE §6.6 拆开：配置声明在 SessionControlConfig，D-Bus 线程与
 * 注册/注销在 ControlService，这里只回答"什么时候开、什么时候关"。
 *
 * @note 能力函数表里的 self 是 PluginInstance*，不是本类；回调里先 state_of(self)
 *       再 static_cast 回 SessionControlPlugin。
 */
class SessionControlPlugin {
public:
    /** @brief 构造：默认配置（enabled = true），不碰任何宿主服务。 */
    SessionControlPlugin();

    /** @brief 析构：兜底调用 stop()，保证 D-Bus 线程已 join、DLL 可以卸载。 */
    ~SessionControlPlugin();

    /** @brief 不可拷贝：内部持有线程、互斥量与订阅句柄。 */
    SessionControlPlugin(const SessionControlPlugin&) = delete;
    /** @brief 不可赋值。 */
    SessionControlPlugin& operator=(const SessionControlPlugin&) = delete;

    /**
     * @brief 挂上宿主服务：发布配置项文案，并按当前 enabled 启停 D-Bus 服务。
     *
     * 本插件没有 ui_provider 的 init/deinit，create(index, host) 是唯一能拿到宿主
     * 服务表的时机，所以由 plugin_entry.cpp 的 create_instance 在状态建好后调用。
     *
     * @param[in] host 宿主服务表；nullptr 时安静降级（不注册 D-Bus）。
     * @return true = 已尽力初始化（D-Bus 名称是否真的拿到看日志）。
     */
    bool start(const ShotHostServices* host);

    /** @brief 注销 D-Bus 名称、退订事件、释放文案表；重复调用是空操作。 */
    void stop();

    /**
     * @brief 配置项个数，转调 SessionControlConfig。
     * @return 配置项个数。
     */
    std::uint32_t configCount() const noexcept;

    /**
     * @brief 声明第 index 个配置项，转调 SessionControlConfig。
     * @param[in]  index 配置项下标。
     * @param[out] out   宿主提供并填好 struct_size 的输出结构。
     * @return 契约返回码：0 成功 / 1 参数非法 / 2 out 结构太短。
     */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const noexcept;

    /**
     * @brief 读一个配置项，转调 SessionControlConfig。
     * @return >= 0 = 值长度（不含 NUL）；负值 = key 不认识或参数非法。
     */
    std::int32_t getConfig(const char* key, char* buf, std::uint32_t bufSize) const;

    /**
     * @brief 写一个配置项；enabled 变化时立刻注册/注销 D-Bus 名称。
     *
     * 任意线程可调：注册/注销动作由 applyEnabled 用互斥量串行化；ControlService 的
     * 启动/停止本身是阻塞的（要 join 线程），但只在开关真正翻转时发生。
     *
     * @return 0 = 已生效；负值 = key 不认识或值非法。
     */
    std::int32_t setConfig(const char* key, const char* value);

private:
    /**
     * @brief 按 enabled 的当前值启动或停止 D-Bus 服务；状态没变时是空操作。
     * @param[in] enabled true = 需要注册名称；false = 需要注销名称。
     */
    void applyEnabled(bool enabled);

    const ShotHostServices* m_host = nullptr; /**< 宿主服务表；stop() 后置空。 */
    SessionControlConfig m_config;            /**< `enabled` 配置项。 */
    ControlService m_service;                 /**< D-Bus 服务线程与注册句柄。 */
    std::unique_ptr<shot::I18n> m_i18n;       /**< 本插件配置项文案；start() 构造，stop() 释放 */
    std::mutex m_applyMutex;                  /**< 串行化 enabled 翻转（防并发 start/stop）。 */
};

}  // namespace session_control
