#include "hostRuntime.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "config.h"
#include "hostI18n.h"
#include "hostServices.h"

namespace {

// 主循环的退出主题；负载为空，任何插件都可以 publish（见 document/EVENT_BUS.md §4）。
const char kAppQuitTopic[] = "app.quit";

// 列表协议全家桶：宿主只订阅这一组主题，写路径全部在 main 线程的分发回调里。
// 前缀通配（"lists.*"）让以后新增 lists.xxx 事件不用再改订阅代码。
const char kListsTopicPattern[] = "lists.*";

}  // namespace

namespace Host {

HostRuntime::HostRuntime()
    : m_listCatalog(m_lifecycleMutex), m_capturePipeline(m_manager, m_lifecycleMutex) {}

HostRuntime::~HostRuntime() {
    // 顺序有讲究，分四步：
    //   1. 先停采集流水线（退订 capture.request + join worker）：它的回调与工作线程里
    //      都有 this，不能在析构后再被调用；worker 正在跑的那张图会先走完再退出；
    //   2. 注销两个服务并退订 app.quit / lists.*，插件线程之后拿到的服务表调用只会安静失败；
    //   3. 放掉列表目录/注册表快照里持有的实例引用——它们比管理器更晚释放的话，实例
    //      会拖到 DLL 卸载之后才销毁，而且 module->shutdown 会早于 destroy，违反契约；
    //   4. 最后卸载插件（deinit 停线程/销窗口），再让成员按声明逆序析构。
    m_capturePipeline.stop();
    m_listCatalog.unbind();
    m_pluginRegistry.unbind();
    if (m_quitSubscribed || m_listsSubscribed) {
        // 两条订阅用的是同一个 user（this），一次退订就全部摘掉。
        HostServices::eventBusUnsubscribe(nullptr, this);
        m_quitSubscribed = false;
        m_listsSubscribed = false;
    }
    m_manager.unloadAll();
}

bool HostRuntime::init(const std::string& pluginDir) {
    // 语言偏好在加载插件之前生效：插件 create 里就能读到 i18n_list（若有），
    // 之后 applyStored 回灌 core/language 的默认值也不会改变已生效的语言。
    Host::I18n::instance().loadPreference();
    m_lastLanguage = Host::I18n::instance().language();
    spdlog::info("i18n: 界面语言 = {}（偏好 {}，回退 {}）", m_lastLanguage,
                 Host::I18n::instance().preference(), Host::I18n::instance().fallbackLanguage());

    if (!m_manager.loadPlugins(pluginDir)) {
        return false;
    }

    m_listCatalog.bind(&m_manager);
    {
        // 启动回灌：先 applyStored（把已存值/默认值推给插件）再 rebuild（把插件的
        // 当前值读成 config_list）。顺序反了，设置页第一次打开会显示插件自己的
        // 初值，而配置文件里其实已经有旧值。
        //
        // init 阶段还没有设置页线程，但 applyStoredLocked / rebuildConfigListLocked
        // 的约定都是"调用方持生命周期锁"——这里持一次，也把"插件调用期间不许卸载"
        // 的纪律固定在入口处。
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        m_listCatalog.applyStoredLocked();
        m_listCatalog.rebuildConfigListLocked();
        // i18n_list 至少要有 core 文案：设置页/托盘在 init 完成后就可能打开。
        m_listCatalog.rebuildI18nListIfDirtyLocked();
    }

    m_pluginRegistry.bind(&m_manager);
    m_pluginRegistry.refresh();

    // 采集流水线与事件订阅都在 init 里做：它们都只把工作交回 main 线程执行。
    m_capturePipeline.start();

    // 列表事件在插件加载之后订阅也没问题：插件可能在 create() 里就 publish
    // lists.add，事件只是入队；主循环分发时才取订阅快照，所以不会漏。
    m_listsSubscribed = HostServices::eventBusSubscribe(
        nullptr, kListsTopicPattern, &HostRuntime::onListEvent, this) == 0;
    if (!m_listsSubscribed) {
        spdlog::warn("HostRuntime: 订阅 {} 失败，列表协议不可用", kListsTopicPattern);
    }

    m_quitSubscribed =
        HostServices::eventBusSubscribe(nullptr, kAppQuitTopic, &HostRuntime::onAppQuit, this) == 0;
    if (!m_quitSubscribed) {
        spdlog::warn("HostRuntime: 订阅 {} 失败，进程只能被外部结束", kAppQuitTopic);
    }

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
        // 启停整段持生命周期锁：设置页线程可能正在列表服务里调用某个插件读值，
        // 这时把它卸掉就是 use-after-free。列表目录反过来也持有同一把锁。
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        // 必须先放掉列表目录快照：快照里的 shared_ptr 会让被禁用的实例活到下一次
        // 目录调用，结果 module->shutdown 早于 destroy、DLL 也释放不掉。
        m_listCatalog.dropSnapshots();

        // 记下启停前的已加载实例：禁用/卸载会让实例从管理器里消失，而"清掉它的
        // 列表贡献"需要知道是哪一个。用实例 id 而不是 getInstalledPlugins 的
        // loaded 标记——后者在加载失败时也会变，但清理只关心"实例没了"。
        std::set<std::string> loadedBefore;
        for (const auto& instance : m_manager.getAllInstances()) {
            if (instance != nullptr) {
                loadedBefore.insert(instance->getId());
            }
        }

        const bool enabledAny = m_pluginRegistry.applyPending(m_manager);

        std::set<std::string> loadedAfter;
        for (const auto& instance : m_manager.getAllInstances()) {
            if (instance != nullptr) {
                loadedAfter.insert(instance->getId());
            }
        }

        // 被禁用/卸载的插件，它的列表贡献必须立刻消失：否则托盘菜单里会留着一条
        // 点了没反应、甚至指向已卸载事件的死条目。tray_command_list 的 core 预置
        // 项 owner 是 "core"，不在两个集合里，不会被误删。
        for (const std::string& pluginId : loadedBefore) {
            if (loadedAfter.count(pluginId) == 0) {
                m_listCatalog.removeOwner(pluginId);
            }
        }

        // 重新启用（含级联启用）的插件先回灌已存配置、再重建 config_list；
        // 顺序反了列表里就是插件自己的初值。
        if (enabledAny) {
            m_listCatalog.applyStoredLocked();
        }
        // config_list 是宿主生成的：无论本轮是启用、禁用还是只有失败，都按当前
        // 已加载实例重建一次，被移除插件的配置项就不会残留在设置页里。
        m_listCatalog.rebuildConfigListLocked();

        m_pluginRegistry.refresh();
        m_configDirty = true;  // 禁用列表可能变了，下一段落盘
    }

