#pragma once

// ============================================================================
//  include/shot_plugin_helpers/shot_logger.h —— 插件侧日志封装 shot::Logger
//
//  包装 ShotHostServices::log：判空、按等级写日志、对老宿主安静降级。
//  本头文件只声明一个类；插件通常只包含汇总头 shot_plugin_helpers.h。
// ============================================================================

#include <cstdint>
#include <string_view>

#include "plugin_api.h"

namespace shot {

/**
 * @brief 插件侧日志封装：包装 ShotHostServices::log，缺服务时静默降级。
 *
 * 老宿主可能没有 log 字段，调用方不需要再写 SHOT_HAS_FIELD 三重判空。
 * 用法（两行）：
 *   shot::Logger log(host);
 *   log.info("plugin ready");
 */
class Logger {
public:
    /**
     * @brief 绑定宿主服务表。
     * @param[in] host 宿主传给 create()/init() 的服务表指针；可以为 nullptr（= 没有服务，调用全部静默丢弃）。
     */
    explicit Logger(const ShotHostServices* host) noexcept : m_host(host) {}

    /** @brief 宿主是否提供了可用的 log 服务（老宿主可能没有）。 */
    bool available() const noexcept {
        return m_host != nullptr &&
               SHOT_HAS_FIELD(m_host, ShotHostServices, log) &&
               m_host->log != nullptr;
    }

    /**
     * @brief 按 SHOT_LOG_* 等级写一条日志（需要自定义等级时用）。
     * @param[in] level SHOT_LOG_DEBUG / SHOT_LOG_INFO / SHOT_LOG_WARN / SHOT_LOG_ERROR。
     * @param[in] msg   日志正文；空串允许，不会自动加换行。
     */
    void log(int32_t level, std::string_view msg) const noexcept {
        if (!available() || msg.data() == nullptr) {
            return;
        }
        m_host->log(m_host->host_context, level, msg.data(), static_cast<int32_t>(msg.size()));
    }

    /** @brief 调试日志（SHOT_LOG_DEBUG）。 */
    void debug(std::string_view msg) const noexcept { log(SHOT_LOG_DEBUG, msg); }
    /** @brief 普通日志（SHOT_LOG_INFO）。 */
    void info(std::string_view msg) const noexcept { log(SHOT_LOG_INFO, msg); }
    /** @brief 警告（SHOT_LOG_WARN）。 */
    void warn(std::string_view msg) const noexcept { log(SHOT_LOG_WARN, msg); }
    /** @brief 错误（SHOT_LOG_ERROR）。 */
    void error(std::string_view msg) const noexcept { log(SHOT_LOG_ERROR, msg); }

private:
    const ShotHostServices* m_host = nullptr;
};

}  // namespace shot

