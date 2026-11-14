#pragma once

// ============================================================================
//  include/generic_plugin.h —— Generic 模板的插件状态
//
//  这个类演示"纯订阅者 + 配置项 + 托盘命令"三件套，没有具名图像/采集能力：
//    · shot.configurable：两个配置项（STRING log_prefix、BOOL verbose）；
//    · capture.done 订阅：拿到保存插件写回的路径，按配置记 INFO / DEBUG 日志；
//    · 托盘命令：通过 lists.add 贡献一条 capture.request 命令，宿主托盘负责
//      渲染与点击，插件不碰任何 UI 代码。
//
//  线程模型：事件回调与 set_config 都可能被宿主在不同线程调用（set_config 的
//  契约允许任意线程），所以配置值用互斥量保护；回调里只做日志，不做文件 IO。
// ============================================================================

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "plugin_api.h"
#include "shot_plugin_helpers/shot_event_bus.h"
#include "shot_plugin_helpers/shot_i18n.h"
#include "shot_plugin_helpers/shot_logger.h"

namespace generic_template {

/**
 * @brief Generic 模板插件实例：两个配置项 + 一条 capture.done 订阅 + 一条托盘命令。
 */
class GenericPlugin {
public:
    GenericPlugin() = default;
    ~GenericPlugin();

    GenericPlugin(const GenericPlugin&) = delete;
    GenericPlugin& operator=(const GenericPlugin&) = delete;

    /**
     * @brief 绑定宿主、发布语言表、订阅 capture.done、贡献托盘命令。
     * @param[in] host 宿主服务表；create() 路径传入，可为 nullptr（老宿主）。
     * @return true = 订阅成功；false = 老宿主没有事件总线，插件降级为只有配置项。
     */
    bool start(const ShotHostServices* host);

    /** @brief 退订事件总线、销毁语言表；重复调用是空操作。 */
    void stop();

    /** @brief 配置项个数（shot.configurable::get_config_count）。 */
    std::uint32_t configCount() const noexcept;

    /** @brief 声明第 index 个配置项（shot.configurable::get_config_desc）。 */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const;

    /** @brief 读配置值（shot.configurable::get_config）。 */
    std::int32_t getConfig(const char* key, char* buf, std::uint32_t bufSize) const;

    /** @brief 写配置值（shot.configurable::set_config）；不认识的 key 返回负数。 */
    std::int32_t setConfig(const char* key, const char* value);

private:
    /**
     * @brief capture.done 的宿主回调：只把负载拷成本地字符串，然后解析 + 记日志。
     * @param[in] user  this。
     * @param[in] topic 主题（固定 "capture.done"）。
     * @param[in] data  负载字节；回调返回后失效。
     * @param[in] len   负载字节数。
     */
    static void onCaptureDone(void* user, const char* topic, const void* data, std::uint32_t len);

    /** @brief 解析 capture.done 的 JSON 负载并按配置记日志。 */
    void handleCaptureDone(const void* data, std::uint32_t len);

    /** @brief 发布 lists.add，向托盘贡献一条「截屏」命令。 */
    void publishTrayCommand();

    const ShotHostServices* m_host = nullptr;           /**< 宿主服务表，生命周期覆盖本实例。 */
    std::unique_ptr<shot::I18n> m_i18n;                 /**< 本插件的语言表。 */
    bool m_subscribed = false;                          /**< 是否已成功订阅 capture.done。 */
    bool m_trayPublished = false;                       /**< 本次实例是否已发布托盘命令。 */

    mutable std::mutex m_configMutex;                   /**< 保护下面两个配置值。 */
    std::string m_logPrefix;                            /**< STRING 配置项 log_prefix 的当前值。 */
    bool m_verbose = true;                              /**< BOOL 配置项 verbose 的当前值。 */
};

}  // namespace generic_template
