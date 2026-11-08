#pragma once

// ============================================================================
//  include/shot_plugin_helpers/shot_plugin_config.h —— 插件侧配置目录封装 shot::ConfigDesc / shot::PluginConfig
//
//  包装 shot.host.plugin_config：枚举宿主与所有可配置插件的配置项、读写值；
//  ConfigDesc 把声明里的字符串拷成本地副本（提供者卸载后仍可安全使用）。
//  本头文件只声明 PluginConfig 与它的数据 struct ConfigDesc（同一关注点）；
//  插件通常只包含汇总头 shot_plugin_helpers.h。
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>

#include "plugin_api.h"

namespace shot {

/**
 * @brief 插件侧的一份配置项声明：字符串已拷成调用方自己的副本。
 *
 * 目录服务返回的 ShotConfigDesc 里，key / label / options / tooltip 都指向提供者
 * 插件内的字符串；插件一旦被热卸载就失效。设置页要把声明留在自己的模型里，
 * 所以这里一律拷成 std::string，生命周期与这份副本相同。
 */
struct ConfigDesc {
    uint32_t kind = 0;        /**< SHOT_CONFIG_KIND_* */
    std::string key;          /**< 配置键；存盘时拼成 <provider_id>/<key> */
    std::string label;        /**< 界面显示名 */
    std::string defaultValue; /**< 默认值（字符串形式），没有默认值时为空串 */
    std::string options;      /**< ENUM 的候选，形如 "luma|average"；其它类型为空串 */
    double min = 0.0;         /**< INT / FLOAT 下界；其它类型忽略 */
    double max = 0.0;         /**< INT / FLOAT 上界；其它类型忽略 */
    double step = 0.0;        /**< INT / FLOAT 步长；其它类型忽略 */
    std::string tooltip;      /**< 悬停提示，可为空串 */
};

/**
 * @brief 插件侧配置目录封装：向宿主 query_interface 取 ShotHostPluginConfigV1。
 *
 * 目录里有两种提供者：索引 0 是宿主自己（id 固定 "core"，宿主设置也在里面），
 * 其余是实现了 shot.configurable 的插件实例。设置页只面对这一张表。
 *
 * 读值/写值都是【同步】转调对应插件的 get_config / set_config——宿主用一把生命周期
 * 锁保证调用期间插件不会被卸载。写值只改内存，落盘与 `config.applied` 通知要再调 save()。
 *
 * 老宿主没有这个服务时 available() 为 false，所有读写安静失败。
 */
class PluginConfig {
public:
    /**
     * @brief 构造时向宿主索要配置目录服务。
     * @param[in] host 宿主传给 create()/init() 的服务表指针；nullptr 时 available() 为 false。
     */
    explicit PluginConfig(const ShotHostServices* host);

    /** @brief 宿主是否提供了可用的配置目录服务（老宿主可能没有）。 */
    bool available() const noexcept { return m_host != nullptr && m_service != nullptr; }

    /** @brief 目录里可配置提供者的个数（含索引 0 的宿主自己）。 */
    uint32_t configurableCount() const noexcept;

    /**
     * @brief 取第 index 个提供者的身份证。
     * @param[in]  index 0 .. configurableCount()-1。
     * @param[out] out   成功时填入 id / name / version；字符串只在下一次目录调用前有效。
     * @return true = 成功；false = 服务不可用、index 越界或提供者没给身份。
     */
    bool configurableInfo(uint32_t index, ShotPluginInfo& out) const;

    /**
     * @brief 按提供者 id 取配置项个数。
     * @param[in] providerId 提供者 id，如 "core" / "example.invert"。
     * @return 配置项个数；服务不可用或 id 不认识时返回 0。
     */
    uint32_t configCount(const char* providerId) const noexcept;

    /**
     * @brief 取一项配置声明，并把字符串拷进 out。
     * @param[in]  providerId 提供者 id。
     * @param[in]  item       0 .. configCount()-1。
     * @param[out] out        成功时填好的本地副本。
     * @return true = 成功；false = 服务不可用、参数非法或提供者拒绝。
     */
    bool configDesc(const char* providerId, uint32_t item, ConfigDesc& out) const;

    /**
     * @brief 读一个配置项的当前值。
     * @param[in] providerId 提供者 id。
     * @param[in] key        配置键。
     * @param[in] fallback   读取失败时的返回值。
     * @return 当前值；失败返回 fallback。
     */
    std::string getString(const char* providerId, const char* key,
                          std::string_view fallback = {}) const;

    /**
     * @brief 写一个配置项（同步转调插件；只改内存）。
     * @return true = 提供者接受；宿主不支持、参数非法或提供者拒绝时 false。
     */
    bool setString(const char* providerId, const char* key, std::string_view value) const;

