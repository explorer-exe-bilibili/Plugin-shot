// 会话控制插件的主体：配置回调 + D-Bus 服务的启停编排。
//
// 这个文件只回答"什么时候开、什么时候关"：
//   · 配置声明与值：session_control_config.cpp
//   · D-Bus 线程、注册/注销、方法派发：control_service*.cpp
//   · ABI 胶水与能力目录：plugin_entry.cpp
//
// 多语言：配置项的 label/tooltip 在 session_control_config.cpp 里写的是语言表 key，
// 文案本体由本文件的 kLangTables 发布到 i18n_source_list；宿主生成 i18n_list 后，
// 设置页按 display(owner, key) 解析。日志保持中文（e2e 按中文日志断言），不翻译。
//
// 线程：start()/stop() 由宿主生命周期锁串行调用；set_config 可能在任意线程被调
// （设置页应用），所以 enabled 翻转统一走 applyEnabled() 的互斥量。

#include "session_control_plugin.h"

#include <iterator>
#include <memory>
#include <utility>

#include "plugin_entry.h"
#include "shot_plugin_helpers.h"

namespace session_control {
namespace {

// 本插件的用户可见文案：配置项的 label/tooltip + 设置页里的插件分区名
// （config_list 的 owner_name_key = "plugin.name"，见 document/LISTS.md §2.4）。
// key 拼写必须与 session_control_config.cpp 完全一致，拼错会原样显示 key。
// 每语言一张 JSON 对象，@meta 必填，两张表的 key 集必须一致；JSON 文本里不能有
// 裸换行，而整行又可能超过 120 列，所以用相邻 raw 字面量拼接——拼出来仍是一行。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"会话控制",)json"
    R"json("config.enabled.label":"启用 D-Bus 控制",)json"
    R"json("config.enabled.tooltip":)json"
    R"json("在会话总线注册 org.pluginshot.Control；Wayland 下用桌面「自定义快捷键」)json"
    R"json(执行 gdbus call 触发截图，关掉即注销名称"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"Session Control",)json"
    R"json("config.enabled.label":"Enable D-Bus control",)json"
    R"json("config.enabled.tooltip":)json"
    R"json("Exposes org.pluginshot.Control on the session bus so a desktop custom shortcut can )json"
    R"json(trigger capture via gdbus; turning it off withdraws the name"})json",
};

}  // namespace

SessionControlPlugin::SessionControlPlugin() = default;

SessionControlPlugin::~SessionControlPlugin() { stop(); }

bool SessionControlPlugin::start(const ShotHostServices* host) {
    if (host == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_applyMutex);
        m_host = host;
    }

    // 文案表要在设置页第一次读 config_list 之前发布：宿主据此生成 i18n_list。
    // owner 用插件身份证（kSessionControl.id），避免与 metadata.json 漂移。
    // I18n 构造/loadTables 会分配内存、发布 lists.add，异常绝不能穿出 ABI 边界。
    try {
        auto i18n = std::make_unique<shot::I18n>(host, kSessionControl.id);
        if (!i18n->loadTables(kLangTables, std::size(kLangTables))) {
            shot::Logger(host).warn("[session_control] 文案表解析失败，配置项文案回退为 key");
        }
        m_i18n = std::move(i18n);
    } catch (...) {
        m_i18n.reset();
        shot::Logger(host).warn("[session_control] 文案表初始化异常，配置项文案回退为 key");
    }

    // create 阶段宿主还没回灌存盘配置：先按默认值（enabled=1）注册；如果存盘值是 0，
    // 宿主随后的 set_config 会立刻注销。这与其它配置项"create 之后 apply_stored"的
    // 顺序一致，也避免在 create 里另读一份宿主配置。
    applyEnabled(m_config.enabled());
    return true;
}

void SessionControlPlugin::stop() {
    {
        std::lock_guard<std::mutex> lock(m_applyMutex);
        m_service.stop();
        m_host = nullptr;
    }
    // I18n 析构会退订 lists.changed；放在服务停止之后，保证不会再回调一个正在销毁的
    // 插件状态。
    m_i18n.reset();
}

std::uint32_t SessionControlPlugin::configCount() const noexcept {
    return m_config.configCount();
}

std::int32_t SessionControlPlugin::configDesc(std::uint32_t index,
                                              ShotConfigDesc* out) const noexcept {
    return m_config.configDesc(index, out);
}

std::int32_t SessionControlPlugin::getConfig(const char* key, char* buf,
                                             std::uint32_t bufSize) const {
    return m_config.getValue(key, buf, bufSize);
}

std::int32_t SessionControlPlugin::setConfig(const char* key, const char* value) {
    const std::int32_t result = m_config.setValue(key, value);
    if (result == 0) {
        // 只在 enabled 真正变化时才会启停服务（applyEnabled 内部先看 running 状态）。
        applyEnabled(m_config.enabled());
    }
    return result;
}

void SessionControlPlugin::applyEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_applyMutex);
    if (m_host == nullptr) {
        return;  // stop() 之后不再接受新的启停请求
    }
    if (enabled) {
        if (m_service.running()) {
            return;  // 已经开着：重复 set_config 不刷日志
        }
        if (!m_service.start(m_host)) {
            return;  // ControlService 已经记录具体原因
        }
        shot::Logger(m_host).info(
            "[session_control] D-Bus 服务已启动，正在申请名称 org.pluginshot.Control");
    } else {
        if (!m_service.running()) {
            return;  // 已经关着：不重复报"已关闭"
        }
        m_service.stop();
        shot::Logger(m_host).info(
            "[session_control] D-Bus 入口已关闭：org.pluginshot.Control 已注销");
    }
}

}  // namespace session_control
