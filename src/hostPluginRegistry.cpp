#include "hostPluginRegistry.h"

#include <atomic>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "hostConfigCatalog.h"
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

bool HostPluginRegistry::findInstalled(const std::string& pluginId,
                                       Plugin::InstalledPlugin& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_current == nullptr) {
        return false;
    }
    for (const Plugin::InstalledPlugin& record : *m_current) {
        if (record.id == pluginId) {
            out = record;
            return true;
        }
    }
    return false;
}

bool HostPluginRegistry::applyPending(Plugin::PluginManager& manager,
                                      HostConfigCatalog& catalog) {
    std::map<std::string, bool> pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pending.swap(m_pending);
    }
    if (pending.empty()) {
        return false;
    }

    // 真正的生命周期操作只能在 main 线程做；调用方已持生命周期锁，这里不再加锁。
    std::vector<Outcome> outcomes;
    outcomes.reserve(pending.size());
    bool enabledAny = false;
    for (const auto& [pluginId, enabled] : pending) {
        std::string reason;
        const bool ok = manager.setPluginEnabled(pluginId, enabled, reason);
        if (ok && enabled) {
            enabledAny = true;
        }
        outcomes.push_back(Outcome{pluginId, enabled, reason});
    }

    // SETTINGS.md §3.1：重新启用后把该插件的已存配置再灌回去。
    // applyStoredLocked 的约定是"调用方持生命周期锁"，这里的调用方就是 HostRuntime。
    if (enabledAny) {
        catalog.applyStoredLocked();
    }

    refresh();

    // 状态以"处理后的真实记录"为准：禁用被依赖拒绝时插件仍是 LOADED，
    // 启用失败时记录是 FAILED。契约里 enabled 的含义是"已启用/已加载"。
    for (const Outcome& outcome : outcomes) {
        Plugin::InstalledPlugin record;
        const bool found = findInstalled(outcome.id, record);
        const bool enabledAfter = found && record.state == SHOT_PLUGIN_STATE_LOADED;
        const uint32_t state = found
            ? record.state
            : (outcome.requested ? SHOT_PLUGIN_STATE_FAILED : SHOT_PLUGIN_STATE_DISABLED);
        publishChanged(outcome.id, enabledAfter, state, outcome.reason);
    }
    return true;
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
    return 0;
}

}  // namespace Host
