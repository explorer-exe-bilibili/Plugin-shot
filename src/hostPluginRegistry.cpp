#include "hostPluginRegistry.h"

#include <atomic>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "hostServices.h"

namespace {

// 进程内唯一的插件注册表；服务表函数没有 ctx 可用，靠它回指宿主实例。
static std::atomic<Host::HostPluginRegistry*> activeRegistry{nullptr};

// 插件启停处理完后发布的主题（负载格式见 document/SETTINGS.md §3.2）。
const char kPluginsChangedTopic[] = "plugins.changed";

}  // namespace

namespace Host {

HostPluginRegistry::HostPluginRegistry() {
    activeRegistry.store(this);
}

HostPluginRegistry::~HostPluginRegistry() {
    HostPluginRegistry* expected = this;
    activeRegistry.compare_exchange_strong(expected, nullptr);
}

void HostPluginRegistry::bind(Plugin::PluginManager* manager) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_manager = manager;
}

void HostPluginRegistry::unbind() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_manager = nullptr;
    m_current.reset();
    m_previous.reset();
}

const ShotHostPluginRegistryV1* HostPluginRegistry::service() {
    // 回调是私有成员，表只能定义在类内部作用域里（仍是本 TU 的静态常量数据）。
    static const ShotHostPluginRegistryV1 kService = {
        .struct_size     = sizeof(ShotHostPluginRegistryV1),
        .version         = 1,
        .installed_count = &HostPluginRegistry::installedCount,
        .installed_info  = &HostPluginRegistry::installedInfo,
        .is_enabled      = &HostPluginRegistry::isEnabled,
        .set_enabled     = &HostPluginRegistry::setEnabled,
    };
    return activeRegistry.load() != nullptr ? &kService : nullptr;
}

void HostPluginRegistry::refresh() {
    Plugin::PluginManager* manager = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        manager = m_manager;
    }
    if (manager == nullptr) {
        return;
    }
    auto snapshot = std::make_shared<const Snapshot>(manager->getInstalledPlugins());
    std::lock_guard<std::mutex> lock(m_mutex);
    // 保留上一份：刚返回的字符串指针要活到调用方下一次注册表调用。
    m_previous = std::move(m_current);
    m_current = std::move(snapshot);
}

bool HostPluginRegistry::hasPending() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_pending.empty();
}

bool HostPluginRegistry::applyPending(Plugin::PluginManager& manager) {
    std::map<std::string, bool> pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pending.swap(m_pending);
    }
    if (pending.empty()) {
        return false;
    }

    // 操作前快照：启用会做依赖级联，被级联的 provider 也会改状态；只有对比前后
    // 状态才能一条不漏地通知设置页（级联启用是"一次点击、多条变化"）。
    const std::vector<Plugin::InstalledPlugin> before = manager.getInstalledPlugins();

    std::map<std::string, std::string> outcomes;  // 被请求的 id -> 拒绝/失败原因
    for (const auto& [pluginId, enabled] : pending) {
        std::string reason;
        manager.setPluginEnabled(pluginId, enabled, reason);
        if (!reason.empty()) {
            outcomes[pluginId] = reason;
        }
    }
    refresh();

    std::map<std::string, Plugin::InstalledPlugin> previous;
    for (const Plugin::InstalledPlugin& record : before) {
        previous[record.id] = record;
    }
    std::map<std::string, Plugin::InstalledPlugin> current;
    // 本轮是否真有插件被（含级联）启用：对比前后 loaded 状态，而不是只看被请求
    // 的 id —— 级联启用的 provider 不在 pending 里，同样需要回灌已存配置。
    bool enabledAny = false;
    for (const Plugin::InstalledPlugin& record : manager.getInstalledPlugins()) {
        current[record.id] = record;
        if (!record.loaded) {
            continue;
        }
        const auto old = previous.find(record.id);
        if (old == previous.end() || !old->second.loaded) {
            enabledAny = true;
        }
    }

    std::size_t published = 0;
    for (const auto& [pluginId, record] : current) {
        const auto old = previous.find(pluginId);
        const bool changed = old == previous.end() || old->second.state != record.state ||
                             old->second.loaded != record.loaded;
        const auto reasonIt = outcomes.find(pluginId);
        // 被请求的 id 优先用请求结果的原因（拒绝/失败）；级联出来的变化用记录里的
        // 失败原因（成功时为空串）。
        const std::string reason = reasonIt != outcomes.end()
            ? reasonIt->second
            : (record.state == SHOT_PLUGIN_STATE_FAILED ? record.reason : std::string());
        if (!changed && reasonIt == outcomes.end()) {
            continue;  // 状态没变、也不是被请求的 id：不发
        }
        publishChanged(pluginId, record.state == SHOT_PLUGIN_STATE_LOADED, record.state, reason);
        ++published;
    }
    // 防御：被请求的 id 在处理后的快照里找不到（例如记录被移除），也要给设置页一条
    // 失败通知，不能让它一直等。
    for (const auto& [pluginId, reason] : outcomes) {
        if (current.find(pluginId) == current.end()) {
            publishChanged(pluginId, false, SHOT_PLUGIN_STATE_FAILED, reason);
            ++published;
        }
    }
    spdlog::info("HostPluginRegistry: 应用 {} 条启停请求，发布 {} 条 plugins.changed",
                 pending.size(), published);
    return enabledAny;
}

