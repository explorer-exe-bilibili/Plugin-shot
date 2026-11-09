#pragma once

// ============================================================================
//  include/hotkey_window_impl.h —— 内部头：HotkeyWindow 的 PIMPL 与热键线程内部类型
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。这里会出现 Win32 类型
//  （HWND / HINSTANCE / UINT），因为热键线程确实要持有它们；公共头
//  hotkey_window.h 不包含 <windows.h>，宿主与其它插件看不到任何 Win32 句柄。
//
//  定义按职责拆成两个 .cpp：
//    hotkey_window.cpp         —— 生命周期 / 线程握手 / 事件订阅（start / stop / post）
//    hotkey_window_message.cpp —— 窗口类 / 消息循环 / 热键注册 / WM_HOTKEY 发布事件
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX  // 防止 <windows.h> 的 min/max 宏把 std::min / std::max 顶掉
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "hotkey_binding.h"
#include "hotkey_window.h"
#include "plugin_api.h"

namespace hotkey {

class HotkeyConfig;

// ---------------------------------------------------------------------------
//  事件主题（document/EVENT_BUS.md 的约定：全小写、点分隔、动词放最后）
// ---------------------------------------------------------------------------

/** @brief 发布：请求采集一次屏幕；区域截屏靠负载 {"mode":"region"} 区分。 */
inline constexpr char kTopicCaptureRequest[] = "capture.request";
/** @brief 发布：请求设置页打开（由 settings_ui 插件订阅）。 */
inline constexpr char kTopicSettingsOpen[] = "settings.open";
/** @brief 订阅：设置页应用后宿主发布空负载的 config.applied，触发热键重注册。 */
inline constexpr char kTopicConfigApplied[] = "config.applied";

/** @brief 区域截屏的 capture.request 负载（固定 JSON 字面量，不需要 JSON 库）。 */
inline constexpr char kRegionCapturePayload[] = R"json({"mode":"region"})json";

/** @brief 宿主卸载 UI 插件前投递的事件（走 post_to_plugin，不走总线）。 */
inline constexpr char kEventHostShutdown[] = "host.shutdown";

// ---------------------------------------------------------------------------
//  热键线程的窗口消息
// ---------------------------------------------------------------------------

/** @brief requestReapply() 请求重注册（配置变了 / 收到 config.applied）。 */
inline constexpr UINT kMsgReapply = WM_APP + 1;
/** @brief stop() 请求退出消息循环（窗口与热键统一在线程收尾做）。 */
inline constexpr UINT kMsgQuit = WM_APP + 2;

// ---------------------------------------------------------------------------
//  注册表
// ---------------------------------------------------------------------------

/** @brief 热键 id：RegisterHotKey 的 id + WM_HOTKEY 的 wParam，从 1 起。 */
inline constexpr int kHotkeyCapture = 1;  /**< 截屏：capture.request（空负载）。 */
inline constexpr int kHotkeyRegion = 2;   /**< 区域截屏：capture.request + {"mode":"region"}。 */
inline constexpr int kHotkeySettings = 3; /**< 打开设置：settings.open（空负载）。 */
/** @brief 注册表容量：与上面三个 id 一一对应（下标 = id - 1）。 */
inline constexpr int kHotkeyCount = 3;

/**
 * @brief 一个已注册（或尝试过）的热键。
 *
 * 热键线程独占访问：解析、RegisterHotKey / UnregisterHotKey、WM_HOTKEY 分发
 * 都只发生在热键线程上，所以这里不需要加锁。
 */
struct RegisteredHotkey {
    bool registered = false;        /**< RegisterHotKey 是否成功；失败/未启用时 false。 */
    std::uint32_t modifiers = 0;    /**< Win32 MOD_* 组合（已包含 MOD_NOREPEAT）。 */
    std::uint32_t virtualKey = 0;   /**< Win32 VK 数值。 */
    std::string label;              /**< 规范化显示名，如 "Ctrl+Alt+S"（日志用）。 */
    const char* action = "";        /**< 动作名（日志用），如 "截屏"。 */
    const char* topic = "";         /**< 要发布的事件主题。 */
    const char* payload = nullptr;  /**< 事件负载；nullptr = 空负载。 */
};

/**
 * @brief HotkeyWindow 的实现：窗口句柄、线程握手、注册表都在这里。
 *
 * 声明集中在这个结构里，定义按职责分散到两个 .cpp（STYLE.md §6.7 / §6.8）。
 */
struct HotkeyWindow::Impl {
    // ---- start() / stop() 跨线程共享的状态（mutex 保护） ----
    std::thread thread;                /**< 热键线程；joinable 表示 start 成功过。 */
    std::mutex mutex;                  /**< 保护 hwnd / acceptingEvents / reapplyPending。 */
    HWND hwnd = nullptr;               /**< 隐藏消息窗口；WM_HOTKEY 与唤醒消息都投给它。 */
    bool acceptingEvents = false;      /**< start 拉起线程后为 true；stop 一开始就置 false。 */
    bool reapplyPending = false;       /**< 收到过重注册请求（热键线程取走并清零）。 */

    // ---- start() 与热键线程之间"窗口建好了没有"的握手 ----
    std::mutex readyMutex;             /**< 保护 ready / failed。 */
    std::condition_variable readyCv;   /**< start 等待线程就绪。 */
    bool ready = false;                /**< 窗口已创建、启动时的热键注册已完成。 */
    bool failed = false;               /**< 初始化失败，start 应返回 false。 */

    const ShotHostServices* host = nullptr; /**< 宿主服务表；生命周期覆盖插件实例。 */
    HotkeyConfig* config = nullptr;         /**< 配置；只在热键线程上读。 */
    bool busSubscribed = false;             /**< 是否成功订阅过事件总线。 */

    // ---- 热键注册表（只在热键线程上访问） ----
    RegisteredHotkey hotkeys[kHotkeyCount]; /**< 下标 0/1/2 = 截屏 / 区域截屏 / 打开设置。 */

    // ---- 生命周期与窗口线程（hotkey_window.cpp / hotkey_window_message.cpp） ----
    void run();
    void messageLoop();
    bool ensureWindowClass(HINSTANCE instance);
    static bool registerWindowClass(HINSTANCE instance);
    void unregisterWindowClass(HINSTANCE instance);
    LRESULT handleMessage(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);

    /**
     * @brief 任意线程可调：置"需要重注册"标记并唤醒热键线程。
     *
     * 只入队 + PostMessage；解析与 RegisterHotKey 全部在热键线程上做，保证
     * 注册与撤销始终在同一个线程、且不与消息分发竞争。
     */
    void requestReapply();

    // ---- 热键注册与事件发布（hotkey_window_message.cpp） ----
    void applyHotkeys();
    void unregisterAllHotkeys();
    bool registerOne(int id, const char* text, const char* action, const char* topic,
                     const char* payload);
    void publishHotkey(int id);
};

}  // namespace hotkey
