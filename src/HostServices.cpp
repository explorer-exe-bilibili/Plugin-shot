#include "hostServices.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "config.h"
#include "hostI18n.h"
#include "hostListCatalog.h"
#include "hostPluginRegistry.h"

using namespace Host;

// ---------------------------------------------------------------------------
// 宿主服务表
//
// 静态实例：它必须在插件实例的整个生命周期内有效，所以插件可以放心把指针存起来。
// 将来新增服务就往后追加字段，插件的 SHOT_HAS_FIELD 检查会自动兼容。
// ---------------------------------------------------------------------------

static std::mutex eventMutex;

// 插件侧等级（SHOT_LOG_*）到 spdlog 等级的映射；越界值按 INFO 兜底。
// 只在本 TU 使用，按 STYLE.md §6.7 的例外放进匿名命名空间。
namespace {

// ---- main 循环阻塞等待（对应 HostServices::waitForActivity）--------------------
//
// 主循环没有事件可分发时阻塞在这里，避免 16ms 空转。醒来的条件只有两类：
// 事件总线 / 旧事件队列有新数据，或者宿主自己登记了"有主线程工作"（插件启停）。
// 用独立的 mutex + 标志而不复用 busMutex：发布路径只置标志 + notify，不需要
// 在总线锁里等待，也不会和 dispatchEvents 的队列交换互相纠缠。
std::mutex mainWakeMutex;
std::condition_variable mainWakeCv;
bool mainWakePending = false;

/**
 * @brief 置"main 线程有活"标志并唤醒等待方（任意线程可调）。
 */
void notifyMainThread() {
    {
        std::lock_guard<std::mutex> lock(mainWakeMutex);
        mainWakePending = true;
    }
    mainWakeCv.notify_one();
}

spdlog::level::level_enum toSpdlogLevel(int32_t level) {
    switch (level) {
        case SHOT_LOG_DEBUG: return spdlog::level::debug;
        case SHOT_LOG_INFO:  return spdlog::level::info;
        case SHOT_LOG_WARN:  return spdlog::level::warn;
        case SHOT_LOG_ERROR: return spdlog::level::err;
        default:             return spdlog::level::info;
    }
}

// ---- 进程内事件总线 --------------------------------------------------------
//
// 数据流：publish 深拷贝入队（任意线程） -> 宿主 main 循环调用 dispatchEvents()
// -> 按订阅快照逐个回调（始终在 main 线程）。回调在锁外执行，所以回调里
// publish / subscribe / unsubscribe 都是安全的；新 publish 的事件排到下一轮。

constexpr std::size_t kMaxEventPayload = 64 * 1024;  /**< 单条事件负载上限 */
constexpr std::size_t kMaxPendingEvents = 4096;      /**< 队列上限，满了丢事件 */

struct Subscription {
    std::string topic;   /**< 主题模式：精确 / "prefix.*" / "*" */
    void (*callback)(void* user, const char* topic, const void* data, uint32_t len);
    void* user;          /**< 订阅者上下文，也是退订句柄 */
};

struct PendingEvent {
    std::string topic;   /**< 主题（精确字符串） */
    std::string payload; /**< 深拷贝的负载字节 */
};

std::mutex busMutex;                       /**< 保护下面两个容器 */
std::vector<Subscription> subscriptions;   /**< 订阅表 */
std::deque<PendingEvent> pendingEvents;    /**< 待分发队列 */

/**
 * @brief 订阅模式与主题是否匹配。
 *
 * 支持三种写法：精确匹配、"prefix.*" 前缀通配、"*" 全局通配。
 * @param[in] pattern subscribe 时给的主题模式。
 * @param[in] topic   publish 时的具体主题。
 * @return true = 命中。
 */
bool topicMatches(const std::string& pattern, const std::string& topic) {
    if (pattern == "*") {
        return true;
    }
    if (pattern.size() >= 2 && pattern.compare(pattern.size() - 2, 2, ".*") == 0) {
        const std::string prefix = pattern.substr(0, pattern.size() - 1);  // 保留结尾的 '.'
        return topic.size() >= prefix.size() && topic.compare(0, prefix.size(), prefix) == 0;
    }
    return pattern == topic;
}

}  // namespace

void HostServices::hostLog(void* /*ctx*/, int32_t level, const char* msg, int32_t len) {
    if (msg == nullptr) {
        return;
    }
    const std::string text(msg, (len < 0) ? std::strlen(msg) : static_cast<std::size_t>(len));
    const auto spdlog_level = toSpdlogLevel(level);
    if (const auto logger = spdlog::get("plugin"); logger != nullptr) {
        logger->log(spdlog_level, text);
    } else {
        // main.cpp 还没初始化日志时退回默认 logger；此时消息里手动带上来源。
        spdlog::log(spdlog_level, "[plugin] {}", text);
    }
}

