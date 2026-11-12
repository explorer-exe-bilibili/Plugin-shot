// 长截图插件主体：生命周期、配置、事件订阅、触发键、会话线程与命令标志。
//
// 职责边界（STYLE.md §6.6/§6.8）：
//   · 本文件：start/stop、配置转发、事件订阅、命令标志、会话线程调度、保存与
//     capture.done 发布；
//   · long_capture_session.cpp：一次会话（找窗口 → HUD/气泡 → 连续抓帧 →
//     拼接 → 保存/取消）；
//   · stitch_canvas / scroll_stitcher / frame_loader / long_image_saver：平台无关算法；
//   · hud_windows / global_keys_windows / tray_hint_windows / window_target_windows：平台实现。
//
// 线程模型：
//   · 宿主 main 线程：create/destroy、set_config、事件回调（只置标志 + 唤醒）；
//   · 会话线程（本类持有）：等"开始"命令，跑一次会话；保存/取消/退出都是标志；
//   · HUD 线程、按键线程：各自的消息循环，回调只置原子标志（见对应平台实现）。
//
// 事件：
//   · 订阅 capture.done / capture.failed（会话线程等结果）、long_capture.start（托盘）、
//     app.quit（提前取消）；
//   · 发布 lists.add（托盘命令）、capture.request（每帧）、capture.done（最终长图）。

#include "long_capture.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "global_keys.h"
#include "hud.h"
#include "long_image_saver.h"
#include "plugin_entry.h"
#include "shot_plugin_helpers.h"
#include "tray_hint.h"

namespace long_capture {
namespace {

// 事件主题（与 document/CAPTURE.md、托盘条目、插件 README 保持一致）。
constexpr char kTopicCaptureRequest[] = "capture.request";
constexpr char kTopicCaptureDone[] = "capture.done";
constexpr char kTopicCaptureFailed[] = "capture.failed";
constexpr char kTopicStart[] = "long_capture.start";
constexpr char kTopicAppQuit[] = "app.quit";
constexpr char kTopicListsAdd[] = "lists.add";

// 托盘命令：id / label key / event / order 是需求约定的固定值，发布后不要改。
constexpr char kTrayCommandId[] = "long.capture";
constexpr char kTrayLabelKey[] = "capture.long";
constexpr int kTrayOrder = 5;

// 本插件的用户可见文案：托盘 label、配置项 label/tooltip、HUD 文案。
// 配置描述符（long_capture_config.cpp）里写的是这些 key，设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析；key 拼写必须一致。
// 每语言一张 JSON 对象，@meta 必填，两张表的 key 集必须一致；JSON 文本里不能有
// 裸换行，所以用相邻 raw 字面量拼接——拼出来仍是一行。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("capture.long":"长截图",)json"
    R"json("plugin.name":"长截图",)json"
    R"json("long.config.hotkey.label":"触发快捷键",)json"
    R"json("long.config.hotkey.tooltip":)json"
    R"json("全局组合键，开始一次长截图；留空则只用托盘菜单",)json"
    R"json("long.config.max_height.label":"长图高度上限",)json"
    R"json("long.config.max_height.tooltip":)json"
    R"json("达到上限后停止拼接，仍需手动保存；单位像素",)json"
    R"json("long.hud.title":"长截图",)json"
    R"json("long.hud.waiting":"等待画面…",)json"
    R"json("long.hud.status":"帧 {frames} · 高度 {height} px",)json"
    R"json("long.hud.gaps":"缺口 {gaps} 处",)json"
    R"json("long.hud.duplicates":"重复帧 {duplicates}",)json"
    R"json("long.hud.limit":"已达高度上限 {height} px，Enter 保存",)json"
    R"json("long.hud.save":"保存",)json"
    R"json("long.hud.cancel":"取消",)json"
    R"json("long.hud.saved":"已保存：{path}",)json"
    R"json("long.hud.failed":"保存失败：{reason}",)json"
    R"json("long.hud.cancelled":"已取消",)json"
    R"json("long.tray.hint_title":"长截图已开始",)json"
    R"json("long.tray.hint_text":"请滚动窗口内容；Enter 保存 / Esc 取消",)json"
    R"json("long.tray.failed_title":"长截图未能开始",)json"
    R"json("long.tray.error_title":"长截图失败",)json"
    R"json("long.tray.saved_title":"长截图已保存",)json"
    R"json("long.tray.limit_text":"已达到高度上限，Enter 保存 / Esc 取消"})json",
    R"json({"@meta":"en-US",)json"
    R"json("capture.long":"Long capture",)json"
    R"json("plugin.name":"Long capture",)json"
    R"json("long.config.hotkey.label":"Trigger hotkey",)json"
    R"json("long.config.hotkey.tooltip":)json"
    R"json("Global hotkey that starts a long capture; leave empty to use the tray menu",)json"
    R"json("long.config.max_height.label":"Maximum image height",)json"
    R"json("long.config.max_height.tooltip":)json"
    R"json("Stop stitching at this height (pixels); you still save manually",)json"
    R"json("long.hud.title":"Long capture",)json"
    R"json("long.hud.waiting":"Waiting for frames…",)json"
    R"json("long.hud.status":"Frames {frames} · {height} px",)json"
    R"json("long.hud.gaps":"{gaps} gap(s)",)json"
    R"json("long.hud.duplicates":"Duplicates {duplicates}",)json"
    R"json("long.hud.limit":"Height limit reached ({height} px); Enter to save",)json"
    R"json("long.hud.save":"Save",)json"
    R"json("long.hud.cancel":"Cancel",)json"
    R"json("long.hud.saved":"Saved: {path}",)json"
    R"json("long.hud.failed":"Save failed: {reason}",)json"
    R"json("long.hud.cancelled":"Cancelled",)json"
    R"json("long.tray.hint_title":"Long capture started",)json"
    R"json("long.tray.hint_text":"Scroll the window; Enter to save, Esc to cancel",)json"
    R"json("long.tray.failed_title":"Long capture could not start",)json"
    R"json("long.tray.error_title":"Long capture failed",)json"
    R"json("long.tray.saved_title":"Long capture saved",)json"
    R"json("long.tray.limit_text":"Height limit reached; Enter to save, Esc to cancel"})json",
};

}  // namespace

