// 剪贴板插件的唯一职责：订阅 capture.done → 把刚落盘的文件复制进剪贴板。
//
// 为什么是"事件驱动"而不是"宿主调我"：像素不进事件总线（document/CAPTURE.md §3），
// capture.done 只带保存插件写回的文件路径，所以本插件从文件读图；这也意味着它
// 天然依赖"有保存插件先落盘"——没有 capture.done 或 path 为空时只记日志跳过。
//
// 线程模型：事件总线回调只在宿主分发线程（main）执行（document/EVENT_BUS.md §3），
// stb 解码 + 剪贴板写入也在这个回调里同步完成：一次截屏的图片通常只有几 MB，
// 解码是几十毫秒级，为此再开线程不值得。Windows 上到此为止；Linux 的 X11
// selection 必须常驻应答，由 ClipboardX11 自己的线程承担（回调只做读图/编码），
// 两者都在 stop() 里收干净，destroy 返回后不留后台任务。
//
// 多语言：配置项的 label/tooltip 在 clipboard_config.cpp 里写的是语言表 key，
// 文案本体由本文件的 kLangTables 发布到 i18n_source_list；宿主生成 i18n_list 后，
// 设置页按 display(owner, key) 解析。日志保持中文（e2e 按中文日志断言），不翻译。
// ============================================================================

#include "clipboard_plugin.h"

#include <iterator>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "plugin_entry.h"
#include "shot_plugin_helpers.h"

namespace clipboard {
namespace {

// capture.done 的主题名：与 document/CAPTURE.md §3、tray 插件订阅的是同一个。
const char kCaptureDoneTopic[] = "capture.done";

// 日志里的 target 名按平台取：同一句中文日志在两个平台说各自真实使用的格式，
// 不把 CF_DIB 这种 Win32 名词带到 Linux 日志里（e2e 按 Windows 文案断言，
// Windows 的字符串必须逐字保持不变）。
#if defined(_WIN32)
const char kImageTarget[] = "CF_DIB";
const char kFileListTarget[] = "CF_HDROP";
#else
const char kImageTarget[] = "image/png";
const char kFileListTarget[] = "text/uri-list";
#endif

// 语言表：key 必须与 clipboard_config.cpp 里 kConfig 的 label/tooltip 完全一致
// （设置页按 display(owner, key) 解析，拼错就显示 key 本身）。两张表的 key 集
// 必须一致、@meta 必填、每条文案都要有；JSON 文本里不能有裸换行，而整行又超过
// 120 列上限，所以用相邻 raw 字面量拼接——拼出来的仍是一行合法 JSON。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"剪贴板",)json"
    R"json("plugin.description":"截屏落盘后把图片写进系统剪贴板，可选同时提供文件列表（CF_HDROP / text/uri-list）",)json"
    R"json("config.enabled.label":"写进剪贴板",)json"
    R"json("config.enabled.tooltip":"截屏落盘后把图片复制进系统剪贴板，可以直接粘贴到聊天/文档",)json"
    R"json("config.also_file_drop.label":"同时提供文件",)json"
    R"json("config.also_file_drop.tooltip":"额外提供 CF_HDROP：资源管理器里可粘贴成文件；不勾选则只发图片"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"Clipboard",)json"
    R"json("plugin.description":"Copies the saved screenshot to the system clipboard, optionally also as a file list (CF_HDROP / text/uri-list)",)json"
    R"json("config.enabled.label":"Copy to clipboard",)json"
    R"json("config.enabled.tooltip":)json"
    R"json("Copies the saved screenshot to the system clipboard so you can paste it into a chat or document",)json"
    R"json("config.also_file_drop.label":"Also provide file",)json"
    R"json("config.also_file_drop.tooltip":)json"
    R"json("Also adds a CF_HDROP file list so Explorer can paste it as a file; unchecked copies the image only"})json",
};

}  // namespace

ClipboardPlugin::~ClipboardPlugin() {
    stop();  // 正常路径由 destroy 调；这里兜底，重复退订是空操作
}

