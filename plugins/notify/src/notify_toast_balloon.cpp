// 气泡文本与发送：把 capture.done / capture.failed 拼成通知标题与正文，交给 Shell。
//
// 文本链路（排查"标题乱码"时先看这里）：
//   · 语言表 / 配置值是 UTF-8（源文件用 /utf-8 编译，见 cmake/ShotPlugin.cmake）；
//   · toWide() 用 MultiByteToWideChar(CP_UTF8, ...) —— 显式 UTF-8 → UTF-16，
//     不经过 CP_ACP/GBK，也不存在 char* 直接塞进宽缓冲的路径；
//   · NOTIFYICONDATAW + Shell_NotifyIconW 全程宽字符，Shell 拿到的就是 UTF-16。
//   showBalloon() 会记一行"标题字符数 + 首字符 codepoint"，下次再有人报乱码时先看
//   这行：首字符是预期的汉字码点（如 U+622A 截）就说明本插件送进去的是正确的文本，
//   问题在显示侧（系统通知中心缓存 / 旧构建 / 用 ANSI 编辑器看日志）。
//
// 标题来源：配置项 `title` 非空时用它的展开结果（见 notify_title.cpp），否则用
// toast.success_title / toast.failed_title 这两条多语言默认标题。

#include "notify_toast.h"

#include <cstdio>
#include <cwchar>
#include <string>

#include <nlohmann/json.hpp>

#include "notify_config.h"
#include "notify_title.h"
#include "notify_toast_impl.h"
#include "shot_plugin_helpers.h"

// NIIF_RESPECT_QUIET_TIME 需要 Windows 7+ 的 SDK 目标；工程没显式声明 NTDDI 版本时
// 老 SDK 可能没有这个宏，这里给数值兜底（0x80 是 Windows SDK 的官方取值）。
#ifndef NIIF_RESPECT_QUIET_TIME
#define NIIF_RESPECT_QUIET_TIME 0x00000080
#endif

namespace notify {
namespace {

// 气泡显示时长（毫秒）：Vista 之后系统用"轻松使用"里的统一时长，这个值只给老系统兜底。
constexpr UINT kBalloonTimeoutMs = 5000;

// szInfo 是 256 个 wchar 的定长缓冲（含结尾 NUL）：正文最多 255 个字符。
// 留一点余量，超过就按"保留头尾、中间用 … 省略"截断（路径末尾通常最关键）。
constexpr std::size_t kMaxInfoChars = 200;

/**
 * @brief 中间截断：超长文本保留头部与尾部，中间用一个省略号（NIF_INFO 定长缓冲不能溢出）。
 * @param[in] text     原始文本。
 * @param[in] maxChars 截断后的最大字符数（含省略号）。
 * @return 不超过 maxChars 个字符的文本；不超长时原样返回。
 */
std::wstring truncateMiddle(const std::wstring& text, std::size_t maxChars) {
    if (text.size() <= maxChars || maxChars <= 1) {
        return text.size() <= maxChars ? text : std::wstring(1, L'…');
    }
    const std::size_t head = maxChars * 3 / 5;    // 头多留一点：盘符/目录名比文件名的前缀信息量高
    const std::size_t tail = maxChars - head - 1;  // 1 个字符给省略号
    return text.substr(0, head) + L"…" + text.substr(text.size() - tail);
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

/**
 * @brief 把标题的第一个 UTF-16 单元格式化成 "U+XXXX"，用于编码自检日志。
 * @param[in] text 标题；空指针/空串返回固定说明文本。
 * @return 可直接拼进日志的 ASCII 文本。
 */
std::string firstCodePointText(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') {
        return "无（空标题）";
    }
    // 只取第一个 UTF-16 单元：常用汉字都在 BMP 内，够用于判断"送进去的是不是汉字"。
    const unsigned long first = static_cast<unsigned long>(text[0]);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "U+%04lX", first);
    return std::string(buffer);
}

}  // namespace

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

// ---------------------------------------------------------------------------
// 文案：气泡标题/正文与隐藏图标的提示都按当前语言取
// ---------------------------------------------------------------------------

const char* NotifyToast::Impl::localized(const char* key, const char* fallback) const {
    // i18n 是可选对象：初始化失败或老宿主没有语言表时用中文原文兜底，
    // 气泡永远有内容可显示（fallback 必须保持中文，见 I18N 约定）。
    return (i18n != nullptr) ? i18n->tr(key, fallback) : fallback;
}

std::wstring NotifyToast::Impl::configuredTitle(const char* defaultKey,
                                                const char* defaultFallback,
                                                const std::string& path, int width,
                                                int height) const {
    if (config != nullptr) {
        // config->title() 返回拷贝（设置页线程可能同时改写）；expandTitle 对空模板
        // 返回空串，这里统一当成"用默认标题"。
        const std::string expanded = expandTitle(config->title(), path, width, height);
        if (!expanded.empty()) {
            const std::wstring wide = toWide(expanded);
            if (!wide.empty()) {
                return wide;
            }
            shot::Logger(host).warn("[notify] 配置的通知标题不是合法 UTF-8，回退默认标题");
        }
    }
    return toWide(localized(defaultKey, defaultFallback));
}