LongCapture::LongCapture() = default;

LongCapture::~LongCapture() { stop(); }

bool LongCapture::start(const ShotHostServices* host) {
    if (m_worker.joinable()) {
        return true;  // 已经在跑：重复 start 是幂等空操作
    }
    m_host = host;

    // 语言表：必须在 HUD/会话线程使用 tr 之前加载。失败不致命，tr 的 fallback 是中文。
    try {
        auto i18n = std::make_unique<shot::I18n>(host, kLongCapture.id);
        if (!i18n->loadTables(kLangTables, std::size(kLangTables))) {
            shot::Logger(host).warn("[long_capture] 语言表解析失败，界面文案回退为 key/中文");
        }
        m_i18n = std::move(i18n);
    } catch (...) {
        m_i18n.reset();
        shot::Logger(host).warn("[long_capture] 初始化多语言失败，界面文案回退为中文");
    }

    // 全局按键：触发键常驻；会话内 Enter/Esc 由 beginSession/endSession 管理。
    try {
        auto keys = std::make_unique<GlobalKeys>();
        keys->setTriggerCallback(&LongCapture::onTriggerKey, this);
        if (keys->start(host)) {
            m_keys = std::move(keys);
        } else {
            shot::Logger(host).warn("[long_capture] 全局按键线程启动失败，只能从托盘菜单触发");
        }
    } catch (...) {
        m_keys.reset();
        shot::Logger(host).warn("[long_capture] 初始化全局按键失败，只能从托盘菜单触发");
    }

    // 事件订阅：先订阅再起会话线程，避免启动阶段漏掉事件。
    shot::EventBus bus(host);
    if (bus.available()) {
        bool subscribed = bus.subscribe(kTopicCaptureDone, &LongCapture::onBusEvent, this);
        subscribed = bus.subscribe(kTopicCaptureFailed, &LongCapture::onBusEvent, this) || subscribed;
        subscribed = bus.subscribe(kTopicStart, &LongCapture::onBusEvent, this) || subscribed;
        subscribed = bus.subscribe(kTopicAppQuit, &LongCapture::onBusEvent, this) || subscribed;
        m_busSubscribed = subscribed;
        if (!subscribed) {
            shot::Logger(host).warn(
                "[long_capture] 订阅事件失败（capture.done / capture.failed / long_capture.start）");
        }
    } else {
        shot::Logger(host).warn("[long_capture] 宿主未提供事件总线（旧版宿主？），长截图不可用");
    }

    // 托盘命令：id long.capture / label key capture.long / event long_capture.start / order 5。
    nlohmann::json command;
    command["list"] = "tray_command_list";
    command["owner"] = kLongCapture.id;
    command["items"] = nlohmann::json::array();
    command["items"].push_back({{"id", kTrayCommandId},
                                {"label", kTrayLabelKey},
                                {"event", kTopicStart},
                                {"data", ""},
                                {"order", kTrayOrder}});
    if (!bus.publish(kTopicListsAdd, command.dump())) {
        shot::Logger(host).warn("[long_capture] 发布托盘命令失败（事件队列满或宿主太旧）");
    }

    // 按当前配置注册触发键；set_config 之后还会按新值重注册。
    if (m_keys) {
        const LongCaptureConfig::Snapshot snapshot = m_config.snapshot();
        m_keys->registerTrigger(snapshot.hotkey.c_str());
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = true;
        m_startPending = false;
        m_sessionActive = false;
        m_saveRequested = false;
        m_cancelRequested = false;
        m_captureReady = false;
        m_capture = CaptureResult{};
    }
    try {
        m_worker = std::thread(&LongCapture::run, this);
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        shot::Logger(host).error("[long_capture] 创建会话线程失败，长截图不可用");
        return false;
    }
    shot::Logger(host).info("[long_capture] 已就绪（托盘命令 long.capture）");
    return true;
}

