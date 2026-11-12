// Linux 桌面通知：libnotify（D-Bus org.freedesktop.Notifications）。
//
// 与 Windows 版的差异只在"怎么显示"，事件订阅、配置项、文案 key 完全一致：
//   · 不用隐藏窗口/占位图标：libnotify 直接把通知交给桌面通知守护进程；
//   · 不起通知线程：show 是一次短 D-Bus 调用，在宿主分发线程同步提交即可，
//     destroy 时只要退订 + notify_uninit，不存在线程/窗口类跨 DLL 的生命周期；
//   · 声音：libnotify 没有"播放声音"接口，只能附 sound-name hint；而 hint 只是
//     给守护进程的建议（GNOME/KDE 可能播放、dunst 可能忽略），本插件绝不 spawn
//     外部播放器——README 已写明"Linux 上不保证有声"。
//
// 失败降级：没有 D-Bus 会话、没有通知守护进程、用户关了通知，show 返回 FALSE
// 或静默成功；本插件只记 WARN，不影响截屏本身（文件已经落盘）。

#include "notify_toast.h"

#include <cstdint>
#include <memory>
#include <string>

#include <glib-object.h>
#include <libnotify/notify.h>
#include <nlohmann/json.hpp>

#include "notify_config.h"
#include "notify_toast_linux_impl.h"
#include "shot_plugin_helpers.h"

namespace notify {
namespace {

// 通知停留时长（毫秒）：与 Windows 的 5s 对上；桌面守护进程可以忽略这个值。
constexpr int kNotificationTimeoutMs = 5000;

/** @brief 从 JSON 负载里读一个字符串字段；缺失/类型不对返回空串。 */
std::string jsonString(const nlohmann::json& object, const char* field) {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

}  // namespace

NotifyToast::NotifyToast() : m_impl(std::make_unique<Impl>()) {}

NotifyToast::~NotifyToast() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool NotifyToast::start(const ShotHostServices* host, NotifyConfig* config,
                        const shot::I18n* i18n) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->initialized) {
        return true;  // 已经在跑：重复调用是空操作
    }
    m_impl->host = host;
    m_impl->config = config;
    m_impl->i18n = i18n;

    shot::EventBus bus(host);
    if (!bus.available()) {
        // 老宿主没有事件总线：收不到任何截屏结果，本插件安静降级为不可用。
        shot::Logger(host).warn("[notify] 宿主未提供事件总线（旧版宿主？），消息提示不可用");
        return false;
    }

    // libnotify 的全局初始化必须在创建通知之前；失败通常意味着没有 D-Bus 会话
    // （例如从 cron/ssh 启动），此时保留插件但提示不可用，不升级成加载失败。
    if (!notify_init("Plugin-shot")) {
        shot::Logger(host).warn("[notify] notify_init 失败（没有 D-Bus 会话？），桌面通知不可用");
        return false;
    }
    m_impl->initialized = true;

    // 两个主题都要订阅：成功和失败是两条独立事件，不能因为第一条订上就短路掉第二条。
    // 任意一条失败都退回半订阅状态，避免用户只看到成功、看不到失败原因。
    const bool subscribedDone = bus.subscribe(kTopicCaptureDone, &Impl::onBusEvent, m_impl.get());
    const bool subscribedFailed =
        bus.subscribe(kTopicCaptureFailed, &Impl::onBusEvent, m_impl.get());
    if (!subscribedDone || !subscribedFailed) {
        bus.unsubscribe(m_impl.get());
        shot::Logger(host).warn("[notify] 订阅 capture.done / capture.failed 失败，消息提示不可用");
        notify_uninit();
        m_impl->initialized = false;
        return false;
    }
    m_impl->acceptingEvents = true;
    shot::Logger(host).info("[notify] 已订阅 capture.done / capture.failed（libnotify 桌面通知）");
    return true;
}

void NotifyToast::stop() {
    if (!m_impl) {
        return;
    }
    // 先退订：destroy 返回后宿主不再回调本插件（EVENT_BUS.md §5）；同一 main
    // 线程上也不会再有在途回调，所以之后释放 libnotify 是安全的。无条件尝试一次：
    // start 可能在“订上一条、第二条失败”的半订阅状态退出，这里要把残留订阅也摘干净。
    if (m_impl->host != nullptr) {
        shot::EventBus bus(m_impl->host);
        bus.unsubscribe(m_impl.get());
    }
    m_impl->acceptingEvents = false;
    if (m_impl->initialized) {
        notify_uninit();
        m_impl->initialized = false;
        shot::Logger(m_impl->host).info("[notify] libnotify 已释放");
    }
    m_impl->host = nullptr;
    m_impl->config = nullptr;
    m_impl->i18n = nullptr;
}

void NotifyToast::Impl::onBusEvent(void* user, const char* topic, const void* data,
                                   std::uint32_t len) {
    auto* self = static_cast<Impl*>(user);
    if (self == nullptr || topic == nullptr || !self->acceptingEvents) {
        return;
    }
    // 回调是跨 DLL 的 C 边界，异常绝不能穿过去（STYLE.md §6.2）：分配失败就丢掉事件。
    try {
        self->handleEvent(topic, static_cast<const char*>(data), len);
    } catch (...) {
        shot::Logger(self->host).warn("[notify] 处理事件时异常，已忽略该事件");
    }
}

