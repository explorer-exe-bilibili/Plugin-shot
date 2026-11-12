#pragma once

// ============================================================================
//  include/hotkey_window_impl.h —— 内部头：HotkeyWindow 的 PIMPL 与热键线程内部类型
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。这里会出现 Win32 类型
//  （HWND / HINSTANCE / UINT），因为热键线程确实要持有它们；公共头
//  hotkey_window.h 不包含 <windows.h>，宿主与其它插件看不到任何 Win32 句柄。
//
//  定义按职责拆成三个 .cpp：
//    hotkey_window.cpp          —— 生命周期 / 线程握手 / 事件订阅（start / stop / post）
//    hotkey_window_message.cpp  —— 窗口类 / 消息循环 / 暂停状态 / WM_HOTKEY 分发
//    hotkey_window_register.cpp —— 按 hotkey_list 快照注册 / 撤销热键
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <mutex>
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

#include "hotkey_binding.h"
#include "hotkey_topics.h"
#include "hotkey_window.h"
#include "plugin_api.h"

namespace hotkey {

class HotkeyCatalog;
class HotkeyConfig;

// ---------------------------------------------------------------------------
//  热键线程的窗口消息
//
//  订阅/投递用的主题常量在 hotkey_topics.h：Linux X11 实现订阅同一批主题，
//  常量只能有一份来源。
// ---------------------------------------------------------------------------

/** @brief requestReapply() 请求重注册（配置变了 / 收到 config.applied）。 */
inline constexpr UINT kMsgReapply = WM_APP + 1;
/** @brief stop() 请求退出消息循环（窗口与热键统一在线程收尾做）。 */
inline constexpr UINT kMsgQuit = WM_APP + 2;
/**
 * @brief 设置页请求暂停/恢复全局热键注册（收到 hotkey.suspend / hotkey.resume）。
 *
 * 消息只负责唤醒热键线程；权威的期望状态在 Impl::suspendPending / suspendValue 里，
 * 这样"暂停→恢复"两条紧挨着的通知最终一定落到恢复（最后一次请求生效）。
 */
inline constexpr UINT kMsgSetSuspend = WM_APP + 3;

// ---------------------------------------------------------------------------
//  注册表
// ---------------------------------------------------------------------------

/**
 * @brief 一个已注册（或尝试过）的热键。
 *
 * 条目数量在运行期变化（hotkey_list 可以随时增删），所以不再用固定数组；
 * 这个结构只在热键线程上访问，不需要加锁。
 */
struct RegisteredHotkey {
    int id = 0;                    /**< RegisterHotKey 的 id + WM_HOTKEY 的 wParam，从 1 起。 */
    bool registered = false;       /**< RegisterHotKey 是否成功。 */
    std::uint32_t modifiers = 0;   /**< hotkey::kMod* 位集合（注册时再转 Win32 MOD_*）。 */
    std::uint32_t virtualKey = 0;  /**< Win32 VK 数值。 */
    std::string label;             /**< 规范化显示名，如 "Ctrl+Alt+S"（日志用）。 */
    std::string action;            /**< 条目 id（日志用），如 "capture" / "long_capture.start"。 */
    std::string topic;             /**< 要发布的事件主题。 */
    std::string payload;           /**< 事件负载；空串 = 空负载。 */
};

/**
 * @brief HotkeyWindow 的实现：窗口句柄、线程握手、注册表都在这里。
 *
 * 声明集中在这个结构里，定义按职责分散到三个 .cpp（STYLE.md §6.7 / §6.8）。
 */
struct HotkeyWindow::Impl {
    // ---- start() / stop() 跨线程共享的状态（mutex 保护） ----
    std::thread thread;                /**< 热键线程；joinable 表示 start 成功过。 */
    std::mutex mutex;                  /**< 保护 hwnd / acceptingEvents / reapplyPending。 */
    HWND hwnd = nullptr;               /**< 隐藏消息窗口；WM_HOTKEY 与唤醒消息都投给它。 */
    bool acceptingEvents = false;      /**< start 拉起线程后为 true；stop 一开始就置 false。 */
    bool reapplyPending = false;       /**< 收到过重注册请求（热键线程取走并清零）。 */
    bool suspendPending = false;       /**< 收到过暂停/恢复请求（热键线程取走并清零）。 */
    bool suspendValue = false;         /**< 与 suspendPending 成对：true = 暂停，false = 恢复。 */

    // ---- start() 与热键线程之间"窗口建好了没有"的握手 ----
    std::mutex readyMutex;             /**< 保护 ready / failed。 */
    std::condition_variable readyCv;   /**< start 等待线程就绪。 */
    bool ready = false;                /**< 窗口已创建、启动时的热键注册已完成。 */
    bool failed = false;               /**< 初始化失败，start 应返回 false。 */

    const ShotHostServices* host = nullptr; /**< 宿主服务表；生命周期覆盖插件实例。 */
    HotkeyConfig* config = nullptr;         /**< 配置；只在热键线程上读。 */
    HotkeyCatalog* catalog = nullptr;       /**< hotkey_list 快照来源；只在热键线程上读。 */
    bool busSubscribed = false;             /**< 是否成功订阅过事件总线。 */

    // ---- 热键注册表（只在热键线程上访问） ----
    std::vector<RegisteredHotkey> hotkeys; /**< 当前这批注册，下标不代表 id。 */
    bool suspended = false; /**< true = 设置页正在录入组合键：全部注册已撤销，不重新注册。 */

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

    /**
     * @brief 任意线程可调：请求暂停/恢复全部全局热键注册。
     *
     * 设置页录入组合键时会发 hotkey.suspend（撤掉注册，避免用户按下的"旧组合键"
     * 先触发真实动作），录入结束后发 hotkey.resume 恢复。这里只置标记 + PostMessage，
     * RegisterHotKey / UnregisterHotKey 仍然只在热键线程上执行。
     *
     * @param[in] suspend true = 暂停（撤销全部注册），false = 恢复（按当前配置注册）。
     */
    void requestSuspend(bool suspend);

    /**
     * @brief 热键线程上执行：暂停（撤销全部注册）或恢复（按当前配置重新注册）。
     * @param[in] suspend true = 暂停，false = 恢复；重复调用是幂等的。
     */
    void setSuspended(bool suspend);

    // ---- 热键注册与事件发布（hotkey_window_register.cpp） ----
    void applyHotkeys();
    void unregisterAllHotkeys();
    bool registerOne(int id, const Binding& binding, const std::string& action,
                     const std::string& topic, const std::string& payload);
    void publishHotkey(int id);
};

}  // namespace hotkey
