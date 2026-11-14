// Generic 模板的插件状态实现：两个配置项 + capture.done 订阅 + 托盘命令。
//
// 为什么事件回调里只记日志：事件总线回调跑在宿主分发线程上，阻塞会拖住所有
// 插件（document/EVENT_BUS.md）；本模板是"看事件"的最小示例。真正要干重活
// （文件 IO、编码）应该把数据拷走转到自己的线程——见 demos/archive。
//
// label / tooltip 写的是语言表 key，不是中文：设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析，文案本体在本文件的
// kLangTables 里；两处 key 拼写必须严格一致，拼错就显示 key 本身。
//
// 日志保持中文、不进语言表（宿主 e2e 按中文日志断言；语言表只服务界面文案）。

#include "generic_plugin.h"

#include <cstring>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

#include "plugin_entry.h"

namespace generic_template {
namespace {

// capture.done 的主题名：与 document/CAPTURE.md §3 一致。
const char kCaptureDoneTopic[] = "capture.done";

// 列表事件主题：托盘菜单条目的注册走 lists.add（数据形状不进 ABI）。
const char kListsAddTopic[] = "lists.add";

const char kKeyLogPrefix[] = "log_prefix";
const char kKeyVerbose[] = "verbose";

// 默认值只在描述符里写一遍：构造函数 / 初始化也从这两个常量取，避免"描述符
// 默认值"与"初始值"两处漂移。
const char kDefaultLogPrefix[] = "[{{TARGET}}]";
const char kDefaultVerbose[] = "1";

// 文案 key：命名规范 config.<配置 key>.label / .tooltip；托盘命令用命令 id 当 key
// （宿主托盘按 display(owner, label) 解析）。
const char kLabelLogPrefix[] = "config.log_prefix.label";
const char kTooltipLogPrefix[] = "config.log_prefix.tooltip";
const char kLabelVerbose[] = "config.verbose.label";
const char kTooltipVerbose[] = "config.verbose.tooltip";
const char kTrayLabel[] = "{{TARGET}}.capture";

// 语言表：每语言一张单行 JSON（@meta 必填）；整行会超过 120 列，所以用相邻
// raw 字面量拼接——拼出来仍然是一行合法 JSON。各语言的 key 集必须一致。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"{{PLUGIN_NAME}}",)json"
    R"json("plugin.description":"{{PLUGIN_DESCRIPTION}}",)json"
    R"json("config.log_prefix.label":"日志前缀",)json"
    R"json("config.log_prefix.tooltip":"收到截图事件时加在这条日志前面的文字",)json"
    R"json("config.verbose.label":"记录每次截图",)json"
    R"json("config.verbose.tooltip":"开：每次截图记 INFO；关：只记 DEBUG（日志级别过滤后通常看不到）",)json"
    R"json("{{TARGET}}.capture":"截屏（{{PLUGIN_NAME}}）"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"{{PLUGIN_NAME}}",)json"
    R"json("plugin.description":"{{PLUGIN_DESCRIPTION}}",)json"
    R"json("config.log_prefix.label":"Log prefix",)json"
    R"json("config.log_prefix.tooltip":"Text prepended to the log line written on each capture event",)json"
    R"json("config.verbose.label":"Log every capture",)json"
    R"json("config.verbose.tooltip":"On: log each capture at INFO; off: DEBUG only (usually filtered out)",)json"
    R"json("{{TARGET}}.capture":"Capture ({{PLUGIN_NAME}}) "})json",
};

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_STRING,
        .key           = kKeyLogPrefix,
        .label         = kLabelLogPrefix,
        .default_value = kDefaultLogPrefix,
        .options       = nullptr,
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipLogPrefix,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyVerbose,
        .label         = kLabelVerbose,
        .default_value = kDefaultVerbose,
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = kTooltipVerbose,
    },
};

/**
 * @brief 把配置里的布尔文本解析成 bool。
 *
 * 只把 "0" / "false" 当关，其它写法按开处理：配置来自可被手改的文件，把用户
 * 本想打开的开关意外关掉，比多记一行日志更烦人。
 *
 * @param[in] value 配置值文本，不能为 nullptr。
 * @return true = 开；false = 关。
 * @note 无状态纯函数，不属于任何类的接口，所以放匿名命名空间（STYLE.md §6.7 例外）。
 */
bool parseBool(const char* value) {
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

/**
 * @brief 按契约把字符串写进调用方缓冲。
 *
 * 契约规定：buf_size 大于返回值（含结尾 NUL）时才写入；装不下只报长度、不算失败。
 *
 * @param[in]  value    要写入的文本。
 * @param[out] buf      接收缓冲；可为 nullptr（此时只查长度）。
 * @param[in]  bufSize  buf 的字节数。
 * @return >= 0 = 值的字节数（不含 NUL）；< 0 = value 为空。
 */
std::int32_t copyValue(const char* value, char* buf, std::uint32_t bufSize) {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    if (buf != nullptr && bufSize > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

/**
 * @brief 把静态描述符拷进宿主提供的 ShotConfigDesc。
 *
 * @param[in]  src 静态描述符。
 * @param[out] out 宿主提供的输出结构，进入前已填好 struct_size。
 * @return 0 = 成功；1 = 参数为空；2 = 宿主结构太短（缺 tooltip 字段）。
 */
std::int32_t copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (out == nullptr || src == nullptr) {
        return 1;
    }
    // 老宿主的结构可能比本插件认识的短：按 struct_size 判断能写到哪，绝不越界。
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = *src;
    return 0;
}

}  // namespace

GenericPlugin::~GenericPlugin() {
    stop();  // 正常路径由 destroy 调；析构兜底，重复退订是空操作
}

bool GenericPlugin::start(const ShotHostServices* host) {
    m_host = host;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_logPrefix = kDefaultLogPrefix;
        m_verbose = parseBool(kDefaultVerbose);
    }

    // 语言表要在宿主第一次生成 config_list / tray_command_list 快照之前发布；
    // create() 拿不到服务表，这里是第一个能拿到 host 的生命周期路径。
    m_i18n = std::make_unique<shot::I18n>(host, kGeneric.id);
    if (!m_i18n->loadTables(kLangTables, std::size(kLangTables))) {
        shot::Logger(host).warn("[{{TARGET}}] 语言表解析失败，界面文案回退为 key");
    }

    shot::EventBus bus(host);
    if (!bus.subscribe(kCaptureDoneTopic, &GenericPlugin::onCaptureDone, this)) {
        // 老宿主没有事件总线：订阅功能不可用，但插件照常加载（安静降级）。
        shot::Logger(host).warn("[{{TARGET}}] 订阅 capture.done 失败（宿主无事件总线），示例订阅不可用");
        return false;
    }
    m_subscribed = true;
    shot::Logger(host).info("[{{TARGET}}] 已订阅 capture.done：每次截图后会收到保存插件写回的路径");

    publishTrayCommand();
    return true;
}

