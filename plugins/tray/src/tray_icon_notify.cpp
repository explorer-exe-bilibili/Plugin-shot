// 事件出队与气泡通知：把 capture.done / capture.failed 的负载解析成人话，
// 按配置决定要不要弹 NIF_INFO 气泡；lists.changed 则转给菜单重建。
//
// 分工：
//   · 事件总线回调（tray_icon_window.cpp 的 onBusEvent）运行在宿主分发线程上，
//     只调 enqueue()：深拷贝主题与负载 + PostMessage 唤醒；这里先写 enqueue 的落点。
//   · drainEvents() / onCaptureDone() / onCaptureFailed() / showBalloon() 全部只在
//     托盘线程上执行——NIF_INFO 与 NotifyIcon 句柄不是线程安全的，集中在单线程最省心。
//   · lists.changed 只做转发：真正的"重新读列表 + 重建 HMENU"在 tray_icon_menu.cpp，
//     同样只在托盘线程上发生。
//
// 负载格式约定见 document/CAPTURE.md：
//   capture.done   → JSON {"path":"...","width":N,"height":N,"processors":N}
//   capture.failed → JSON {"stage":"...","reason":"..."}

#include "tray_icon.h"

#include <cwchar>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "shot_plugin_helpers.h"
#include "tray_config.h"
#include "tray_icon_impl.h"

namespace tray {
namespace {

// 气泡标题：NIF_INFO 的标题 + 正文，两类结果各用一句，用户一眼能看出发生了什么。
constexpr wchar_t kDoneTitle[] = L"截屏完成";
constexpr wchar_t kFailedTitle[] = L"截屏失败";

// 气泡显示时长（毫秒）：Vista 之后系统用"轻松使用"里的统一时长，这个值只是给
// 老系统兜底，所以写 5000 而不是把时长做成配置项。
constexpr UINT kBalloonTimeoutMs = 5000;

}  // namespace

void TrayIcon::Impl::enqueue(const char* name, const char* data, std::uint32_t len) {
    if (name == nullptr) {
        return;
    }
    std::string payload;
    if (data != nullptr && len > 0) {
        payload.assign(data, len);  // 契约要求：返回前把 data_len 字节拷走
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!acceptingEvents) {
            return;
        }
        queue.push(QueuedEvent{name, std::move(payload)});
        wnd = hwnd;
    }
    if (wnd != nullptr) {
        // 唤醒阻塞在 GetMessage 里的托盘线程；唤醒失败也不丢数据——消息循环启动/
        // 下一轮会再 drain 一次。
        PostMessageW(wnd, kMsgDrain, 0, 0);
    }
}

void TrayIcon::Impl::drainEvents() {
    std::queue<QueuedEvent> local;
    {
        std::lock_guard<std::mutex> lock(mutex);
        local.swap(queue);
    }
    while (!local.empty()) {
        const QueuedEvent& event = local.front();
        if (event.name == kEventHostShutdown) {
            // 兜底：正常情况下 post() 收到 host.shutdown 会直接 stop()；这里再留一条
            // 线程内退出路径，防止事件从队列进来时消息循环还在跑。
            shot::Logger(host).info("[tray] 收到 host.shutdown，托盘线程退出");
            PostQuitMessage(0);
        } else if (event.name == kTopicCaptureDone) {
            onCaptureDone(event.payload);
        } else if (event.name == kTopicCaptureFailed) {
            onCaptureFailed(event.payload);
        } else if (event.name == kTopicListsChanged) {
            // 列表变了：按最新快照重建右键菜单（打开中的菜单会先挂起，见 onListsChanged）。
            onListsChanged(event.payload);
        }
        // 其它主题安静忽略：事件总线是共享的，收到不认识的主题不是错误。
        local.pop();
    }
}

void TrayIcon::Impl::onCaptureDone(const std::string& payload) {
    // 配置可能在任意线程被改，这里读的是原子快照；关掉通知就安静跳过（不写 WARN，
    // 用户主动关的东西不该在日志里刷警告）。
    if (config != nullptr && !config->notifyOnCaptureDone()) {
        shot::Logger(host).debug("[tray] notify 已关闭，跳过成功气泡");
        return;
    }
    const std::wstring text = makeCaptureDoneText(payload);
    showBalloon(kDoneTitle, text.c_str(), NIIF_INFO);
}

void TrayIcon::Impl::onCaptureFailed(const std::string& payload) {
    if (config != nullptr && !config->notifyOnCaptureError()) {
        shot::Logger(host).debug("[tray] notify_on_error 已关闭，跳过失败气泡");
        return;
    }
    const std::wstring text = makeCaptureFailedText(payload);
    showBalloon(kFailedTitle, text.c_str(), NIIF_WARNING);
}

void TrayIcon::Impl::showBalloon(const wchar_t* title, const wchar_t* text, DWORD infoFlags) {
    if (!iconAdded || hwnd == nullptr || title == nullptr || text == nullptr) {
        // 图标还没加/已被删：没有可挂气泡的对象，安静跳过。
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    // szInfoTitle / szInfo 是定长缓冲；_TRUNCATE 保证超长时安全截断，不会写坏结构体。
    wcsncpy_s(data.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(data.szInfo, text, _TRUNCATE);
    data.dwInfoFlags = infoFlags;
    data.uTimeout = kBalloonTimeoutMs;
    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
        shot::Logger(host).warn("[tray] 弹出气泡通知失败");
    } else {
        // 用户看不到气泡时（系统关了通知）Shell 也会返回成功，这里只能证明"请求已交给 Shell"。
        shot::Logger(host).info("[tray] 气泡通知已提交（NIF_INFO）");
    }
}

std::wstring TrayIcon::Impl::makeCaptureDoneText(const std::string& payload) {
    const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_discarded() && doc.is_object()) {
        const auto it = doc.find("path");
        if (it != doc.end() && it->is_string()) {
            const std::string path = it->get<std::string>();
            if (!path.empty()) {
                return std::wstring(L"已保存到 ") + toWide(path);
            }
        }
    }
    // 解析失败也要通知：气泡退化成通用提示，日志留一条说明，免得"没弹气泡"没法查。
    shot::Logger(host).warn("[tray] capture.done 负载不是约定 JSON（缺 path），气泡只显示通用提示");
    return L"截屏已保存（路径未知）";
}

std::wstring TrayIcon::Impl::makeCaptureFailedText(const std::string& payload) {
    const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_discarded() && doc.is_object()) {
        const auto it = doc.find("reason");
        if (it != doc.end() && it->is_string()) {
            const std::string reason = it->get<std::string>();
            if (!reason.empty()) {
                return std::wstring(L"截屏失败：") + toWide(reason);
            }
        }
    }
    shot::Logger(host).warn("[tray] capture.failed 负载不是约定 JSON（缺 reason），气泡只显示通用提示");
    return L"截屏失败（原因未知）";
}

std::wstring TrayIcon::Impl::toWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    // 事件负载按契约是 UTF-8（路径由保存插件以 UTF-8 写进事件），Win32 的宽字符
    // API 要 UTF-16，所以统一从这一个入口转，不在调用点各写一遍。
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) {
        shot::Logger(host).warn("[tray] 气泡文本 UTF-8 转 UTF-16 失败");
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

}  // namespace tray