void HostPluginRegistry::publishChanged(const std::string& pluginId, bool enabled,
                                        uint32_t state, const std::string& reason) {
    nlohmann::json payload;
    payload["plugin"] = pluginId;
    payload["enabled"] = enabled;
    payload["state"] = state;
    payload["reason"] = reason;
    const std::string text = payload.dump();
    HostServices::eventBusPublish(nullptr, kPluginsChangedTopic, text.data(),
                                  static_cast<uint32_t>(text.size()));
}

uint32_t HostPluginRegistry::installedCount(void* /*ctx*/) {
    HostPluginRegistry* registry = activeRegistry.load();
    if (registry == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(registry->m_mutex);
    return registry->m_current == nullptr
        ? 0
        : static_cast<uint32_t>(registry->m_current->size());
}

int32_t HostPluginRegistry::installedInfo(void* /*ctx*/, uint32_t index,
                                          ShotPluginStatus* out) {
    if (out == nullptr) {
        return 1;
    }
    if (out->struct_size < SHOT_REQUIRES(ShotPluginStatus, reason)) {
        return 2;
    }
    HostPluginRegistry* registry = activeRegistry.load();
    if (registry == nullptr) {
        return 1;
    }
    std::lock_guard<std::mutex> lock(registry->m_mutex);
    if (registry->m_current == nullptr || index >= registry->m_current->size()) {
        return 1;
    }
    const Plugin::InstalledPlugin& record = (*registry->m_current)[index];
    out->id = record.id.c_str();
    out->name = record.name.c_str();
    out->version = record.version.c_str();
    out->path = record.dllPath.c_str();
    out->state = record.state;
    out->reason = record.reason.c_str();
    return 0;
}

int32_t HostPluginRegistry::isEnabled(void* /*ctx*/, const char* pluginId) {
    if (pluginId == nullptr) {
        return -1;
    }
    HostPluginRegistry* registry = activeRegistry.load();
    if (registry == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(registry->m_mutex);
    if (registry->m_current == nullptr) {
        return -1;
    }
    for (const Plugin::InstalledPlugin& record : *registry->m_current) {
        if (record.id == pluginId) {
            // FAILED 不算"禁用"：用户没有关它，只是这次没起来；state 里能看出失败。
            return record.state == SHOT_PLUGIN_STATE_DISABLED ? 0 : 1;
        }
    }
    return -1;  // 没有这个 id
}

int32_t HostPluginRegistry::setEnabled(void* /*ctx*/, const char* pluginId, int32_t enabled) {
    if (pluginId == nullptr) {
        return -1;
    }
    HostPluginRegistry* registry = activeRegistry.load();
    if (registry == nullptr) {
        return -1;
    }
    const std::string id(pluginId);
    std::lock_guard<std::mutex> lock(registry->m_mutex);
    if (registry->m_current == nullptr) {
        return -1;
    }
    bool known = false;
    for (const Plugin::InstalledPlugin& record : *registry->m_current) {
        if (record.id == id) {
            known = true;
            break;
        }
    }
    if (!known) {
        return -1;  // 没有这个 id
    }
    // 只登记，立刻返回：真正的卸载/加载在 main 线程下一轮 tick 执行。
    registry->m_pending[id] = (enabled != 0);
    // 主循环现在阻塞等待事件（见 HostServices::waitForActivity）；启停请求不走
    // 事件总线，必须显式唤醒 main 线程，否则要等兜底超时才执行。
    HostServices::requestMainThreadWork();
    return 0;
}

}  // namespace Host