    /**
     * @brief 把宿主持久化的已存值灌回所有提供者（宿主启动时会自动做一次）。
     * @return true = 成功灌入；服务不可用或失败时 false。
     */
    bool applyStored() const;

    /**
     * @brief 请宿主把配置落盘；成功后宿主发布 `config.applied`。
     * @return true = 已落盘。
     */
    bool save() const;

private:
    const ShotHostServices* m_host = nullptr;
    const ShotHostPluginConfigV1* m_service = nullptr;
};

inline PluginConfig::PluginConfig(const ShotHostServices* host) : m_host(host) {
    if (host == nullptr) {
        return;
    }
    if (!SHOT_HAS_FIELD(host, ShotHostServices, query_interface) ||
        host->query_interface == nullptr) {
        return;
    }
    void* iface = host->query_interface(host->host_context, SHOT_HOST_IFACE_PLUGIN_CONFIG, 1);
    if (iface == nullptr) {
        return;
    }
    const auto* service = static_cast<const ShotHostPluginConfigV1*>(iface);
    // 少任何一个必备操作都当"没有这个服务"：老宿主的结构可能只填了一部分。
    if (!SHOT_HAS_FIELD(service, ShotHostPluginConfigV1, save) ||
        service->configurable_count == nullptr || service->configurable_info == nullptr ||
        service->config_desc == nullptr || service->get_value == nullptr ||
        service->set_value == nullptr || service->apply_stored == nullptr ||
        service->save == nullptr) {
        return;
    }
    m_service = service;
}

inline uint32_t PluginConfig::configurableCount() const noexcept {
    if (!available()) {
        return 0;
    }
    return m_service->configurable_count(m_host->host_context);
}

inline bool PluginConfig::configurableInfo(uint32_t index, ShotPluginInfo& out) const {
    if (!available()) {
        return false;
    }
    out = ShotPluginInfo{};
    out.struct_size = sizeof(ShotPluginInfo);
    if (m_service->configurable_info(m_host->host_context, index, &out) != 0) {
        return false;
    }
    return out.id != nullptr;
}

inline uint32_t PluginConfig::configCount(const char* providerId) const noexcept {
    if (!available() || providerId == nullptr) {
        return 0;
    }
    return m_service->config_count(m_host->host_context, providerId);
}

inline bool PluginConfig::configDesc(const char* providerId, uint32_t item, ConfigDesc& out) const {
    if (!available() || providerId == nullptr) {
        return false;
    }
    ShotConfigDesc raw{};
    raw.struct_size = sizeof(ShotConfigDesc);
    if (m_service->config_desc(m_host->host_context, providerId, item, &raw) != 0) {
        return false;
    }
    if (raw.key == nullptr || raw.label == nullptr) {
        return false;
    }
    // 声明里的字符串归提供者所有，出函数前全部拷走（见 ConfigDesc 的注释）。
    out.kind = raw.kind;
    out.key = raw.key;
    out.label = raw.label;
    out.defaultValue = (raw.default_value != nullptr) ? raw.default_value : "";
    out.options = (raw.options != nullptr) ? raw.options : "";
    out.min = raw.min;
    out.max = raw.max;
    out.step = raw.step;
    out.tooltip = (raw.tooltip != nullptr) ? raw.tooltip : "";
    return true;
}

inline std::string PluginConfig::getString(const char* providerId, const char* key,
                                           std::string_view fallback) const {
    if (!available() || providerId == nullptr || key == nullptr) {
        return std::string(fallback);
    }
    const int32_t len = m_service->get_value(m_host->host_context, providerId, key, nullptr, 0);
    if (len < 0) {
        return std::string(fallback);
    }
    std::string buffer(static_cast<std::size_t>(len) + 1, '\0');
    const int32_t got = m_service->get_value(m_host->host_context, providerId, key,
                                             buffer.data(), static_cast<uint32_t>(buffer.size()));
    if (got < 0 || static_cast<std::size_t>(got) >= buffer.size()) {
        return std::string(fallback);
    }
    buffer.resize(static_cast<std::size_t>(got));
    return buffer;
}

inline bool PluginConfig::setString(const char* providerId, const char* key,
                                    std::string_view value) const {
    if (!available() || providerId == nullptr || key == nullptr) {
        return false;
    }
    const std::string text(value);
    return m_service->set_value(m_host->host_context, providerId, key, text.c_str()) == 0;
}

inline bool PluginConfig::applyStored() const {
    if (!available()) {
        return false;
    }
    return m_service->apply_stored(m_host->host_context) >= 0;
}

inline bool PluginConfig::save() const {
    if (!available()) {
        return false;
    }
    return m_service->save(m_host->host_context) == 0;
}

}  // namespace shot

