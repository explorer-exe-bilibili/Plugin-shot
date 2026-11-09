#pragma once

// ============================================================================
//  include/tray_icon_impl.h —— 内部头：TrayIcon 的 PIMPL 与托盘线程内部类型
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。这里会出现 Win32 类型
//  （HWND / HICON / HMENU），因为托盘线程确实要持有它们；公共头 tray_icon.h
//  不包含 <windows.h>，宿主与其它插件看不到任何 Win32 句柄。
//
//  定义按职责拆成几个 .cpp：
//    tray_icon.cpp        —— 生命周期 / 线程握手 / 事件投递（start / stop / post）
//    tray_icon_window.cpp —— 窗口线程主体 / 消息循环 / 托盘图标 / 事件回调
//    tray_icon_menu.cpp   —— 右键菜单：HMENU 重建 / TrackPopupMenu / 点击发布事件
//    tray_icon_notify.cpp —— 事件出队 / 气泡通知（capture.done / capture.failed）
//    tray_menu.cpp        —— 菜单数据模型（tray_command_list 的解析 / 排序 / 兜底）
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX  // 防止 <windows.h> 的 min/max 宏把 std::min / std::max 顶掉
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include "plugin_api.h"
#include "tray_icon.h"
#include "tray_menu.h"

namespace tray {

class TrayConfig;

// ---------------------------------------------------------------------------
//  托盘用到的事件主题（document/EVENT_BUS.md 的约定：全小写、点分隔）
// ---------------------------------------------------------------------------

/** @brief 发布：请求采集一次屏幕（左键单击的兜底动作、core 预置菜单项）。 */
inline constexpr char kTopicCaptureRequest[] = "capture.request";
/** @brief 订阅：采集 + 处理 + 保存成功（负载见 document/CAPTURE.md）。 */
inline constexpr char kTopicCaptureDone[] = "capture.done";
/** @brief 订阅：采集/处理/保存某个阶段失败（负载见 document/CAPTURE.md）。 */
inline constexpr char kTopicCaptureFailed[] = "capture.failed";
/**
 * @brief 订阅：宿主列表内容变化（负载 {"list":"<list id>"}）。
 *
 * 托盘只用它唤醒托盘线程重建右键菜单，不做任何数据解释（解析在托盘线程上）。
 */
inline constexpr char kTopicListsChanged[] = "lists.changed";
/** @brief 宿主卸载 UI 插件前投递的事件（走 post_to_plugin，不走总线）。 */
inline constexpr char kEventHostShutdown[] = "host.shutdown";

// ---------------------------------------------------------------------------
//  托盘消息窗口的自定义消息
// ---------------------------------------------------------------------------

/** @brief 队列里来了新事件，唤醒阻塞在 GetMessage 里的托盘线程。 */
inline constexpr UINT kMsgDrain = WM_APP + 1;
/** @brief stop() 请求退出消息循环（窗口销毁统一放在线程收尾做）。 */
inline constexpr UINT kMsgQuit = WM_APP + 2;
/**
 * @brief 托盘鼠标回调（NOTIFYICONDATAW::uCallbackMessage）；lParam 是原始鼠标消息。
 *
 * 用 WM_APP 段的自定义消息而不是窗口自己的 WM_LBUTTONUP：托盘回调把鼠标消息
 * 塞在 lParam 里，和真实窗口消息的 wParam/lParam 语义不同，必须单独一个入口。
 */
inline constexpr UINT kMsgTrayCallback = WM_APP + 10;

/** @brief 队列里的一个事件：主题名 + 负载字节（总线单条负载上限 64 KiB）。 */
struct QueuedEvent {
    std::string name;     /**< 主题/事件名，如 "capture.done"。 */
    std::string payload;  /**< 负载字节；不保证以 NUL 结尾（按长度解释）。 */
};

/** @brief 托盘图标 id：与 hWnd 一起定位一个图标（同一个窗口可以有多个托盘图标）。 */
inline constexpr UINT kTrayIconId = 1;

/**
 * @brief TrayIcon 的实现：窗口句柄、线程握手、事件队列、托盘图标状态都在这里。
 *
 * 声明集中在这个结构里，定义按职责分散到多个 .cpp（STYLE.md §6.7 / §6.8）。
 */
struct TrayIcon::Impl {
    // ---- start() / stop() 跨线程共享的状态（mutex 保护） ----
    std::thread thread;                 /**< 托盘线程；joinable 表示 start 成功过。 */
    std::mutex mutex;                   /**< 保护下面的事件队列 / hwnd / 入队开关。 */
    std::queue<QueuedEvent> queue;      /**< 尚未处理的宿主事件。 */
    HWND hwnd = nullptr;                /**< 隐藏消息窗口；托盘回调与唤醒消息都投给它。 */
    bool acceptingEvents = false;       /**< start 拉起线程后为 true；stop 一开始就置 false。 */