    // i18n_list 的重建不调用插件能力，不需要生命周期锁：core/language 的 set_config
    // 与 i18n_source_list 的贡献都只在 main 线程改脏标记，这里统一重建一次再 flush。
    m_listCatalog.rebuildI18nListIfDirtyLocked();
    // 语言变了：注册表快照里的 reason 文本要按新语言重新生成（快照由 main 线程重建）。
    const std::string& language = Host::I18n::instance().language();
    if (language != m_lastLanguage) {
        m_lastLanguage = language;
        m_pluginRegistry.refresh();
    }

    // 列表变更统一在这里 flush：本轮 dispatch 里处理的事件、以及上面的启停清理
    // 产生的变化，都被合并进同一个 pending 集合，每个 list 只发一条 lists.changed。
    m_listCatalog.flushChanged();

    // 配置落盘统一在 tick 末尾做（loadPlugins/apply_stored 只改内存）。
    // 设置页"应用"时走的 lists.commit 是例外：它明确要求同步落盘。
    if (m_configDirty) {
        if (core::Config::getInstance().saveConfigFile()) {
            m_configDirty = false;
        } else {
            spdlog::warn("Failed to persist configuration, will retry on the next tick");
        }
    }
}

bool HostRuntime::quitRequested() const {
    return m_quitRequested;
}

void HostRuntime::onAppQuit(void* user, const char* /*topic*/, const void* /*data*/,
                            uint32_t /*len*/) {
    auto* runtime = static_cast<HostRuntime*>(user);
    if (runtime == nullptr) {
        return;
    }
    // 回调在宿主分发线程（main）执行；重复的 app.quit 只记一次日志。
    if (!runtime->m_quitRequested) {
        runtime->m_quitRequested = true;
        spdlog::info("HostRuntime: 收到 {}，主循环退出", kAppQuitTopic);
    }
}

void HostRuntime::onListEvent(void* user, const char* topic, const void* data, uint32_t len) {
    auto* runtime = static_cast<HostRuntime*>(user);
    if (runtime == nullptr || topic == nullptr) {
        return;
    }
    // 事件总线已经深拷贝过负载，但这里仍按"只在回调期间有效"的契约立即拷成
    // std::string，再交给目录解析；回调返回后不再持有 data 指针。
    const std::string payload =
        (data != nullptr && len != 0) ? std::string(static_cast<const char*>(data), len)
                                      : std::string();
    runtime->m_listCatalog.handleEvent(topic, payload);
}

Plugin::PluginManager& HostRuntime::manager() {
    return m_manager;
}

HostListCatalog& HostRuntime::listCatalog() {
    return m_listCatalog;
}

HostPluginRegistry& HostRuntime::pluginRegistry() {
    return m_pluginRegistry;
}

CapturePipeline& HostRuntime::capturePipeline() {
    return m_capturePipeline;
}

}  // namespace Host
