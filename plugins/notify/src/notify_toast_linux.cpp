// Linux 桌面通知：libnotify（D-Bus org.freedesktop.Notifications）。
//
// 与 Windows 版的差异只在"怎么显示"与"怎么收点击"，事件订阅、配置项、文案 key 完全一致：
//   · 不用隐藏窗口/占位图标：libnotify 直接把通知交给桌面通知守护进程；
//   · 显示通知不起线程：show 是一次短 D-Bus 调用，在宿主分发线程同步提交即可；
//   · 点击动作必须有一个 GLib 主循环：ActionInvoked 是 libnotify 通过 GLib 主上下文
//     分发的信号，而宿主的主循环不是 GLib 的。所以本插件起一个只跑 GMainLoop 的
//     后台线程（见 Impl::startActionLoop），stop() 里先 quit + join 再释放 libnotify；
//   · 声音：libnotify 没有"播放声音"接口，只能附 sound-name hint；而 hint 只是
//     给守护进程的建议（GNOME/KDE 可能播放、dunst 可能忽略），本插件绝不 spawn
//     外部播放器——README 已写明"Linux 上不保证有声"。
//
// 应用名编码：通知头部的应用名不是我们拼的文本，而是桌面通知服务按 D-Bus 发送方/
// `.desktop` 关联推断的；没有关联时它会回退到窗口标题（本地化的中文），并按 Latin-1
// 渲染成乱码。所以每条通知都显式 `notify_notification_set_app_name("Plugin-shot")` +
// `desktop-entry` hint（见 README §4.3），标题/正文仍然是正常 UTF-8。
//
// 失败降级：没有 D-Bus 会话、没有通知守护进程、用户关了通知，show 返回 FALSE
// 或静默成功；本插件只记 WARN，不影响截屏本身（文件已经落盘）。

#include "notify_toast.h"

#include <cstdint>
#include <memory>
#include <new>
#include <string>

#include <glib.h>
#include <libnotify/notify.h>
#include <nlohmann/json.hpp>

#include "notify_config.h"
#include "notify_title.h"
#include "notify_toast_linux_impl.h"
#include "shot_plugin_helpers.h"

namespace notify {
namespace {

// 通知停留时长（毫秒）：与 Windows 的 5s 对上；桌面守护进程可以忽略这个值。
constexpr int kNotificationTimeoutMs = 5000;

/**
 * @brief 一次通知动作要打开的目标；由通知对象销毁时通过 GFreeFunc 释放。
 *
 * 为什么把路径拷一份而不是引用 NotifyConfig/Impl：动作回调可能在 GLib 主循环线程上
 * 晚于本次 show 很久才触发，指针必须独立有效。这里只放纯数据 + 宿主服务表指针
 * （生命周期由宿主保证，stop() 之前一定有效）。
 */
struct ClickTarget {
    std::string path;                        /**< 图片绝对路径（UTF-8）。 */
    bool folder = false;                     /**< true = 打开所在文件夹；false = 打开图片。 */
    const ShotHostServices* host = nullptr;  /**< 只用于写日志。 */
};

/** @brief 释放 ClickTarget；作为 notify_notification_add_action 的 free_func。 */
void freeClickTarget(gpointer data) {
    delete static_cast<ClickTarget*>(data);
}

/** @brief 从 JSON 负载里读一个字符串字段；缺失/类型不对返回空串。 */
std::string jsonString(const nlohmann::json& object, const char* field) {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

/** @brief 从 JSON 负载里读一个整数（宽/高）；缺失/类型不对返回 0（= 未知）。 */
int jsonInt(const nlohmann::json& object, const char* field) {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_number_integer()) {
        return 0;
    }
    return it->get<int>();
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
    // 通知头部的应用名：libnotify 在缺少 .desktop 关联时会回退到进程/窗口标题，而我们的
    // 窗口标题是本地化的中文（如「Plugin-shot 设置」），桌面通知服务按 Latin-1 渲染就会
    // 变成乱码（实测 Ubuntu 24 GNOME：`Plugin-shot è®¾ç½®`）。这里先设一个 GLib 全局
    // ASCII 应用名兜底，showNotification 里还会给每条通知显式设 app_name + desktop-entry。
    g_set_application_name("Plugin-shot");

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
    // 通知点击需要 GLib 主循环（ActionInvoked 信号靠它分发）。起不来不影响显示，
    // 只是点了没反应，所以失败只 WARN。
    m_impl->startActionLoop();
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
    // 再停 GLib 主循环：动作/关闭回调都在那个线程上跑，join 之后本插件不会再收到
    // 回调，随后释放通知对象与 libnotify 才是安全的（DLL 卸载不留悬空函数指针）。
    m_impl->stopActionLoop();
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
    int width = 0;
    int height = 0;
    const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_discarded() && doc.is_object()) {
        path = jsonString(doc, "path");
        width = jsonInt(doc, "width");
        height = jsonInt(doc, "height");
    } else {
        shot::Logger(host).warn("[notify] capture.done 负载不是合法 JSON 对象，只显示通用提示");
    }

