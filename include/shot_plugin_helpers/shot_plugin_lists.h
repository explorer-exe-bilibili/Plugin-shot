#pragma once

// ============================================================================
//  include/shot_plugin_helpers/shot_plugin_lists.h —— 插件侧列表服务封装 shot::PluginLists
//
//  包装 shot.host.plugin_lists：按 list id 取宿主聚合出来的 JSON 快照。
//  列表的注册/写值/提交都是事件（lists.add / lists.update / lists.commit），
//  不在本封装里；这里只解决"读"。
//
//  本头文件只声明一个类；插件通常只包含汇总头 shot_plugin_helpers.h。
// ============================================================================

#include <cstdint>
#include <string>

#include "plugin_api.h"

namespace shot {

/**
 * @brief 插件侧列表服务封装：向宿主 query_interface 取 ShotHostPluginListsV1。
 *
 * 老宿主没有这个服务时 available() 为 false，所有读取安静失败（返回空串/0）。
 * 读取是两段式：先查长度，再分配缓冲拷回来；返回的字符串是调用方自己的副本，
 * 列表之后怎么变都不影响它。
 *
 * 用法（以设置页为例）：
 *   shot::PluginLists lists(host);
 *   const std::string text = lists.json("config_list");   // JSON 文本
 *   nlohmann::json items = nlohmann::json::parse(text, nullptr, false);
 *
 * @note 列表内容变化时宿主会发布 `lists.changed`（负载是 list id）；不要在渲染
 *       循环里每帧调用 json()，收到事件后再刷新本地模型。
 */
class PluginLists {
public:
    /**
     * @brief 构造时向宿主索要列表服务。
     * @param[in] host 宿主传给 create()/init() 的服务表指针；nullptr 时 available() 为 false。
     */
    explicit PluginLists(const ShotHostServices* host);

    /** @brief 宿主是否提供了可用的列表服务（老宿主可能没有）。 */
    bool available() const noexcept { return m_host != nullptr && m_service != nullptr; }

    /**
     * @brief 当前已注册的列表个数。
     * @return 列表个数；服务不可用时 0。
     */
    uint32_t listCount() const noexcept;

    /**
     * @brief 取第 index 个列表的 id。
     * @param[in] index 0 .. listCount()-1。
     * @return list id；越界或服务不可用时为空串。
     */
    std::string listId(uint32_t index) const;

    /**
     * @brief 取某个列表的 JSON 快照。
     * @param[in] listId 列表 id，如 "config_list"；不能为 nullptr。
     * @return UTF-8 JSON 文本；服务不可用、id 不认识或读取失败时返回空串。
     * @note 返回的是本地副本，可以长期持有；下次需要最新内容时重新调用。
     */
    std::string json(const char* listId) const;

private:
    const ShotHostServices* m_host = nullptr;
    const ShotHostPluginListsV1* m_service = nullptr;
};

inline PluginLists::PluginLists(const ShotHostServices* host) : m_host(host) {
    if (host == nullptr) {
        return;
    }
    if (!SHOT_HAS_FIELD(host, ShotHostServices, query_interface) ||
        host->query_interface == nullptr) {
        return;
    }
    void* service = host->query_interface(host->host_context, SHOT_HOST_IFACE_PLUGIN_LISTS, 1);
    if (service == nullptr) {
        return;
    }
    const auto* table = static_cast<const ShotHostPluginListsV1*>(service);
    if (!SHOT_HAS_FIELD(table, ShotHostPluginListsV1, list_json) || table->list_json == nullptr) {
        return;
    }
    m_service = table;
}

inline uint32_t PluginLists::listCount() const noexcept {
    if (!available()) {
        return 0;
    }
    return m_service->list_count(m_host->host_context);
}

inline std::string PluginLists::listId(uint32_t index) const {
    if (!available()) {
        return {};
    }
    // 第一段：只查长度（buf = NULL）。
    const int32_t length = m_service->list_id(m_host->host_context, index, nullptr, 0);
    if (length < 0) {
        return {};
    }
    std::string text(static_cast<std::size_t>(length), '\0');
    if (length == 0) {
        return text;
    }
    // 第二段：拷进自备缓冲（多留一个字节给 NUL）。
    text.resize(static_cast<std::size_t>(length) + 1);
    const int32_t got =
        m_service->list_id(m_host->host_context, index, text.data(), static_cast<uint32_t>(text.size()));
    if (got < 0) {
        return {};
    }
    text.resize(static_cast<std::size_t>(got));
    return text;
}

inline std::string PluginLists::json(const char* listId) const {
    if (!available() || listId == nullptr) {
        return {};
    }
    const int32_t length = m_service->list_json(m_host->host_context, listId, nullptr, 0);
    if (length < 0) {
        return {};
    }
    std::string text(static_cast<std::size_t>(length), '\0');
    if (length == 0) {
        return text;
    }
    text.resize(static_cast<std::size_t>(length) + 1);
    const int32_t got = m_service->list_json(m_host->host_context, listId, text.data(),
                                             static_cast<uint32_t>(text.size()));
    if (got < 0) {
        return {};
    }
    text.resize(static_cast<std::size_t>(got));
    return text;
}

}  // namespace shot