// ---------------------------------------------------------------------------
// 通知内容（capture.done / capture.failed → 气泡）
// ---------------------------------------------------------------------------

void NotifyToast::Impl::onCaptureDone(const std::string& payload) {
    if (config == nullptr) {
        return;
    }
    if (!config->enabled() || !config->showOnSuccess()) {
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
    const std::wstring widePath = toWide(path);
    if (!path.empty() && widePath.empty()) {
        shot::Logger(host).warn("[notify] capture.done 的 path 不是合法 UTF-8，气泡显示通用提示");
    }
    std::wstring text;
    if (path.empty()) {
        text = toWide(localized("toast.no_path", "已保存（保存插件未回报路径）"));
        lastImagePath.clear();  // 没有路径就没有可点击的目标
    } else {
        // 路径是宿主/保存插件给的原文（capture.done 的 path），原样拼接、不做二次翻译。
        text = toWide(localized("toast.saved_prefix", "已保存到 "));
        text.append(widePath.empty()
                        ? toWide(localized("toast.path_unavailable", "（路径无法显示）"))
                        : widePath);
        text = truncateMiddle(text, kMaxInfoChars);
        // 记下这次通知对应的图片，用户点击通知时打开它（notify_toast_click.cpp）。
        lastImagePath = widePath.empty() ? std::wstring() : widePath;
    }
    const std::wstring title =
        configuredTitle("toast.success_title", "截屏成功", path, width, height);
    shot::Logger(host).info("[notify] capture.done：" +
                            (path.empty() ? std::string("<无路径>") : path));
    showBalloon(title.c_str(), text.c_str(), NIIF_INFO);
}

void NotifyToast::Impl::onCaptureFailed(const std::string& payload) {
    if (config == nullptr) {
        return;
    }
    if (!config->enabled() || !config->showOnError()) {
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
    // 失败通知没有图片路径：清掉上一次成功通知留下的目标，点这条通知不做任何事。
    lastImagePath.clear();
    const std::wstring title = configuredTitle("toast.failed_title", "截屏失败", std::string(), 0, 0);
    const std::wstring text =
        truncateMiddle(toWide(localized("toast.failed_prefix", "截图失败：")) + toWide(reason),
                       kMaxInfoChars);
    shot::Logger(host).info("[notify] capture.failed：" + reason);
    showBalloon(title.c_str(), text.c_str(), NIIF_WARNING);
}

void NotifyToast::Impl::showBalloon(const wchar_t* title, const wchar_t* text, DWORD infoFlags) {
    if (!iconAdded || hwnd == nullptr || title == nullptr || text == nullptr) {
        // 隐藏图标还没加/已被删：没有可挂气泡的对象，安静跳过。
        return;
    }
    // 编码自检（排障用，低频）：标题从 UTF-8 转到 UTF-16 之后，本插件就再也看不到
    // 原始字节了。"字符数 + 首字符 codepoint" 这行日志能区分三种情况：
    //   · 首字符是预期汉字码点（如 U+622A）→ 送进 Shell 的文本是对的；
    //   · 首字符是 U+FFFD → 转换时出现替换字符（输入编码不对）；
    //   · 0 个字符 → toWide 失败，标题根本没送进去。
    shot::Logger(host).info("[notify] 气泡标题自检：" + std::to_string(std::wcslen(title)) +
                            " 个 UTF-16 单元，首字符 " + firstCodePointText(title));
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kNotifyIconId;
    data.uFlags = NIF_INFO;
    // szInfoTitle / szInfo 是定长缓冲；_TRUNCATE 保证超长时安全截断，不会写坏结构体。
    wcsncpy_s(data.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(data.szInfo, text, _TRUNCATE);
    // NIIF_RESPECT_QUIET_TIME：系统处于"专注助手/免打扰"时不打扰用户。
    data.dwInfoFlags = infoFlags | NIIF_RESPECT_QUIET_TIME;
    if (config != nullptr && !config->soundEnabled()) {
        data.dwInfoFlags |= NIIF_NOSOUND;  // 用户关了提示音：Windows 10+ 的静音标志
    }
    data.uTimeout = kBalloonTimeoutMs;
    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
        shot::Logger(host).warn("[notify] 提交气泡通知失败（Shell_NotifyIconW NIM_MODIFY）");
    } else {
        // 用户看不到通知时（系统关了通知）Shell 也会返回成功，这里只能证明"请求已交给 Shell"。
        shot::Logger(host).info("[notify] 气泡通知已提交（NIF_INFO）");
    }
}

}  // namespace notify