void* HostServices::hostQueryInterface(void* /*ctx*/, const char* ifaceId, uint32_t /*version*/) {
    if (ifaceId == nullptr) {
        return nullptr;
    }
    // 每个服务只有一张表：事件总线/宿主配置/看板广播定义在本 TU，列表目录与
    // 插件注册表的表定义在各自类的 .cpp 里，这里只做 id 分发，不再重复定义。
    if (std::strcmp(ifaceId, SHOT_HOST_IFACE_CONFIG) == 0) {
        return const_cast<ShotHostConfigV1*>(&g_hostConfig);
    }
    if (std::strcmp(ifaceId, SHOT_HOST_IFACE_BOARD_MSG) == 0) {
        return const_cast<ShotHostBoardEventV1*>(&g_hostBoardEvent);
    }
    if (std::strcmp(ifaceId, SHOT_HOST_IFACE_EVENT_BUS) == 0) {
        return const_cast<ShotHostEventBusV1*>(&g_eventBus);
    }
    if (std::strcmp(ifaceId, SHOT_HOST_IFACE_PLUGIN_LISTS) == 0) {
        return const_cast<ShotHostPluginListsV1*>(HostListCatalog::service());
    }
    if (std::strcmp(ifaceId, SHOT_HOST_IFACE_PLUGIN_REGISTRY) == 0) {
        return const_cast<ShotHostPluginRegistryV1*>(HostPluginRegistry::service());
    }
    return nullptr;  // 其它 id 暂不支持：插件按"没有这个服务"安静降级。
}

// 可能在插件的任意线程上被调用：先拷一份再入队，投递方返回后就与这份数据无关了。
int32_t HostServices::postToHost(void* /*ctx*/, const ShotEvent* ev) {
    if (ev == nullptr || ev->name == nullptr) {
        return -1;
    }
    if (!SHOT_HAS_FIELD(ev, ShotEvent, data_len)) {
        return -1;  // 老插件的事件里没有 data_len，不猜
    }

    HostEvent event;
    event.name = ev->name;
    event.data = ev->data;
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        g_eventQueue.push(std::move(event));
    }
    // 旧事件队列同样由 main 循环排空，入队后也要唤醒。
    notifyMainThread();

    const std::string line =
        "[host] received event " + std::string(ev->name) + " (" + std::to_string(ev->data_len) + " bytes)";
    hostLog(nullptr, SHOT_LOG_INFO, line.c_str(), -1);
    return 0;
}

int32_t HostServices::broadcastEvent(const ShotEvent* ev) {
    if (ev == nullptr || ev->name == nullptr) {
        return -1;
    }
    if (!SHOT_HAS_FIELD(ev, ShotEvent, data_len)) {
        return -1;  // 老插件的事件里没有 data_len，不猜
    }
    if (ev->data == nullptr && ev->data_len != 0) {
        return -1;
    }

    // 广播 = 发布到事件总线：订阅了同名主题的插件都会收到。
    const int32_t rc = eventBusPublish(nullptr, ev->name, ev->data, ev->data_len);
    if (rc == 0) {
        const std::string line =
            "[host] broadcast event " + std::string(ev->name) + " (" + std::to_string(ev->data_len) + " bytes)";
        hostLog(nullptr, SHOT_LOG_INFO, line.c_str(), -1);
    } else {
        hostLog(nullptr, SHOT_LOG_WARN, "broadcast_event dropped: bus queue full or invalid", -1);
    }
    return rc;
}

int32_t HostServices::eventBusPublish(void* /*ctx*/, const char* topic, const void* data, uint32_t len) {
    if (topic == nullptr || topic[0] == '\0') {
        return -1;
    }
    if (data == nullptr && len != 0) {
        return -1;
    }
    if (len > kMaxEventPayload) {
        return -1;
    }
    PendingEvent event;
    event.topic = topic;
    if (len != 0) {
        event.payload.assign(static_cast<const char*>(data), len);
    }
    {
        std::lock_guard<std::mutex> lock(busMutex);
        if (pendingEvents.size() >= kMaxPendingEvents) {
            return -1;  // 队列满：丢弃并让发布方知道（不阻塞）
        }
        pendingEvents.push_back(std::move(event));
    }
    notifyMainThread();
    return 0;
}

int32_t HostServices::eventBusSubscribe(
    void* /*ctx*/, const char* topic,
    void (*callback)(void* user, const char* topic, const void* data, uint32_t len),
    void* user) {
    if (topic == nullptr || topic[0] == '\0' || callback == nullptr) {
        return -1;
    }
    Subscription subscription;
    subscription.topic = topic;
    subscription.callback = callback;
    subscription.user = user;
    {
        std::lock_guard<std::mutex> lock(busMutex);
        subscriptions.push_back(std::move(subscription));
    }
    return 0;
}