    // ---- start() 与托盘线程之间"图标加好了没有"的握手 ----
    std::mutex readyMutex;              /**< 保护 ready / failed。 */
    std::condition_variable readyCv;    /**< start 等待线程就绪。 */
    bool ready = false;                 /**< 托盘图标已成功添加。 */
    bool failed = false;                /**< 初始化失败，start 应返回 false。 */

    const ShotHostServices* host = nullptr; /**< 宿主服务表；生命周期覆盖插件实例。 */
    TrayConfig* config = nullptr;           /**< 开关配置；只在托盘线程上读。 */
    bool busSubscribed = false;             /**< 是否成功订阅过事件总线。 */

    // ---- 只在托盘线程上访问（线程创建后到退出前） ----
    HICON icon = nullptr;              /**< 托盘图标；LoadIconW 返回的系统共享图标，不能 DestroyIcon。 */
    bool iconAdded = false;            /**< Shell_NotifyIconW(NIM_ADD) 是否成功过。 */
    UINT taskbarCreatedMessage = 0;    /**< "TaskbarCreated" 注册消息；explorer 重启后重加图标。 */
    UINT trayCallbackMessage = 0;      /**< NOTIFYICONDATAW::uCallbackMessage 的取值。 */

    // ---- 右键菜单（只在托盘线程上访问；由 lists.changed 驱动重建） ----
    TrayMenu menuModel;                    /**< 菜单数据模型：tray_command_list 的解析结果。 */
    HMENU menu = nullptr;                  /**< 缓存的右键菜单；重建时 DestroyMenu 后换新。 */
    std::vector<std::size_t> commandItems; /**< 命令 id（从 1 起）→ menuModel.items() 下标。 */
    bool menuOpen = false;                 /**< TrackPopupMenu 正在跑，此时不能重建 HMENU。 */
    bool menuRefreshPending = false;       /**< 菜单打开期间收到 lists.changed，关闭后补重建。 */

    // ---- 生命周期与窗口线程（tray_icon.cpp / tray_icon_window.cpp） ----
    void run();
    void messageLoop();
    bool addIcon(HWND wnd);
    void removeIcon();
    bool ensureWindowClass(HINSTANCE instance);
    static bool registerWindowClass(HINSTANCE instance);
    void unregisterWindowClass(HINSTANCE instance);
    LRESULT handleMessage(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);

    // ---- 右键菜单与事件发布（tray_icon_menu.cpp；数据模型在 tray_menu.cpp） ----
    void rebuildMenu();
    void showContextMenu();
    void onListsChanged(const std::string& payload);
    void publish(const char* topic);
    void publishCommand(const TrayCommandItem& item);
    void publishDefaultCommand();

    // ---- 事件出队与气泡通知（tray_icon_notify.cpp） ----
    void drainEvents();
    void enqueue(const char* name, const char* data, std::uint32_t len);
    void showBalloon(const wchar_t* title, const wchar_t* text, DWORD infoFlags);
    void onCaptureDone(const std::string& payload);
    void onCaptureFailed(const std::string& payload);
    std::wstring makeCaptureDoneText(const std::string& payload);
    std::wstring makeCaptureFailedText(const std::string& payload);
    std::wstring toWide(const std::string& utf8);
};

}  // namespace tray