void GenericPlugin::stop() {
    if (m_subscribed) {
        // 退订句柄就是 this（subscribe 时的 user 指针）：一次摘掉本插件注册的全部主题。
        shot::EventBus bus(m_host);
        bus.unsubscribe(this);
        m_subscribed = false;
    }
    // I18n 析构会退订 lists.changed；放在退订 capture.done 之后，宿主事件分发期间
    // 不会回调一个正在销毁的状态。
    m_i18n.reset();
}

std::uint32_t GenericPlugin::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t GenericPlugin::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t GenericPlugin::getConfig(const char* key, char* buf, std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (std::strcmp(key, kKeyLogPrefix) == 0) {
        return copyValue(m_logPrefix.c_str(), buf, bufSize);
    }
    if (std::strcmp(key, kKeyVerbose) == 0) {
        return copyValue(m_verbose ? "1" : "0", buf, bufSize);
    }
    return -1;  // 不认识的 key：安全忽略（宿主升级后多传一项也不会踩这里）
}

std::int32_t GenericPlugin::setConfig(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (std::strcmp(key, kKeyLogPrefix) == 0) {
        m_logPrefix = value;
        return 0;
    }
    if (std::strcmp(key, kKeyVerbose) == 0) {
        m_verbose = parseBool(value);
        return 0;
    }
    return -1;
}

void GenericPlugin::onCaptureDone(void* user, const char* /*topic*/, const void* data,
                                   std::uint32_t len) {
    auto* self = static_cast<GenericPlugin*>(user);
    if (self == nullptr) {
        return;
    }
    // 事件负载只在回调期间有效：立刻拷走，之后不再碰 data 指针。
    // 回调里不抛异常是硬要求（异常绝不跨越 DLL 边界），解析失败也只 WARN。
    try {
        self->handleCaptureDone(data, len);
    } catch (...) {
        shot::Logger(self->m_host).warn("[{{TARGET}}] 处理 capture.done 时异常，已忽略本次事件");
    }
}

void GenericPlugin::handleCaptureDone(const void* data, std::uint32_t len) {
    std::string payload;
    if (data != nullptr && len > 0) {
        payload.assign(static_cast<const char*>(data), len);
    }
    const nlohmann::json event = nlohmann::json::parse(payload, nullptr, false);
    if (event.is_discarded()) {
        shot::Logger(m_host).warn("[{{TARGET}}] capture.done 负载不是合法 JSON，忽略");
        return;
    }
    const auto pathIt = event.find("path");
    if (pathIt == event.end() || !pathIt->is_string() || pathIt->get<std::string>().empty()) {
        shot::Logger(m_host).warn("[{{TARGET}}] capture.done 缺少 path（没有可用的保存插件？），忽略");
        return;
    }
    std::string prefix;
    bool verbose = false;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        prefix = m_logPrefix;
        verbose = m_verbose;
    }
    const std::string message = prefix + " 收到 capture.done：" + pathIt->get<std::string>();
    if (verbose) {
        shot::Logger(m_host).info(message);
    } else {
        shot::Logger(m_host).debug(message);
    }
}

void GenericPlugin::publishTrayCommand() {
    if (m_trayPublished) {
        return;
    }
    // 托盘菜单是宿主聚合出来的 tray_command_list：插件只发一条 lists.add 声明
    // 自己的条目。owner 必须等于发布者插件 id（宿主按它校验，禁用/卸载时按它清理）。
    // label 写命令 id 当文案 key，托盘用 i18n_list 解析成当前语言。
    const char kTrayCommandPayload[] = R"json({
        "list": "tray_command_list",
        "owner": "{{PLUGIN_ID}}",
        "items": [
            {
                "id": "{{TARGET}}.capture",
                "label": "{{TARGET}}.capture",
                "event": "capture.request",
                "data": "",
                "order": 50
            }
        ]
    })json";
    shot::EventBus bus(m_host);
    if (!bus.publish(kListsAddTopic, kTrayCommandPayload)) {
        shot::Logger(m_host).warn("[{{TARGET}}] 发布 lists.add 失败（无事件总线或队列满），托盘命令不可用");
        return;
    }
    m_trayPublished = true;
    shot::Logger(m_host).info("[{{TARGET}}] 已贡献托盘命令 {{TARGET}}.capture（事件 capture.request）");
}

}  // namespace generic_template
