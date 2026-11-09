// 事件出队与 UTF-8 转宽字符：托盘线程从自己的队列里取宿主事件并分发。
//
// 分工：
//   · 事件总线回调（tray_icon_window.cpp 的 onBusEvent）运行在宿主分发线程上，
//     只调 enqueue()：深拷贝主题与负载 + PostMessage 唤醒；这里先写 enqueue 的落点。
//   · drainEvents() 只在托盘线程上执行：lists.changed → 重建右键菜单、
//     config.applied → 重读 icon 配置，两者最终都在托盘线程上碰 Win32。
//   · 托盘不订阅、也不处理任何 capture.* 结果事件：成功/失败气泡由独立的消息提示
//     插件 plugins/notify 负责，托盘只管图标 + 菜单（避免同一件事弹两个气泡）。
//
// toWide 也放在这里：它是"UTF-8 文本 → Win32 宽字符"的唯一入口，图标路径（配置值）
// 与日志文本都复用它，避免每个调用点各写一遍 MultiByteToWideChar。

#include "tray_icon.h"

#include <string>
#include <utility>

#include "shot_plugin_helpers.h"
#include "tray_icon_impl.h"

namespace tray {

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
        } else if (event.name == kTopicListsChanged) {
            // 列表变了：按最新快照重建右键菜单（打开中的菜单会先挂起，见 onListsChanged）。
            onListsChanged(event.payload);
        } else if (event.name == kTopicConfigApplied || event.name == kEventRefreshIcon) {
            // 配置已应用（设置页路径）或宿主刚 set_config 过（apply_stored 路径）：
            // 重读 icon 配置，来源路径真变了才重设图标。
            refreshIcon();
        }
        // 其它主题安静忽略：事件总线是共享的，收到不认识的主题不是错误。
        local.pop();
    }
}

std::wstring TrayIcon::Impl::toWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    // 配置值与日志文本按契约是 UTF-8，Win32 的宽字符 API 要 UTF-16，所以统一从
    // 这一个入口转，不在调用点各写一遍。
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) {
        shot::Logger(host).warn("[tray] 文本 UTF-8 转 UTF-16 失败");
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

}  // namespace tray