void LongCapture::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        m_cancelRequested = true;
        m_cv.notify_all();
    }
    // 先 join 会话线程：它可能正在用 m_keys / m_i18n / m_trayHint，join 之后再拆。
    if (m_worker.joinable()) {
        m_worker.join();
    }
    if (m_keys) {
        m_keys->stop();
        m_keys.reset();
    }
    if (m_busSubscribed) {
        shot::EventBus bus(m_host);
        bus.unsubscribe(this);
        m_busSubscribed = false;
    }
    m_trayHint.reset();
    m_i18n.reset();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_startPending = false;
        m_sessionActive = false;
        m_saveRequested = false;
        m_cancelRequested = false;
        m_captureReady = false;
        m_capture = CaptureResult{};
    }
    m_host = nullptr;
}

std::uint32_t LongCapture::configCount() const noexcept { return LongCaptureConfig::itemCount(); }

std::int32_t LongCapture::configDesc(std::uint32_t index, ShotConfigDesc* out) const noexcept {
    return LongCaptureConfig::describe(index, out);
}

std::int32_t LongCapture::getConfig(const char* key, char* buf, std::uint32_t bufSize) const {
    return m_config.get(key, buf, bufSize);
}

std::int32_t LongCapture::setConfig(const char* key, const char* value) {
    const std::int32_t result = m_config.set(key, value);
    if (result == 0 && key != nullptr && std::strcmp(key, "hotkey") == 0 && m_keys) {
        // 热键变化立即按新值重注册；语法错/被占用只记 WARN，不影响其它配置项。
        const LongCaptureConfig::Snapshot snapshot = m_config.snapshot();
        m_keys->registerTrigger(snapshot.hotkey.c_str());
    }
    return result;
}

void LongCapture::onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len) {
    auto* self = static_cast<LongCapture*>(user);
    if (self != nullptr) {
        self->handleBusEvent(topic, static_cast<const char*>(data), len);
    }
}

void LongCapture::onTriggerKey(void* user) {
    auto* self = static_cast<LongCapture*>(user);
    if (self != nullptr) {
        self->requestStart();
    }
}