bool ClipboardPlugin::start(const ShotHostServices* host) {
    m_host = host;

    // 语言表要在设置页第一次读 config_list 之前发布：宿主据此生成 i18n_list。
    // owner 直接用插件身份证（kClipboard.id），宿主会校验它等于已加载的插件 id，
    // 所以这里不另写字符串，避免与 metadata.json / plugin_entry.cpp 漂移。
    m_i18n = std::make_unique<shot::I18n>(host, kClipboard.id);
    if (!m_i18n->loadTables(kLangTables, std::size(kLangTables))) {
        shot::Logger(host).warn("[clipboard] 语言表解析失败，配置项文案回退为 key");
    }

    shot::EventBus bus(host);
    if (!bus.subscribe(kCaptureDoneTopic, &ClipboardPlugin::onCaptureDone, this)) {
        // 老宿主没有事件总线：剪贴板功能不可用，但插件本身照常加载（安静降级，
        // 不把"少一个服务"升级成"插件加载失败"）。
        shot::Logger(host).warn("[clipboard] 订阅 capture.done 失败（宿主无事件总线），剪贴板功能不可用");
        m_subscribed = false;
        return false;
    }
    m_subscribed = true;
#if !defined(_WIN32)
    // Linux/X11：selection 必须常驻应答，所以除了订阅事件还要起一个 X11 线程。
    // 没有 X11 Display（Wayland 会话）时只 WARN：截屏与其它插件照常工作。
    std::string error;
    m_writerReady = m_writer.start(host, error);
    if (m_writerReady) {
        shot::Logger(host).info("[clipboard] X11 CLIPBOARD selection 线程已就绪");
    } else {
        // 纯 Wayland 且没有 XWayland 是预期情况：WARN 一次即可。之后 capture.done
        // 直接跳过，不再每条事件都读图/编码后才发现剪贴板不可用。
        shot::Logger(host).warn("[clipboard] X11 剪贴板不可用：" + error);
    }
#endif
#if defined(_WIN32)
    shot::Logger(host).info("[clipboard] 已订阅 capture.done：截屏落盘后写进剪贴板");
#else
    shot::Logger(host).info("[clipboard] 已订阅 capture.done：截屏落盘后交给剪贴板后端");
#endif
    return true;
}

void ClipboardPlugin::stop() {
    if (m_subscribed) {
        // 退订句柄就是 this（subscribe 时的 user 指针）：一次摘掉本插件注册的全部主题。
        shot::EventBus bus(m_host);
        bus.unsubscribe(this);
        m_subscribed = false;
    }
#if !defined(_WIN32)
    // 退订之后、释放语言表之前停 X11 线程：它会先放弃 selection 所有权再 join，
    // 宿主之后 FreeLibrary 时不会再留下指向本 DLL 的事件处理代码。
    m_writer.stop();
#endif
    // I18n 析构会退订 lists.changed。放在退订 capture.done 之后：宿主事件分发期间
    // 不会回调一个正在销毁的状态。
    m_i18n.reset();
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
#if !defined(_WIN32)
    if (!m_writerReady) {
        // start() 已经 WARN 过一次；这里只记 debug，避免纯 Wayland 会话每次截屏都刷警告。
        shot::Logger(m_host).debug("[clipboard] X11 剪贴板不可用，跳过本次写入");
        return;
    }
#endif
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
                              (alsoFileDrop ? std::string("，含 ") + kFileListTarget : "") +
                              "）");
    std::string error;
    if (!m_writer.copyImageFile(path, alsoFileDrop, error)) {
        // 剪贴板写入失败不影响流水线（文件已经落盘），只记 WARN 供排障。
        shot::Logger(m_host).warn("[clipboard] 写剪贴板失败：" + error);
        return;
    }
#if defined(_WIN32)
    shot::Logger(m_host).info(std::string("[clipboard] 图片已写入剪贴板（") + kImageTarget +
                              (alsoFileDrop ? std::string(" + ") + kFileListTarget : "") +
                              "）");
#else
    // Linux 的 copyImageFile 只保证“已排进 X11 线程”：真正取得所有权与 TARGETS
    // 自检在 selection 线程异步完成，日志不能提前宣称已经可粘贴。
    shot::Logger(m_host).info(
        std::string("[clipboard] 图片已交给 X11 selection 线程（") + kImageTarget +
        (alsoFileDrop ? std::string(" + ") + kFileListTarget : "") + "），等待 SelectionNotify 确认");
#endif
}

}  // namespace clipboard
