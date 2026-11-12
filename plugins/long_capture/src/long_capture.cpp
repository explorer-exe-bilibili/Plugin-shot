// 长截图插件主体：生命周期、配置、事件订阅、会话线程与命令标志。
//
// 职责边界（STYLE.md §6.6/§6.8）：
//   · 本文件：start/stop、配置转发、事件订阅、命令标志、会话线程调度、
//     region.select.request / region.selected 握手；
//   · long_capture_session.cpp：一次会话（等选择 → HUD/气泡 → 自带采集 → 拼接 → 保存/取消）；
//   · long_capture_output.cpp：保存、config_list 目录解析、文案与托盘气泡；
//   · long_capture_i18n.cpp：中英文语言表；
//   · stitch_canvas / scroll_stitcher / long_image_saver：平台无关算法；
//   · capture_windows / hud_windows / global_keys_windows / tray_hint_windows：平台实现。
//
// 线程模型：
//   · 宿主 main 线程：create/destroy、set_config、事件回调（只置标志 + 唤醒）；
//   · 会话线程（本类持有）：等"开始"命令，跑一次会话；保存/取消/退出都是标志；
//   · HUD 线程、按键线程：各自的消息循环，回调只置原子标志（见对应平台实现）。
//
// 事件：
//   · 订阅 region.selected（选择结果）、long_capture.start（托盘/hotkey）、app.quit；
//   · 发布 lists.add（托盘命令 + hotkey_list 贡献）、region.select.request、
//     capture.done（只在最终保存时发一条）。

#include "long_capture.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "capture_windows.h"
#include "global_keys.h"
#include "long_capture_i18n.h"
#include "plugin_entry.h"
#include "shot_plugin_helpers.h"
#include "tray_hint.h"

namespace long_capture {
namespace {

// 事件主题（与 document/CAPTURE.md、document/EVENT_BUS.md、插件 README 保持一致）。
constexpr char kTopicStart[] = "long_capture.start";
constexpr char kTopicAppQuit[] = "app.quit";
constexpr char kTopicListsAdd[] = "lists.add";
constexpr char kTopicRegionSelectRequest[] = "region.select.request";
constexpr char kTopicRegionSelected[] = "region.selected";

// 选择握手：本插件的 purpose 标识；region_selector 原样回传，只有匹配才启动会话。
constexpr char kSelectionPurpose[] = "long_capture";

// 等待用户选择目标窗口的上限：超时回到空闲并提示，避免会话线程永远挂着。
constexpr int kSelectionTimeoutMs = 60000;

// 托盘命令：id / label key / event / order 是需求约定的固定值，发布后不要改。
constexpr char kTrayCommandId[] = "long.capture";
constexpr char kTrayLabelKey[] = "capture.long";
constexpr int kTrayOrder = 5;

// hotkey_list 贡献：长期热键由 hotkey 插件统一注册，本插件只声明"我要这个组合键"。
// combo 是默认值，用户在设置页改键后由 hotkey 插件持久化，不写回这里。
constexpr char kHotkeyId[] = "start";
constexpr char kHotkeyCombo[] = "Ctrl+Alt+L";
constexpr char kHotkeyLabelKey[] = "long.config.hotkey.label";

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
        std::size_t tableCount = 0;
        const char* const* tables = languageTables(tableCount);
        if (!i18n->loadTables(tables, tableCount)) {
            shot::Logger(host).warn("[long_capture] 语言表解析失败，界面文案回退为 key/中文");
        }
        m_i18n = std::move(i18n);
    } catch (...) {
        m_i18n.reset();
        shot::Logger(host).warn("[long_capture] 初始化多语言失败，界面文案回退为中文");
    }

    // 会话按键：只建隐藏窗口与消息线程，Enter/Esc 在 beginSession 时才注册。
    try {
        auto keys = std::make_unique<GlobalKeys>();
        if (keys->start(host)) {
            m_keys = std::move(keys);
        } else {
            shot::Logger(host).warn("[long_capture] 会话按键线程启动失败，只能用 HUD 按钮保存/取消");
        }
    } catch (...) {
        m_keys.reset();
        shot::Logger(host).warn("[long_capture] 初始化会话按键失败，只能用 HUD 按钮保存/取消");
    }

