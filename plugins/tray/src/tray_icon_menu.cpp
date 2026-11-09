// 右键菜单的 Win32 渲染与点击分发：把 TrayMenu 的数据模型变成 HMENU，
// 并在列表变化（lists.changed）时按最新快照重建。
//
// 三条硬约束：
//   · 菜单只在托盘线程上创建 / 销毁 / 弹出。事件总线回调（tray_icon.cpp 的订阅 +
//     tray_icon_notify.cpp 的 drainEvents）只负责唤醒，绝不碰 HMENU；
//   · TrackPopupMenu 是模态循环：菜单打开期间不能 DestroyMenu 正在用的句柄，
//     收到 lists.changed 只记 pending，关闭后立刻补重建；
//   · 点击菜单项 = 发布条目声明的 event（负载 item.data）。托盘不认识任何具体命令，
//     也不直接调用别的插件（EVENT_BUS.md §1）。
//
// 菜单数据的解析 / 排序 / 兜底在 tray_menu.cpp；本文件只处理 Win32 这一半。

#include "tray_icon.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "shot_plugin_helpers.h"
#include "tray_icon_impl.h"
#include "tray_menu.h"

namespace tray {
namespace {

// 菜单命令 id 从 1 开始（0 留给分隔线与 TrackPopupMenu 的"取消"返回值）：
// 命令 id - kMenuCommandBase 就是它在 commandItems 里的下标。
constexpr UINT kMenuCommandBase = 1;

/**
 * @brief lists.changed 的负载是否与托盘菜单有关。
 *
 * 宿主负载是 JSON `{"list":"<id>"}`；空负载 / 结构不认识时按"可能是任意列表"
 * 处理——宁可多重建一次，也不要漏掉本插件关心的菜单刷新。
 *
 * 只服务本文件的 lists.changed 过滤、不碰 Impl 状态，所以放匿名命名空间而不是
 * Impl 的成员（STYLE.md §6.7）。
 */
bool refersToTrayCommandList(const std::string& payload) {
    if (payload.empty()) {
        return true;
    }
    const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, false);
    if (doc.is_object()) {
        const auto it = doc.find("list");
        if (it != doc.end() && it->is_string()) {
            return it->get<std::string>() == kTrayCommandListId;
        }
        return true;
    }
    // 不是 JSON：兼容"负载直接就是 list id"的约定，用子串匹配容忍引号/空白。
    return payload.find(kTrayCommandListId) != std::string::npos;
}

}  // namespace

