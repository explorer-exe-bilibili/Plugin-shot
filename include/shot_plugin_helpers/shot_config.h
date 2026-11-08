#pragma once

// ============================================================================
//  include/shot_plugin_helpers/shot_config.h —— 插件侧宿主配置封装 shot::Config
//
//  包装 shot.host.config 服务：读 key 返回 fallback、写入失败返回 false；
//  这是宿主的全局配置，不是插件自己实现的 shot.configurable。
//  本头文件只声明一个类；插件通常只包含汇总头 shot_plugin_helpers.h。
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "plugin_api.h"

namespace shot {

/**
 * @brief 插件侧宿主配置封装：向宿主 query_interface 取 ShotHostConfigV1 读写配置。
 *
 * 这是【宿主】的配置服务（读写宿主/全局配置），不是插件自己实现的 shot.configurable。
 * 宿主没提供配置服务、或 key 不存在时：get* 返回调用方给的 fallback，set* 返回 false。
 * 所有接口都不抛异常（失败用返回值表达）。
 *
 * 用法（三行）：
 *   shot::Config config(host);
 *   const int strength = config.getInt("strength", 100);
 *   config.setBool("dark_mode", true);
 */
class Config {
public:
    /**
     * @brief 构造时向宿主索要配置服务。
     * @param[in] host 宿主传给 create()/init() 的服务表指针；nullptr 时 available() 为 false。
     */
    explicit Config(const ShotHostServices* host);

    /** @brief 宿主是否提供了可用的配置服务（拿到表且能读）。 */
    bool available() const noexcept {
        return m_host != nullptr && m_config != nullptr && m_config->get_config != nullptr;
    }

    /**
     * @brief 读一个配置项，按字符串返回。
     * @param[in] key      配置键，如 "dark_mode"；为 nullptr 时直接返回 fallback。
     * @param[in] fallback key 不存在、宿主不支持或读取失败时的返回值。
     * @return 配置值；失败时返回 fallback。
     * @note 内部先查长度、再按长度取一次；两次调用之间值变化也不会越界。
     */
    std::string getString(const char* key, std::string_view fallback = {}) const;

    /**
     * @brief 读一个整数配置项（十进制解析，整串都必须是数字）。
     * @param[in] key      配置键。
     * @param[in] fallback 不存在或不是合法整数时的返回值。
     * @return 解析结果，失败返回 fallback。
     */
    int getInt(const char* key, int fallback = 0) const;

    /**
     * @brief 读一个浮点配置项（整串都必须是数字）。
     * @param[in] key      配置键。
     * @param[in] fallback 不存在或不是合法浮点数时的返回值。
     * @return 解析结果，失败返回 fallback。
     */
    double getDouble(const char* key, double fallback = 0.0) const;

    /**
     * @brief 读一个布尔配置项；接受 "1"/"0"/"true"/"false"（大小写不敏感）。
     * @param[in] key      配置键。
     * @param[in] fallback 不存在或不是合法布尔值时的返回值。
     * @return 解析结果，失败返回 fallback。
     */
    bool getBool(const char* key, bool fallback = false) const;

    /**
     * @brief 写一个字符串配置项；是否立即落盘由宿主决定，需要落盘再调 save()。
     * @param[in] key   配置键，不能为 nullptr。
     * @param[in] value 新值，不能含 NUL（C 字符串语义）。
     * @return true = 已生效；宿主没提供写接口或写入被拒绝时 false。
     */
    bool setString(const char* key, std::string_view value) const;

    /** @brief 写一个整数配置项（转成十进制字符串），见 setString。 */
    bool setInt(const char* key, int value) const;

    /** @brief 写一个浮点配置项（%.17g），见 setString。 */
    bool setDouble(const char* key, double value) const;

    /** @brief 写一个布尔配置项（写成 "true"/"false"），见 setString。 */
    bool setBool(const char* key, bool value) const;

    /**
     * @brief 请求宿主把配置落盘（宿主必须同步完成）。
     * @return true = 已保存；宿主没提供 save_config 时 false。
     */
    bool save() const;

private:
    const ShotHostServices* m_host = nullptr;
    const ShotHostConfigV1* m_config = nullptr;
};

// 实现放在类定义之后，让上面的接口一眼能读完。

inline Config::Config(const ShotHostServices* host) : m_host(host) {
    if (host == nullptr) {
        return;
    }
    if (!SHOT_HAS_FIELD(host, ShotHostServices, query_interface) ||
        host->query_interface == nullptr) {
        return;
    }
    void* iface = host->query_interface(host->host_context, SHOT_HOST_IFACE_CONFIG, 1);
    if (iface == nullptr) {
        return;
    }
    const auto* config = static_cast<const ShotHostConfigV1*>(iface);
    if (!SHOT_HAS_FIELD(config, ShotHostConfigV1, get_config) || config->get_config == nullptr) {
        return;
    }
    m_config = config;
}

inline std::string Config::getString(const char* key, std::string_view fallback) const {
    if (!available() || key == nullptr) {
        return std::string(fallback);
    }
    const int32_t len = m_config->get_config(m_host->host_context, key, nullptr, 0);
    if (len < 0) {
        return std::string(fallback);
    }
    std::string buffer(static_cast<std::size_t>(len) + 1, '\0');
    const int32_t got = m_config->get_config(
        m_host->host_context, key, buffer.data(), static_cast<uint32_t>(buffer.size()));
    if (got < 0 || static_cast<std::size_t>(got) >= buffer.size()) {
        return std::string(fallback);
    }
    buffer.resize(static_cast<std::size_t>(got));
    return buffer;
}

inline int Config::getInt(const char* key, int fallback) const {
    const std::string text = getString(key, {});
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || end == nullptr || *end != '\0') {
        return fallback;
    }
    return static_cast<int>(value);
}

inline double Config::getDouble(const char* key, double fallback) const {
    const std::string text = getString(key, {});
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || end == nullptr || *end != '\0') {
        return fallback;
    }
    return value;
}

inline bool Config::getBool(const char* key, bool fallback) const {
    const std::string text = getString(key, {});
    if (text == "1" || text == "true" || text == "TRUE") {
        return true;
    }
    if (text == "0" || text == "false" || text == "FALSE") {
        return false;
    }
    return fallback;
}

inline bool Config::setString(const char* key, std::string_view value) const {
    if (!available() || key == nullptr) {
        return false;
    }
    if (!SHOT_HAS_FIELD(m_config, ShotHostConfigV1, set_config) ||
        m_config->set_config == nullptr) {
        return false;
    }
    const std::string text(value);
    return m_config->set_config(m_host->host_context, key, text.c_str()) == 0;
}

inline bool Config::setInt(const char* key, int value) const {
    return setString(key, std::to_string(value));
}

inline bool Config::setDouble(const char* key, double value) const {
    char buffer[32];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
        return false;
    }
    return setString(key, std::string_view(buffer, static_cast<std::size_t>(written)));
}

inline bool Config::setBool(const char* key, bool value) const {
    return setString(key, value ? "true" : "false");
}

inline bool Config::save() const {
    if (!available()) {
        return false;
    }
    if (!SHOT_HAS_FIELD(m_config, ShotHostConfigV1, save_config) ||
        m_config->save_config == nullptr) {
        return false;
    }
    m_config->save_config(m_host->host_context);
    return true;
}

}  // namespace shot