int32_t HostServices::eventBusUnsubscribe(void* /*ctx*/, void* user) {
    if (user == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(busMutex);
    const auto firstRemoved = std::remove_if(
        subscriptions.begin(), subscriptions.end(),
        [user](const Subscription& subscription) { return subscription.user == user; });
    const bool hadAny = firstRemoved != subscriptions.end();
    subscriptions.erase(firstRemoved, subscriptions.end());
    return hadAny ? 0 : -1;
}

void HostServices::dispatchEvents() {
    // 先在锁内换出队列、拷一份订阅快照；回调全部在锁外执行。
    std::deque<PendingEvent> events;
    std::vector<Subscription> snapshot;
    {
        std::lock_guard<std::mutex> lock(busMutex);
        events.swap(pendingEvents);
        snapshot = subscriptions;
    }
    for (const PendingEvent& event : events) {
        const void* data = event.payload.empty() ? nullptr : event.payload.data();
        const auto len = static_cast<uint32_t>(event.payload.size());
        for (const Subscription& subscription : snapshot) {
            if (topicMatches(subscription.topic, event.topic)) {
                subscription.callback(subscription.user, event.topic.c_str(), data, len);
            }
        }
    }
}

void HostServices::requestMainThreadWork() {
    notifyMainThread();
}

bool HostServices::waitForActivity(std::int32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mainWakeMutex);
    if (!mainWakePending && timeoutMs > 0) {
        // predicate 写在 wait_for 里：被 notify 时只会在标志真的置位后返回，
        // 不会因为虚假唤醒提前返回。
        mainWakeCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [] { return mainWakePending; });
    }
    if (!mainWakePending) {
        return false;  // 兜底超时：只防漏唤醒，正常路径不会走到这里
    }
    mainWakePending = false;
    return true;
}

int32_t HostServices::hostGetConfig(void* /*ctx*/, const char* key, char* buf, uint32_t buf_size) {
    if (key == nullptr) {
        return -1;
    }
    core::Config& config = core::Config::getInstance();
    // 先查存在性：Config::get 在缺键时会回落默认实例并打日志，而插件读一个还没
    // 出现过的键（如 settings_ui.window 首次启动）是正常情况，应该安静返回 -1。
    if (!config.contains(key)) {
        return -1;
    }
    std::string value = config.get(key).toString();
    if (buf != nullptr && buf_size > 0) {
        if (buf_size <= value.size()) {
            return -1;  // 缓冲区太小
        }
        std::copy(value.begin(), value.end(), buf);
        buf[value.size()] = '\0';  // NUL 结尾
    }
    return static_cast<int32_t>(value.size());
}

int32_t HostServices::hostSetConfig(void* /*ctx*/, const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    // core/language 同时作用于运行期语言中心：只写配置会让"当前语言"与配置不一致，
    // 直到下次重启才生效。这里立刻切换；i18n_list 由 HostRuntime::tick 的语言检测重建。
    if (std::strcmp(key, "core/language") == 0 &&
        !Host::I18n::instance().setPreference(value)) {
        return -1;  // 配置已写入但语言值非法：明确返回失败，调用方按需处理
    }
    core::Config::getInstance().set(key, core::ConfigItem(value));
    return 0;
}

void HostServices::hostSaveConfig(void* /*ctx*/) {
    core::Config::getInstance().saveConfigFile();
}

const ShotHostConfigV1 HostServices::g_hostConfig = {
    .struct_size = sizeof(ShotHostConfigV1),
    .version     = 1,
    .get_config  = &HostServices::hostGetConfig,
    .set_config  = &HostServices::hostSetConfig,
    .save_config = &HostServices::hostSaveConfig,
};

const ShotHostBoardEventV1 HostServices::g_hostBoardEvent = {
    .struct_size     = sizeof(ShotHostBoardEventV1),
    .version         = 1,
    .broadcast_event = &HostServices::broadcastEvent,
};

const ShotHostEventBusV1 HostServices::g_eventBus = {
    .struct_size = sizeof(ShotHostEventBusV1),
    .version     = 1,
    .publish     = &HostServices::eventBusPublish,
    .subscribe   = &HostServices::eventBusSubscribe,
    .unsubscribe = &HostServices::eventBusUnsubscribe,
};

std::queue<HostEvent> Host::g_broadcastQueue;
std::queue<HostEvent> Host::g_eventQueue;

const ShotHostServices Host::g_hostServices = {
    .struct_size     = sizeof(ShotHostServices),
    .abi_major       = SHOT_ABI_MAJOR,
    .abi_minor       = SHOT_ABI_MINOR,
    .reserved0       = 0,
    .host_context    = nullptr,
    .log             = &Host::HostServices::hostLog,
    .query_interface = &Host::HostServices::hostQueryInterface,
    .post_to_host    = &Host::HostServices::postToHost,
};