void TrayIcon::Impl::rebuildMenu() {
    menuRefreshPending = false;
    HMENU fresh = CreatePopupMenu();
    if (fresh == nullptr) {
        shot::Logger(host).warn("[tray] 创建右键菜单失败，保留旧菜单");
        return;
    }

    // 新模型与新菜单都先建在局部变量里，全部成功后才一次性替换成员：任何一步
    // 失败时旧菜单 + 旧映射（commandItems）仍然配套，点击不会串到别的条目上。
    TrayMenu freshModel;
    std::vector<std::size_t> freshCommands;
    bool built = false;
    try {
        // 1. 从宿主列表服务取最新快照。列表不可用时 TrayMenu 会装本地兜底「退出」。
        freshModel.load(host);
        // 2. 把模型渲染成新菜单。
        UINT commandId = kMenuCommandBase;
        for (std::size_t index = 0; index < freshModel.items().size(); ++index) {
            const TrayCommandItem& item = freshModel.items()[index];
            if (item.separator) {
                AppendMenuW(fresh, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            const std::wstring label = toWide(item.label);
            if (label.empty()) {
                // toWide 失败时已经记过 WARN；空文字的菜单项没有意义，跳过。
                continue;
            }
            if (!AppendMenuW(fresh, MF_STRING, commandId, label.c_str())) {
                shot::Logger(host).warn("[tray] 添加菜单项失败：" + item.id);
                continue;
            }
            freshCommands.push_back(index);
            ++commandId;
        }
        built = !freshCommands.empty();
    } catch (...) {
        // 解析/分配失败不能让异常穿出托盘线程（std::terminate 会杀掉宿主）：
        // 刚建的句柄统一在下面销毁，旧菜单继续用。
        shot::Logger(host).warn("[tray] 重建右键菜单异常，保留旧菜单");
    }

    if (!built) {
        DestroyMenu(fresh);
        shot::Logger(host).warn("[tray] 菜单没有可点击命令，保留旧菜单");
        return;
    }

    menuModel = std::move(freshModel);
    commandItems = std::move(freshCommands);
    const HMENU old = menu;
    menu = fresh;
    if (old != nullptr) {
        DestroyMenu(old);  // 新菜单与模型都就绪后才销毁旧句柄
    }
    // 把渲染结果写进日志：验收/排障时不用截图也能确认菜单里到底有什么。
    std::string summary;
    for (const TrayCommandItem& item : menuModel.items()) {
        if (item.separator) {
            summary += " |";
        } else {
            summary += " [" + item.label + "→" + item.event + "]";
        }
    }
    shot::Logger(host).info(
        "[tray] 右键菜单已重建：" + std::to_string(commandItems.size()) + " 个命令（来源 " +
        (menuModel.isFallback() ? "本地兜底" : "tray_command_list") + "）" + summary);
}

void TrayIcon::Impl::showContextMenu() {
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        wnd = hwnd;
    }
    if (wnd == nullptr) {
        return;
    }
    // 菜单打开期间收到的 lists.changed 是挂起的（见 onListsChanged）：先按最新数据
    // 重建再弹；menu 为空说明上次重建失败，这里再试一次。
    if (menu == nullptr || menuRefreshPending) {
        rebuildMenu();
    }
    if (menu == nullptr) {
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    // 先把自己提到前台再弹菜单：否则用户点菜单外面时菜单不会关闭（托盘菜单的经典坑）。
    SetForegroundWindow(wnd);
    // menuOpen 是给 onListsChanged 看的：TrackPopupMenu 是模态循环，期间不能重建 HMENU。
    menuOpen = true;
    const UINT command = TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, wnd, nullptr);
    menuOpen = false;
    // 菜单关掉后补一条空消息，把"点掉菜单"的那次鼠标消息吃掉；否则它会被当成
    // 一次托盘左键点击，又截一张图。
    PostMessageW(wnd, WM_NULL, 0, 0);

    // 先把命令 id 映射回条目并发布，再决定是否重建：命令 id 只对"刚关掉的那份
    // 菜单"有效，重建会换掉 menuModel / commandItems 的对应关系。
    if (command >= kMenuCommandBase) {
        const std::size_t index = static_cast<std::size_t>(command - kMenuCommandBase);
        if (index < commandItems.size() && commandItems[index] < menuModel.items().size()) {
            publishCommand(menuModel.items()[commandItems[index]]);
        }
    }
    if (menuRefreshPending) {
        rebuildMenu();  // 菜单打开期间列表变过：现在补上
    }
}

void TrayIcon::Impl::onListsChanged(const std::string& payload) {
    if (!refersToTrayCommandList(payload)) {
        shot::Logger(host).debug("[tray] lists.changed 与托盘菜单无关，忽略");
        return;
    }
    if (menuOpen) {
        // 用户正看着的菜单挂在当前 HMENU 上：销毁会动到它，等菜单关闭后再重建。
        menuRefreshPending = true;
        shot::Logger(host).info("[tray] 菜单打开中，托盘列表变化已挂起，关闭后重建");
        return;
    }
    rebuildMenu();
}

void TrayIcon::Impl::publish(const char* topic) {
    // 托盘不直接调用别的插件：所有动作只通过事件总线广播（EVENT_BUS.md §1），
    // 发送方不知道谁在听，接收方不知道谁发的。
    shot::EventBus bus(host);
    if (bus.publish(topic)) {
        // 日志写"动作"而不是"发布成功"：验收/排障时靠它确认哪个入口触发过。
        shot::Logger(host).info(std::string("[tray] 发布 ") + topic);
    } else {
        // 队列满 / 老宿主没有总线时事件会被丢弃；托盘不是关键路径，记一条警告即可。
        shot::Logger(host).warn(std::string("[tray] 发布失败（无事件总线或队列满）: ") + topic);
    }
}

void TrayIcon::Impl::publishCommand(const TrayCommandItem& item) {
    // 点击 = 发布条目声明的事件：托盘既不知道、也不关心是谁在听。
    if (item.event.empty()) {
        return;
    }
    shot::EventBus bus(host);
    if (bus.publish(item.event.c_str(), item.data)) {
        // 日志带 topic，验收脚本靠 "发布 <topic>" 确认点击真的触发了事件。
        shot::Logger(host).info("[tray] 发布 " + item.event + "（菜单项 " + item.id + "）");
    } else {
        shot::Logger(host).warn("[tray] 发布失败（无事件总线或队列满）: " + item.event);
    }
}

void TrayIcon::Impl::publishDefaultCommand() {
    // 左键单击：列表里标了 "default":true 的条目就是"默认动作"；core 预置项没有
    // 这个字段，所以默认仍是历史行为 capture.request。
    for (const TrayCommandItem& item : menuModel.items()) {
        if (!item.separator && item.isDefault && !item.event.empty()) {
            shot::Logger(host).info("[tray] 左键单击：执行 default 命令 " + item.id);
            publishCommand(item);
            return;
        }
    }
    publish(kTopicCaptureRequest);
}

}  // namespace tray