    // 标题：配置了 title 就用模板展开结果，否则用默认多语言标题；路径/宽高只对成功
    // 通知有意义（失败通知传入空路径与 0）。body 里的 path 是保存插件给的原文，
    // 原样展示、不做二次翻译；D-Bus 正文没有 256 字符的定长缓冲，所以这里不截断。
    std::string title = expandTitle(config->title(), path, width, height);
    if (title.empty()) {
        title = localized("toast.success_title", "截屏成功");
    }
    const std::string body = path.empty()
                                 ? localized("toast.no_path", "已保存（保存插件未回报路径）")
                                 : std::string(localized("toast.saved_prefix", "已保存到 ")) + path;
    shot::Logger(host).info("[notify] capture.done：" +
                            (path.empty() ? std::string("<无路径>") : path));
    // 失败通知没有图片路径，所以点击动作只挂在成功通知上；没有路径时传空串。
    const bool openFolder = config->clickAction() == ClickAction::OpenFolder;
    showNotification(title, body, /*failure=*/false, config->soundEnabled(), path, openFolder);
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
    std::string title = expandTitle(config->title(), std::string(), 0, 0);
    if (title.empty()) {
        title = localized("toast.failed_title", "截屏失败");
    }
    const std::string body = std::string(localized("toast.failed_prefix", "截图失败：")) + reason;
    shot::Logger(host).info("[notify] capture.failed：" + reason);
    showNotification(title, body, /*failure=*/true, config->soundEnabled(), std::string(),
                     /*openFolder=*/false);
}

void NotifyToast::Impl::showNotification(const std::string& title, const std::string& body,
                                         bool failure, bool sound, const std::string& imagePath,
                                         bool openFolder) {
    if (!initialized) {
        return;
    }
    const char* icon = failure ? "dialog-warning" : "dialog-information";
    // 需要 <libnotify/notify.h> 的 NotifyNotification 与 glib 的 g_object_unref。
    NotifyNotification* notification = notify_notification_new(title.c_str(), body.c_str(), icon);
    if (notification == nullptr) {
        shot::Logger(host).warn("[notify] 创建通知对象失败（内存不足？）");
        return;
    }
    // 通知头部（应用名）必须是 ASCII 且稳定：显式设置 app_name，避免 libnotify 回退到
    // 本地化窗口标题；再附 desktop-entry hint，桌面上有 plugin-shot.desktop 时 GNOME/KDE
    // 会用它取图标与显示名（没有这个文件也只是被忽略，不影响通知本身）。
    notify_notification_set_app_name(notification, "Plugin-shot");
    (void)notify_notification_set_hint_string(notification, "desktop-entry", "plugin-shot");
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

    // 点击动作：freedesktop 约定动作名 "default" = 点通知正文本身；目标路径与动作
    // 类型拷进 ClickTarget，通知销毁时由 freeClickTarget 释放，回调不依赖本对象。
    bool actionAdded = false;
    if (!imagePath.empty()) {
        auto* target = new (std::nothrow) ClickTarget{imagePath, openFolder, host};
        if (target != nullptr) {
            const char* labelKey =
                NotifyConfig::clickActionKey(openFolder ? ClickAction::OpenFolder
                                                        : ClickAction::OpenImage);
            const char* label = localized(labelKey, openFolder ? "打开所在文件夹" : "打开图片");
            (void)notify_notification_add_action(notification, "default", label,
                                                 &Impl::onActionInvoked, target, freeClickTarget);
            actionAdded = true;
        } else {
            shot::Logger(host).warn("[notify] 为通知申请点击目标失败，本条通知不可点击");
        }
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

    if (actionAdded) {
        // 带动作的通知必须活到用户点击/关闭为止（动作回调的 user_data 与信号都挂在
        // 它上面），所以把创建时的引用交给"关闭时自释放"，并登记到 pending；
        // stop() 时对还没关闭的做兜底释放。
        trackPending(notification);
    } else {
        // 没有动作：没有任何回调会引用它，show 返回后立即释放。
        g_object_unref(notification);
    }
}

void NotifyToast::Impl::startActionLoop() {
    // 为什么必须有这个线程：libnotify 的 ActionInvoked / NotificationClosed 是 GLib
    // 主上下文里的信号，没人跑主循环就永远收不到"用户点了通知"。宿主自己的主循环
    // 不是 GLib 的，所以只能由本插件提供这一个最小循环。
    try {
        loopThread = std::thread(&Impl::runActionLoop, this);
    } catch (...) {
        shot::Logger(host).warn(
            "[notify] 启动 GLib 主循环线程失败，通知点击动作不可用（通知照常显示）");
    }
}

void NotifyToast::Impl::runActionLoop() {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    if (loop == nullptr) {
        shot::Logger(host).warn("[notify] 创建 GLib 主循环失败，通知点击动作不可用");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(loopMutex);
        mainLoop = loop;
    }
    // 默认 GLib 上下文：libnotify 在 notify_init / show 时把信号回调挂在这里。
    g_main_loop_run(loop);
    {
        // 先摘掉指针再释放：stop() 在 loopMutex 内调 g_main_loop_quit，两者不会
        // 同时碰到已释放的 GMainLoop。
        std::lock_guard<std::mutex> lock(loopMutex);
        if (mainLoop == loop) {
            mainLoop = nullptr;
        }
    }
    g_main_loop_unref(loop);
}

void NotifyToast::Impl::stopActionLoop() {
    {
        std::lock_guard<std::mutex> lock(loopMutex);
        if (mainLoop != nullptr) {
            g_main_loop_quit(mainLoop);  // 线程安全：唤醒 g_main_loop_run
        }
    }
    if (loopThread.joinable()) {
        loopThread.join();  // join 之后不会再有动作/关闭回调进入本插件
    }
    releasePending();
}

void NotifyToast::Impl::trackPending(NotifyNotification* notification) {
    if (notification == nullptr) {
        return;
    }
    // 引用不是"额外加一次"：创建时的那个引用直接转交给通知自己，关闭信号里释放。
    g_signal_connect(notification, "closed", G_CALLBACK(&Impl::onNotificationClosed), this);
    std::lock_guard<std::mutex> lock(pendingMutex);
    pending.push_back(notification);
}

bool NotifyToast::Impl::removePending(NotifyNotification* notification) {
    std::lock_guard<std::mutex> lock(pendingMutex);
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        if (*it == notification) {
            pending.erase(it);
            return true;
        }
    }
    return false;
}

void NotifyToast::Impl::releasePending() {
    std::vector<NotifyNotification*> local;
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        local.swap(pending);
    }
    // 循环线程已经 join，这里不会与关闭回调并发。
    for (NotifyNotification* notification : local) {
        g_object_unref(notification);
    }
}

