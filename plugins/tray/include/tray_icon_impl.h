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
//    tray_icon_window.cpp —— 窗口线程主体 / 消息循环 / 事件回调
//    tray_icon_icon.cpp   —— 托盘图标来源与刷新（宿主图标 / icon 配置 / 系统兜底）
//    tray_icon_menu.cpp   —— 右键菜单：HMENU 重建 / TrackPopupMenu / 点击发布事件
//    tray_icon_events.cpp —— 事件出队 / UTF-8 转宽字符（订阅回调只入队，Win32 在托盘线程做）
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
/**
 * @brief 订阅：宿主列表内容变化（负载 {"list":"<list id>"}）。
 *
 * 托盘只用它唤醒托盘线程重建右键菜单，不做任何数据解释（解析在托盘线程上）。
 */
inline constexpr char kTopicListsChanged[] = "lists.changed";
/**
 * @brief 订阅：批量配置已落盘（空负载）。
 *
 * 设置页"应用"最终会发这个事件；托盘收到后重读 icon 配置、必要时重设图标。
 */
inline constexpr char kTopicConfigApplied[] = "config.applied";
/** @brief 宿主卸载 UI 插件前投递的事件（走 post_to_plugin，不走总线）。 */
inline constexpr char kEventHostShutdown[] = "host.shutdown";
/**
 * @brief 内部唤醒（只进本插件队列，不来自事件总线）：配置变了，托盘线程重读图标配置。
 *
 * 宿主的 apply_stored 阶段会直接 set_config 而**不发** config.applied；为了让启动时
 * 已存图标路径也立刻生效，config_set 回调会入队这条内部事件，而不是只等事件总线。
 */
inline constexpr char kEventRefreshIcon[] = "tray.internal.refresh_icon";

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
    std::string name;     /**< 主题/事件名，如 "lists.changed" / "config.applied"。 */
    std::string payload;  /**< 负载字节；不保证以 NUL 结尾（按长度解释）。 */
};

/** @brief 托盘图标 id：与 hWnd 一起定位一个图标（同一个窗口可以有多个托盘图标）。 */
inline constexpr UINT kTrayIconId = 1;

/** @brief 托盘图标的鼠标悬停提示（NOTIFYICONDATAW::szTip，最长 127 字符）。 */
inline constexpr wchar_t kTooltip[] = L"Plugin-shot 托盘";

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

    // ---- 托盘图标（只在托盘线程上访问；来源与刷新见 tray_icon_icon.cpp） ----
    HICON icon = nullptr;         /**< 当前图标句柄：宿主图标 / 文件图标 / 系统兜底图标。 */
    bool iconOwned = false;       /**< true = 本插件创建（LoadImageW 从文件加载），换/删图标必须 DestroyIcon。 */
    bool iconHostResource = false;/**< true = 当前图标是宿主 exe 的 IDI_ICON1 资源（共享句柄）。 */
    std::string iconConfigPath;   /**< 非空 = 当前图标来自 icon 配置的 .ico 路径。 */
    bool iconAdded = false;       /**< Shell_NotifyIconW(NIM_ADD) 是否成功过。 */
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
    bool ensureWindowClass(HINSTANCE instance);
    static bool registerWindowClass(HINSTANCE instance);
    void unregisterWindowClass(HINSTANCE instance);
    LRESULT handleMessage(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);

    // ---- 托盘图标来源与刷新（tray_icon_icon.cpp） ----
    /** @brief 一次图标解析结果：句柄 + 归属（能否销毁）+ 来源，用于日志与"来源没变就不重设"。 */
    struct ResolvedIcon {
        HICON handle = nullptr;    /**< 图标句柄；解析失败时为 nullptr。 */
        bool owned = false;        /**< true = 本插件创建（文件加载），换/删图标必须 DestroyIcon。 */
        bool hostResource = false; /**< true = 宿主 exe 的 IDI_ICON1 资源（共享，不能销毁）。 */
        std::string configPath;    /**< 非空 = 来自 icon 配置的文件路径。 */
    };
    bool addIcon(HWND wnd);
    void removeIcon();
    void refreshIcon();
    bool resolveIcon(ResolvedIcon& out);
    void adoptIcon(const ResolvedIcon& resolved);
    void releaseIcon();
    std::string iconSourceDescription() const;

    // ---- 右键菜单与事件发布（tray_icon_menu.cpp；数据模型在 tray_menu.cpp） ----
    void rebuildMenu();
    void showContextMenu();
    void onListsChanged(const std::string& payload);
    void publish(const char* topic);
    void publishCommand(const TrayCommandItem& item);
    void publishDefaultCommand();

    // ---- 事件出队（tray_icon_events.cpp；托盘不弹任何气泡，提示归消息提示插件） ----
    void drainEvents();
    void enqueue(const char* name, const char* data, std::uint32_t len);
    std::wstring toWide(const std::string& utf8);
};

}  // namespace tray