    // 事件订阅：先订阅再起会话线程，避免启动阶段漏掉事件。
    shot::EventBus bus(host);
    if (bus.available()) {
        bool subscribed = bus.subscribe(kTopicRegionSelected, &LongCapture::onBusEvent, this);
        subscribed = bus.subscribe(kTopicStart, &LongCapture::onBusEvent, this) || subscribed;
        subscribed = bus.subscribe(kTopicAppQuit, &LongCapture::onBusEvent, this) || subscribed;
        m_busSubscribed = subscribed;
        if (!subscribed) {
            shot::Logger(host).warn(
                "[long_capture] 订阅事件失败（region.selected / long_capture.start / app.quit）");
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

    // hotkey_list 贡献：hotkey 插件读这个列表后统一注册/改键/停用长期热键。
    // 本插件不再自己 RegisterHotKey，也不再有"触发快捷键"配置项。
    nlohmann::json hotkeys;
    hotkeys["list"] = "hotkey_list";
    hotkeys["owner"] = kLongCapture.id;
    hotkeys["items"] = nlohmann::json::array();
    hotkeys["items"].push_back({{"id", kHotkeyId},
                                {"combo", kHotkeyCombo},
                                {"event", kTopicStart},
                                {"data", ""},
                                {"label", kHotkeyLabelKey}});
    if (!bus.publish(kTopicListsAdd, hotkeys.dump())) {
        shot::Logger(host).warn("[long_capture] 发布 hotkey_list 失败（事件队列满或宿主太旧）");
    }

    // 设置页解析 hotkey_list 的 label 时用的 owner 是 provider（hotkey.main），本插件
    // 自己名下的 i18n 条目在那里查不到；按 i18n 协议补一条 target=hotkey.main 的译文
    // （用户语言包同样可以覆盖它，优先级规则不变）。
    nlohmann::json labelTexts;
    labelTexts["list"] = "i18n_source_list";
    labelTexts["owner"] = kLongCapture.id;
    labelTexts["items"] = nlohmann::json::array();
    labelTexts["items"].push_back(
        {{"id", kHotkeyLabelKey},
         {"target", "hotkey.main"},
         {"texts", {{"zh-CN", "长截图"}, {"en-US", "Long capture"}}}});
    if (!bus.publish(kTopicListsAdd, labelTexts.dump())) {
        shot::Logger(host).warn("[long_capture] 发布热键标签译文失败（设置页会显示 key）");
    }

    // 旧版把长期热键存在自己的配置项 long_capture.main/hotkey 里；现在归 hotkey.main
    // 统一管理。首次启动搬一次（新键已有值时不覆盖），否则用户原来的组合键会被重置。
    {
        shot::Config config(host);
        const std::string legacy = config.getString("long_capture.main/hotkey");
        if (!legacy.empty() && config.getString("hotkey.main/start").empty()) {
            if (config.setString("hotkey.main/start", legacy)) {
                shot::Logger(host).info("[long_capture] 旧热键已迁移到 hotkey.main/start");
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = true;
        m_startPending = false;
        m_sessionActive = false;
        m_saveRequested = false;
        m_cancelRequested = false;
        m_selectionReady = false;
        m_selectionCancelled = false;
        m_selectedRegion = ScreenRect{};
    }
    try {
        m_worker = std::thread(&LongCapture::run, this);
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        shot::Logger(host).error("[long_capture] 创建会话线程失败，长截图不可用");
        return false;
    }
    shot::Logger(host).info("[long_capture] 已就绪（托盘命令 long.capture / hotkey_list 已贡献）");
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
        m_selectionReady = false;
        m_selectionCancelled = false;
        m_selectedRegion = ScreenRect{};
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
    return m_config.set(key, value);
}

void LongCapture::onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len) {
    auto* self = static_cast<LongCapture*>(user);
    if (self != nullptr) {
        self->handleBusEvent(topic, static_cast<const char*>(data), len);
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
    if (std::strcmp(topic, kTopicRegionSelected) == 0) {
        handleRegionSelected(data, len);
        return;
    }
    // 其它主题安静忽略：事件总线是共享的，收到不认识的主题不是错误。
}

void LongCapture::handleRegionSelected(const char* data, std::uint32_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    const auto parsed = nlohmann::json::parse(data, data + len, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        shot::Logger(m_host).warn("[long_capture] region.selected 负载不是合法 JSON 对象，已忽略");
        return;
    }
    const auto purpose = parsed.find("purpose");
    if (purpose == parsed.end() || !purpose->is_string() ||
        purpose->get<std::string>() != kSelectionPurpose) {
        return;  // 不是本插件发起的选窗请求（共享总线上可能有别的 purpose）
    }
    const auto cancelled = parsed.find("cancelled");
    const bool isCancelled =
        cancelled != parsed.end() && cancelled->is_boolean() && cancelled->get<bool>();

    ScreenRect region;
    if (!isCancelled) {
        const auto regionIt = parsed.find("region");
        if (regionIt == parsed.end() || !regionIt->is_object()) {
            shot::Logger(m_host).warn("[long_capture] region.selected 缺少 region 对象，已忽略");
            return;
        }
        const auto readInt = [&regionIt](const char* key, std::int64_t& out) {
            const auto field = regionIt->find(key);
            if (field == regionIt->end() || !field->is_number_integer()) {
                return false;
            }
            out = field->get<std::int64_t>();
            return true;
        };
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t width = 0;
        std::int64_t height = 0;
        if (!readInt("x", x) || !readInt("y", y) || !readInt("width", width) ||
            !readInt("height", height) || width <= 0 || height <= 0) {
            shot::Logger(m_host).warn("[long_capture] region.selected 的 region 字段非法，已忽略");
            return;
        }
        // region_selector 发的是虚拟桌面坐标；BitBlt 要绝对屏幕坐标，这里加回原点。
        int originX = 0;
        int originY = 0;
        ScreenGrabber::virtualDesktopOrigin(originX, originY);
        region.x = static_cast<int>(x) + originX;
        region.y = static_cast<int>(y) + originY;
        region.width = static_cast<int>(width);
        region.height = static_cast<int>(height);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 只接受会话期间的结果：上一次会话迟到的 region.selected 不能被当成这一次的选择。
        if (!m_sessionActive) {
            return;
        }
        if (isCancelled) {
            m_selectionCancelled = true;
        } else {
            m_selectedRegion = region;
            m_selectionReady = true;
        }
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
            m_selectionReady = false;
            m_selectionCancelled = false;
            m_selectedRegion = ScreenRect{};
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
            m_selectionReady = false;
            m_selectionCancelled = false;
            m_selectedRegion = ScreenRect{};
        }
    }
}

bool LongCapture::waitCommand(int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
        return m_saveRequested || m_cancelRequested || !m_running;
    });
}

bool LongCapture::publishSelectRequest() const {
    nlohmann::json payload;
    payload["purpose"] = kSelectionPurpose;
    const std::string text = payload.dump();
    const bool published = shot::EventBus(m_host).publish(kTopicRegionSelectRequest, text);
    if (!published) {
        shot::Logger(m_host).warn("[long_capture] 发布 region.select.request 失败（宿主事件队列满？）");
    }
    return published;
}

bool LongCapture::awaitSelection(ScreenRect& region, std::string& error) {
    error.clear();
    if (!publishSelectRequest()) {
        error = "无法请求目标选择（事件总线不可用或队列满）";
        return false;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    const bool signaled = m_cv.wait_for(lock, std::chrono::milliseconds(kSelectionTimeoutMs),
                                        [this]() {
                                            return m_selectionReady || m_selectionCancelled ||
                                                   m_cancelRequested || !m_running;
                                        });
    if (!signaled) {
        lock.unlock();
        error = localized("long.tray.select_timeout", "60 秒内没有选择目标窗口，长截图已取消");
        showTrayHint("long.tray.failed_title", "长截图未能开始", nullptr, error.c_str());
        return false;
    }
    if (m_selectionCancelled) {
        lock.unlock();
        error = localized("long.tray.select_cancelled", "已取消目标选择");
        showTrayHint("long.tray.failed_title", "长截图未能开始", nullptr, error.c_str());
        return false;
    }
    if (m_cancelRequested || !m_running) {
        error = "会话已取消";
        return false;
    }
    region = m_selectedRegion;
    m_selectionReady = false;
    if (!region.valid()) {
        error = "选择的目标区域为空";
        return false;
    }
    return true;
}

}  // namespace long_capture