void LongCapture::onSaveKey(void* user) {
    auto* self = static_cast<LongCapture*>(user);
    if (self != nullptr) {
        self->requestSave();
    }
}

void LongCapture::onCancelKey(void* user) {
    auto* self = static_cast<LongCapture*>(user);
    if (self != nullptr) {
        self->requestCancel();
    }
}

void LongCapture::onHudSave(void* user) {
    auto* self = static_cast<LongCapture*>(user);
    if (self != nullptr) {
        self->requestSave();
    }
}

void LongCapture::onHudCancel(void* user) {
    auto* self = static_cast<LongCapture*>(user);
    if (self != nullptr) {
        self->requestCancel();
    }
}

void LongCapture::handleBusEvent(const char* topic, const char* data, std::uint32_t len) {
    if (topic == nullptr) {
        return;
    }
    if (std::strcmp(topic, kTopicStart) == 0) {
        requestStart();
        return;
    }
    if (std::strcmp(topic, kTopicAppQuit) == 0) {
        requestCancel();
        return;
    }
    if (std::strcmp(topic, kTopicCaptureDone) != 0 && std::strcmp(topic, kTopicCaptureFailed) != 0) {
        return;
    }

    const bool failedTopic = std::strcmp(topic, kTopicCaptureFailed) == 0;
    CaptureResult result;
    result.ready = true;
    result.failed = failedTopic;

    if (data == nullptr || len == 0) {
        result.failed = true;
        result.reason = failedTopic ? "采集失败（宿主未给出原因）" : "抓帧结果为空";
    } else {
        const auto parsed = nlohmann::json::parse(data, data + len, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            result.failed = true;
            result.reason = "抓帧结果负载不是合法 JSON 对象";
        } else if (failedTopic) {
            const auto reason = parsed.find("reason");
            result.reason = (reason != parsed.end() && reason->is_string())
                                ? reason->get<std::string>()
                                : "采集失败（宿主未给出原因）";
        } else {
            const auto path = parsed.find("path");
            if (path != parsed.end() && path->is_string()) {
                result.path = path->get<std::string>();
            } else {
                result.failed = true;
                result.reason = "抓帧结果缺少 path";
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 只接受会话期间的结果：别的来源的 capture.done 不能被当前会话当成自己的帧。
        if (!m_sessionActive) {
            return;
        }
        m_capture = std::move(result);
        m_captureReady = true;
        m_cv.notify_all();
    }
}

void LongCapture::requestStart() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running || m_sessionActive || m_startPending) {
        return;
    }
    m_startPending = true;
    m_cv.notify_all();
}

void LongCapture::requestSave() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sessionActive) {
        return;
    }
    m_saveRequested = true;
    m_cv.notify_all();
}

void LongCapture::requestCancel() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cancelRequested = true;
    m_cv.notify_all();
}

void LongCapture::wake() { m_cv.notify_all(); }

void LongCapture::run() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return !m_running || m_startPending; });
            if (!m_running) {
                return;
            }
            m_startPending = false;
            m_sessionActive = true;
            m_saveRequested = false;
            m_cancelRequested = false;
            m_captureReady = false;
            m_capture = CaptureResult{};
        }

        try {
            runSession();
        } catch (const std::exception& error) {
            shot::Logger(m_host).error(std::string("[long_capture] 会话异常：") + error.what());
        } catch (...) {
            shot::Logger(m_host).error("[long_capture] 会话异常（未知类型）");
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sessionActive = false;
            m_saveRequested = false;
            m_cancelRequested = false;
            m_captureReady = false;
            m_capture = CaptureResult{};
        }
    }
}

bool LongCapture::waitCapture(CaptureResult& result, int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_mutex);
    const bool signaled = m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
        return m_captureReady || m_saveRequested || m_cancelRequested || !m_running;
    });
    if (!signaled) {
        return false;
    }
    if (m_saveRequested || m_cancelRequested || !m_running) {
        return false;  // 命令优先：用户已经按了保存/取消，不要再处理这一帧
    }
    if (!m_captureReady) {
        return false;
    }
    result = m_capture;
    m_captureReady = false;
    return true;
}

