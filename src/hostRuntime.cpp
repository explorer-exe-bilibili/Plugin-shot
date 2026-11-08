#include "hostRuntime.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "config.h"

namespace Host {

HostRuntime::HostRuntime() : m_configCatalog(m_lifecycleMutex) {}

HostRuntime::~HostRuntime() {
    // 顺序有讲究，分三步：
    //   1. 先注销两个服务，插件线程之后拿到的服务表调用只会安静失败；
    //   2. 放掉目录/注册表快照里持有的实例引用——它们比管理器更晚释放的话，实例
    //      会拖到 DLL 卸载之后才销毁，而且 module->shutdown 会早于 destroy，违反契约；
    //   3. 最后卸载插件（deinit 停线程/销窗口），再让成员按声明逆序析构。
    m_configCatalog.unbind();
    m_pluginRegistry.unbind();
    m_manager.unloadAll();
}

bool HostRuntime::init(const std::string& pluginDir) {
    if (!m_manager.loadPlugins(pluginDir)) {
        return false;
    }

    m_configCatalog.bind(&m_manager);
    {
        // 启动回灌：把已存值 / default_value 灌回所有提供者。init 阶段还没有设置页
        // 线程，但 applyStoredLocked 的约定是"调用方持生命周期锁"——这里持一次，
        // 也把"插件调用期间不许卸载"的纪律固定在入口处。
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        m_configCatalog.applyStoredLocked();
    }

    m_pluginRegistry.bind(&m_manager);
    m_pluginRegistry.refresh();

    // apply_stored 与 loadPlugins 写的都是内存（默认值、禁用列表），第一次 tick 落盘。
    m_configDirty = true;

    std::size_t loaded = 0;
    const std::vector<Plugin::InstalledPlugin> installed = m_manager.getInstalledPlugins();
    for (const Plugin::InstalledPlugin& record : installed) {
        if (record.loaded) {
            ++loaded;
        }
    }
    spdlog::info("Host runtime ready: {} plugin instances loaded, {} installed records",
                 loaded, installed.size());
    return true;
}

void HostRuntime::tick() {
    if (m_pluginRegistry.hasPending()) {
        // 启停整段持生命周期锁：设置页线程可能正在目录服务里调用某个插件读值，
        // 这时把它卸掉就是 use-after-free。目录服务反过来也持有同一把锁。
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        // 必须先放掉目录快照：快照里的 shared_ptr 会让被禁用的实例活到下一次目录
        // 调用，结果 module->shutdown 早于 destroy、DLL 也释放不掉。
        m_configCatalog.dropSnapshots();
        m_pluginRegistry.applyPending(m_manager, m_configCatalog);
        // 实时枚举实现下 refresh 只是表达"目录可能变了"；保留调用以对齐生命周期顺序。
        m_configCatalog.refresh();
        m_pluginRegistry.refresh();
        m_configDirty = true;  // 禁用列表可能变了，下一段落盘
    }

    // 配置落盘统一在 tick 末尾做（loadPlugins/apply_stored 只改内存）。
    // 设置页"应用"时走的 config.save() 是例外：它明确要求同步落盘。
    if (m_configDirty) {
        if (core::Config::getInstance().saveConfigFile()) {
            m_configDirty = false;
        } else {
            spdlog::warn("Failed to persist configuration, will retry on the next tick");
        }
    }
}

Plugin::PluginManager& HostRuntime::manager() {
    return m_manager;
}

HostConfigCatalog& HostRuntime::configCatalog() {
    return m_configCatalog;
}

HostPluginRegistry& HostRuntime::pluginRegistry() {
    return m_pluginRegistry;
}

}  // namespace Host
