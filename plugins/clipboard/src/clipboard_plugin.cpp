// 剪贴板插件的唯一职责：订阅 capture.done → 把刚落盘的文件复制进剪贴板。
//
// 为什么是"事件驱动"而不是"宿主调我"：像素不进事件总线（document/CAPTURE.md §3），
// capture.done 只带保存插件写回的文件路径，所以本插件从文件读图；这也意味着它
// 天然依赖"有保存插件先落盘"——没有 capture.done 或 path 为空时只记日志跳过。
//
// 线程模型：事件总线回调只在宿主分发线程（main）执行（document/EVENT_BUS.md §3），
// stb 解码 + 剪贴板写入也在这个回调里同步完成：一次截屏的图片通常只有几 MB，
// 解码是几十毫秒级，为此开线程不值得，而且会让"退订后还有线程在写剪贴板"的
// 生命周期更难保证（插件无自己的线程，destroy 返回后不留任何后台任务）。

#include "clipboard_plugin.h"

#include <string>

#include <nlohmann/json.hpp>

#include "shot_plugin_helpers.h"

namespace clipboard {
namespace {

// capture.done 的主题名：与 document/CAPTURE.md §3、tray 插件订阅的是同一个。
const char kCaptureDoneTopic[] = "capture.done";

}  // namespace

ClipboardPlugin::~ClipboardPlugin() {
    stop();  // 正常路径由 destroy 调；这里兜底，重复退订是空操作
}

bool ClipboardPlugin::start(const ShotHostServices* host) {
    m_host = host;
    shot::EventBus bus(host);
    if (!bus.subscribe(kCaptureDoneTopic, &ClipboardPlugin::onCaptureDone, this)) {
        // 老宿主没有事件总线：剪贴板功能不可用，但插件本身照常加载（安静降级，
        // 不把"少一个服务"升级成"插件加载失败"）。
        shot::Logger(host).warn("[clipboard] 订阅 capture.done 失败（宿主无事件总线），剪贴板功能不可用");
        m_subscribed = false;
        return false;
    }
    m_subscribed = true;
    shot::Logger(host).info("[clipboard] 已订阅 capture.done：截屏落盘后写进剪贴板");
    return true;
}

void ClipboardPlugin::stop() {
    if (!m_subscribed) {
        return;
    }
    // 退订句柄就是 this（subscribe 时的 user 指针）：一次摘掉本插件注册的全部主题。
    shot::EventBus bus(m_host);
    bus.unsubscribe(this);
    m_subscribed = false;
}

std::uint32_t ClipboardPlugin::configCount() const noexcept {
    return m_config.configCount();
}

std::int32_t ClipboardPlugin::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    return m_config.configDesc(index, out);
}

std::int32_t ClipboardPlugin::getConfig(const char* key, char* buf, std::uint32_t buf_size) const {
    return m_config.getValue(key, buf, buf_size);
}

std::int32_t ClipboardPlugin::setConfig(const char* key, const char* value) {
    return m_config.setValue(key, value);
}

void ClipboardPlugin::onCaptureDone(void* user, const char* /*topic*/, const void* data,
                                    std::uint32_t len) {
    auto* self = static_cast<ClipboardPlugin*>(user);
    if (self == nullptr) {
        return;
    }
    // 事件总线已经深拷贝过负载，但契约是"只在回调期间有效"：这里仍立即拷成本地
    // 字符串，之后怎么处理都不再碰 data 指针。
    std::string payload;
    if (data != nullptr && len > 0) {
        payload.assign(static_cast<const char*>(data), len);
    }
    // 异常绝不跨越 DLL 边界（STYLE.md §6.2）：回调是宿主调进来的，抛出去会让宿主
    // terminate。这里兜住所有异常并转成一条 WARN。
    try {
        self->handleCaptureDone(payload);
    } catch (...) {
        shot::Logger(self->m_host).warn("[clipboard] 处理 capture.done 时异常，已忽略本次事件");
    }
}

void ClipboardPlugin::handleCaptureDone(const std::string& payload) {
    // 用户主动关掉的开关不该在日志里刷警告：跳过只记 debug。
    if (!m_config.enabled()) {
        shot::Logger(m_host).debug("[clipboard] enabled=0，跳过剪贴板写入");
        return;
    }
    const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        shot::Logger(m_host).warn("[clipboard] capture.done 负载不是合法 JSON 对象，忽略");
        return;
    }
    const auto pathIt = doc.find("path");
    if (pathIt == doc.end() || !pathIt->is_string()) {
        shot::Logger(m_host).warn("[clipboard] capture.done 缺少 path（保存插件没写回路径？），忽略");
        return;
    }
    const std::string path = pathIt->get<std::string>();
    if (path.empty()) {
        // 保存成功但路径未知（CAPTURE.md §4）：没有文件可读，只能跳过。
        shot::Logger(m_host).warn("[clipboard] capture.done 的 path 为空，跳过剪贴板写入");
        return;
    }

    // width/height 只是日志信息，真正可信的尺寸由解码后的文件决定。
    int width = 0;
    int height = 0;
    if (const auto it = doc.find("width"); it != doc.end() && it->is_number_integer()) {
        width = it->get<int>();
    }
    if (const auto it = doc.find("height"); it != doc.end() && it->is_number_integer()) {
        height = it->get<int>();
    }

    const bool alsoFileDrop = m_config.alsoFileDrop();
    shot::Logger(m_host).info("[clipboard] 收到 capture.done：" + path + "（" +
                              std::to_string(width) + "x" + std::to_string(height) +
                              (alsoFileDrop ? "，含 CF_HDROP" : "") + "）");
    std::string error;
    if (!m_writer.copyImageFile(path, alsoFileDrop, error)) {
        // 剪贴板写入失败不影响流水线（文件已经落盘），只记 WARN 供排障。
        shot::Logger(m_host).warn("[clipboard] 写剪贴板失败：" + error);
        return;
    }
    shot::Logger(m_host).info(std::string("[clipboard] 图片已写入剪贴板（CF_DIB") +
                              (alsoFileDrop ? " + CF_HDROP" : "") + "）");
}

}  // namespace clipboard