void LongCapture::clearCaptureResult() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_captureReady = false;
    m_capture = CaptureResult{};
}

bool LongCapture::waitCommand(int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
        return m_saveRequested || m_cancelRequested || !m_running;
    });
}

bool LongCapture::saveCanvas(const StitchCanvas& canvas, std::string& path, std::string& error) {
    error.clear();
    path.clear();
    if (canvas.empty()) {
        error = "画布是空的，没有可保存的内容";
        return false;
    }
    const SaveOutcome outcome = LongImageSaver::save(canvas, configuredSaveDirectory());
    if (!outcome.ok) {
        error = outcome.error;
        return false;
    }
    path = outcome.path;

    nlohmann::json payload;
    payload["path"] = path;
    payload["width"] = canvas.width();
    payload["height"] = canvas.height();
    payload["processors"] = 0;
    const std::string text = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (!shot::EventBus(m_host).publish(kTopicCaptureDone, text)) {
        // 文件已经落盘；事件发布失败只影响通知/剪贴板等消费者，不把保存判失败。
        shot::Logger(m_host).warn(
            "[long_capture] 保存成功，但发布 capture.done 失败（事件队列满？）");
    }
    return true;
}

std::string LongCapture::configuredSaveDirectory() const {
    shot::PluginLists lists(m_host);
    if (!lists.available()) {
        return {};
    }
    const std::string text = lists.json("config_list");
    if (text.empty()) {
        return {};
    }
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return {};
    }
    for (const auto& item : parsed) {
        if (!item.is_object()) {
            continue;
        }
        const auto owner = item.find("owner");
        const auto id = item.find("id");
        const auto value = item.find("value");
        if (owner == item.end() || id == item.end() || value == item.end() ||
            !owner->is_string() || !id->is_string() || !value->is_string()) {
            continue;
        }
        // 保存目录以 image_saver 插件的配置为准；读不到时交给 LongImageSaver 退回 Pictures。
        if (owner->get<std::string>() == "image_saver.main" &&
            id->get<std::string>() == "directory") {
            return value->get<std::string>();
        }
    }
    return {};
}

std::string LongCapture::localized(const char* key, const char* fallback) const {
    const char* text = (fallback != nullptr) ? fallback : "";
    if (key != nullptr && m_i18n) {
        text = m_i18n->tr(key, text);
    }
    return std::string(text);
}

void LongCapture::showTrayHint(const char* titleKey, const char* titleFallback,
                               const char* textKey, const char* textFallback) const {
    try {
        if (!m_trayHint) {
            m_trayHint = std::make_unique<TrayHint>();
        }
        const std::string title = localized(titleKey, titleFallback);
        const std::string text = (textKey != nullptr) ? localized(textKey, textFallback)
                                                      : std::string(textFallback ? textFallback : "");
        if (!m_trayHint->show(m_host, title, text)) {
            shot::Logger(m_host).debug("[long_capture] 托盘气泡显示失败（explorer 未运行？）");
        }
    } catch (...) {
        shot::Logger(m_host).warn("[long_capture] 托盘气泡创建异常，已忽略");
    }
}

bool LongCapture::resolveTarget(TargetWindow& target, std::string& reason) const {
    return WindowTarget::findForeground(target, reason);
}

bool LongCapture::publishCaptureRequest(const ScreenRect& client) const {
    int originX = 0;
    int originY = 0;
    WindowTarget::virtualDesktopOrigin(originX, originY);
    nlohmann::json region;
    region["x"] = client.x - originX;
    region["y"] = client.y - originY;
    region["width"] = client.width;
    region["height"] = client.height;
    nlohmann::json payload;
    payload["mode"] = "region";
    payload["region"] = region;
    const std::string text = payload.dump();
    const bool published = shot::EventBus(m_host).publish(kTopicCaptureRequest, text);
    if (!published) {
        shot::Logger(m_host).warn("[long_capture] 发布 capture.request 失败（宿主事件队列满？）");
    }
    return published;
}

}  // namespace long_capture