void NotifyToast::Impl::handleEvent(const char* topic, const char* data, std::uint32_t len) {
    std::string payload;
    if (data != nullptr && len > 0) {
        payload.assign(data, len);  // 契约：只在回调期间有效，立即深拷贝
    }
    if (std::string(topic) == kTopicCaptureDone) {
        onCaptureDone(payload);
    } else if (std::string(topic) == kTopicCaptureFailed) {
        onCaptureFailed(payload);
    }
    // 其它主题安静忽略：事件总线是共享的，收到不认识的主题不是错误。
}

const char* NotifyToast::Impl::localized(const char* key, const char* fallback) const {
    // i18n 是可选对象：初始化失败或老宿主没有语言表时用中文原文兜底，
    // 通知永远有内容可显示（fallback 必须保持中文，见 I18N 约定）。
    return (i18n != nullptr) ? i18n->tr(key, fallback) : fallback;
}

void NotifyToast::Impl::onCaptureDone(const std::string& payload) {
    if (config == nullptr || !config->enabled() || !config->showOnSuccess()) {
        shot::Logger(host).debug("[notify] enabled/show_on_success=0，跳过成功提示");
        return;
    }
    std::string path;
    const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_discarded() && doc.is_object()) {
        path = jsonString(doc, "path");
    } else {
        shot::Logger(host).warn("[notify] capture.done 负载不是合法 JSON 对象，只显示通用提示");
    }

    // 标题/正文沿用 Windows 版的中文措辞；path 是保存插件给的原文，原样展示，
    // 不做二次翻译。D-Bus 正文没有 256 字符的定长缓冲，所以这里不截断路径。
    const std::string title = localized("toast.success_title", "截屏成功");
    const std::string body = path.empty()
                                 ? localized("toast.no_path", "已保存（保存插件未回报路径）")
                                 : std::string(localized("toast.saved_prefix", "已保存到 ")) + path;
    shot::Logger(host).info("[notify] capture.done：" + (path.empty() ? std::string("<无路径>") : path));
    showNotification(title, body, /*failure=*/false, config->soundEnabled());
}

void NotifyToast::Impl::onCaptureFailed(const std::string& payload) {
    if (config == nullptr || !config->enabled() || !config->showOnError()) {
        shot::Logger(host).debug("[notify] enabled/show_on_error=0，跳过失败提示");
        return;
    }
    std::string reason;
    const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_discarded() && doc.is_object()) {
        reason = jsonString(doc, "reason");
    }
    if (reason.empty()) {
        // capture.failed 的 reason 是宿主已经本地化好的文本，本插件只原样展示；
        // 只有整个字段缺失时才自己补一句（走本插件语言表）。
        reason = localized("toast.reason_unknown", "原因未知");
    }
    const std::string title = localized("toast.failed_title", "截屏失败");
    const std::string body = std::string(localized("toast.failed_prefix", "截图失败：")) + reason;
    shot::Logger(host).info("[notify] capture.failed：" + reason);
    showNotification(title, body, /*failure=*/true, config->soundEnabled());
}

void NotifyToast::Impl::showNotification(const std::string& title, const std::string& body,
                                         bool failure, bool sound) {
    if (!initialized) {
        return;
    }
    const char* icon = failure ? "dialog-warning" : "dialog-information";
    // 需要 <libnotify/notify.h> 的 NotifyNotification 与 glib 的 g_object_unref；
    // 对象只在本次调用内存活，show 返回后立即释放（libnotify 不保留回调引用）。
    NotifyNotification* notification = notify_notification_new(title.c_str(), body.c_str(), icon);
    if (notification == nullptr) {
        shot::Logger(host).warn("[notify] 创建通知对象失败（内存不足？）");
        return;
    }
    notify_notification_set_timeout(notification, kNotificationTimeoutMs);
    notify_notification_set_urgency(notification,
                                    failure ? NOTIFY_URGENCY_CRITICAL : NOTIFY_URGENCY_NORMAL);
    if (sound) {
        // libnotify 没有播放接口；sound-name 是 freedesktop 通知规范里的 hint，
        // 是否播放、播什么完全由桌面环境决定（有的守护进程直接忽略）。
        (void)notify_notification_set_hint_string(
            notification, "sound-name", failure ? "dialog-warning" : "message-new-instant");
    } else {
        // 用户关了 sound：请求通知守护进程不要播放默认提示音。同样是 hint，
        // 遵守与否由桌面决定，本插件不会自己 spawn 播放器（README 已写明）。
        (void)notify_notification_set_hint_byte(notification, "suppress-sound", 1);
    }
    GError* error = nullptr;
    if (!notify_notification_show(notification, &error)) {
        const std::string reason = (error != nullptr && error->message != nullptr)
                                       ? std::string(error->message)
                                       : std::string("未知错误");
        if (error != nullptr) {
            g_error_free(error);
        }
        shot::Logger(host).warn("[notify] 提交桌面通知失败：" + reason);
    } else {
        // 用户看不到通知时（系统关了通知）守护进程也可能返回成功，这里只能证明
        // "请求已交给桌面通知服务"。
        shot::Logger(host).info("[notify] 桌面通知已提交（libnotify）");
    }
    g_object_unref(notification);
}

}  // namespace notify