void NotifyToast::Impl::onNotificationClosed(NotifyNotification* notification, gpointer userData) {
    auto* self = static_cast<Impl*>(userData);
    if (self == nullptr || notification == nullptr) {
        return;
    }
    // 只有登记过的才释放：防止同一通知重复收到 closed 时二次 unref。
    if (self->removePending(notification)) {
        g_object_unref(notification);
    }
}

void NotifyToast::Impl::onActionInvoked(NotifyNotification* notification, char* action,
                                        gpointer userData) {
    (void)notification;
    (void)action;  // 本插件只注册了一个 "default" 动作
    auto* target = static_cast<ClickTarget*>(userData);
    if (target == nullptr || target->path.empty()) {
        return;
    }
    if (!g_file_test(target->path.c_str(), G_FILE_TEST_EXISTS)) {
        // 图片可能已被用户/清理工具删掉：只 WARN，不弹任何错误提示。
        shot::Logger(target->host).warn("[notify] 通知被点击，但图片已不存在：" + target->path);
        return;
    }
    // 打开所在文件夹时把路径换成父目录（xdg-open 打开目录 = 文件管理器）。
    std::string openPath = target->path;
    if (target->folder) {
        gchar* directory = g_path_get_dirname(target->path.c_str());
        if (directory != nullptr) {
            openPath = directory;
            g_free(directory);
        }
    }
    // xdg-open 由桌面环境决定用哪个程序（看图器/文件管理器）；用 g_spawn_async 而不是
    // system()：不经过 shell、不用自己拼引号，子进程由 GLib 负责回收。
    gchar* argv[] = {const_cast<gchar*>("xdg-open"), const_cast<gchar*>(openPath.c_str()),
                     nullptr};
    GError* error = nullptr;
    const gboolean spawned =
        g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error);
    if (!spawned) {
        const std::string reason =
            (error != nullptr && error->message != nullptr) ? std::string(error->message)
                                                           : std::string("未知错误");
        if (error != nullptr) {
            g_error_free(error);
        }
        shot::Logger(target->host).warn("[notify] 打开目标失败：" + reason);
        return;
    }
    shot::Logger(target->host).info("[notify] 已打开" +
                                    std::string(target->folder ? "所在文件夹：" : "图片：") + openPath);
}

}  // namespace notify
