#pragma once

// ============================================================================
//  include/notify_toast_impl.h —— 内部头：NotifyToast 的 PIMPL 与通知线程内部类型
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。这里会出现 Win32 类型
//  （HWND / HICON），因为通知线程确实要持有它们；公共头 notify_toast.h 不包含
//  <windows.h>，宿主与其它插件看不到任何 Win32 句柄。
//
//  定义拆成四个 .cpp：
//    notify_toast.cpp        —— 生命周期 / 线程握手 / 事件订阅（start / stop）
//    notify_toast_window.cpp —— 隐藏窗口 / 消息循环 / 事件出队 / 隐藏占位图标
//    notify_toast_balloon.cpp—— 气泡文本（标题/正文）与发送、UTF-8 → UTF-16
//    notify_toast_click.cpp  —— 用户点击通知：打开图片 / 打开所在文件夹
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX  // 防止 <windows.h> 的 min/max 宏把 std::min / std::max 顶掉
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include "notify_config.h"
#include "notify_toast.h"
#include "plugin_api.h"

namespace notify {

// ---------------------------------------------------------------------------
//  事件主题（document/EVENT_BUS.md / CAPTURE.md 的约定：全小写、点分隔）
// ---------------------------------------------------------------------------

/** @brief 订阅：截屏落盘成功（JSON {"path","width","height","processors"}）。 */
inline constexpr char kTopicCaptureDone[] = "capture.done";
/** @brief 订阅：截屏某个阶段失败（JSON {"stage","reason"}）。 */
inline constexpr char kTopicCaptureFailed[] = "capture.failed";
/**
 * @brief 订阅：宿主即将退出（托盘"退出"菜单发布）。
 *
 * 本插件没有 ui_provider，收不到 post_to_plugin 的 host.shutdown；用 app.quit
 * 事件提前收线程，destroy 路径的 stop() 仍然是最后兜底。
 */
inline constexpr char kTopicAppQuit[] = "app.quit";

// ---------------------------------------------------------------------------
//  隐藏消息窗口的自定义消息
// ---------------------------------------------------------------------------

/** @brief 队列里来了新事件，唤醒阻塞在 GetMessage 里的通知线程。 */
inline constexpr UINT kMsgDrain = WM_APP + 1;
/** @brief 请求退出消息循环（stop() 或 app.quit）。 */
inline constexpr UINT kMsgQuit = WM_APP + 2;
/**
 * @brief 隐藏占位图标的 Shell 回调消息（NOTIFYICONDATA::uCallbackMessage）。
 *
 * Shell 把图标/气泡事件（lParam 低位是 NIN_*）投到这条自定义消息上；本插件只关心
 * NIN_BALLOONUSERCLICK（用户点了通知）。不注册这条消息就永远收不到点击，气泡只是
 * 一条拦截不到输入的横幅。
 */
inline constexpr UINT kMsgNotifyIconEvent = WM_APP + 3;

/** @brief 隐藏托盘图标的 id：只用于挂气泡，通知区域不显示。 */
inline constexpr UINT kNotifyIconId = 2;

/** @brief 队列里的一个事件：主题名 + 负载字节（总线单条负载上限 64 KiB）。 */
struct QueuedEvent {
    std::string name;     /**< 主题/事件名，如 "capture.done"。 */
    std::string payload;  /**< 负载字节；不保证以 NUL 结尾（按长度解释）。 */
};

/**
 * @brief 把 UTF-8 文本转成 Win32 需要的 UTF-16；失败返回空串。
 * @param[in] utf8 UTF-8 文本（可含中文）；空串直接返回空串。
 * @return UTF-16 文本；输入不是合法 UTF-8 时返回空串，调用方回落成通用提示。
 * @note 跨 TU 使用（窗口标题 / 气泡正文 / 通知文本都在别的 .cpp 里），所以不是在
 *       匿名命名空间里的私有辅助函数；定义在 notify_toast_balloon.cpp。
 */
std::wstring toWide(const std::string& utf8);

/**
 * @brief NotifyToast 的实现：线程握手、事件队列、隐藏窗口与隐藏图标都在这里。
 *
 * 声明集中在这个结构里，定义按职责分散到四个 .cpp（STYLE.md §6.7 / §6.8）。
 */
struct NotifyToast::Impl {
    // ---- start() / stop() 跨线程共享的状态（mutex 保护） ----
    std::thread thread;              /**< 通知线程；joinable 表示 start 成功过。 */
    std::mutex mutex;                /**< 保护事件队列 / hwnd / 入队开关。 */
    std::queue<QueuedEvent> queue;   /**< 尚未处理的宿主事件。 */
    HWND hwnd = nullptr;             /**< 隐藏消息窗口；唤醒消息都投给它。 */
    bool acceptingEvents = false;    /**< start 拉起线程后为 true；stop 一开始就置 false。 */

    // ---- start() 与通知线程之间"窗口 + 隐藏图标就绪了没有"的握手 ----
    std::mutex readyMutex;           /**< 保护 ready / failed。 */
    std::condition_variable readyCv; /**< start 等待线程就绪。 */
    bool ready = false;              /**< 隐藏窗口与占位图标已加好。 */
    bool failed = false;             /**< 初始化失败，start 应返回 false。 */

    const ShotHostServices* host = nullptr; /**< 宿主服务表；生命周期覆盖插件实例。 */
    NotifyConfig* config = nullptr;         /**< 开关配置；只在通知线程上读。 */
    const shot::I18n* i18n = nullptr;       /**< 语言表；只在通知线程上查。 */
    bool busSubscribed = false;             /**< 是否成功订阅过事件总线。 */

    // ---- 隐藏窗口与占位图标（只在通知线程上访问） ----
    bool iconAdded = false;             /**< Shell_NotifyIconW(NIM_ADD) 是否成功过。 */
    UINT taskbarCreatedMessage = 0;     /**< "TaskbarCreated" 注册消息；explorer 重启后重加图标。 */
    std::wstring lastImagePath;         /**< 最近一次**成功**通知的图片路径；点击通知时打开它。 */

    // ---- 生命周期与线程（notify_toast.cpp） ----
    void run();
    void messageLoop();
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);

    // ---- 隐藏窗口 / 消息 / 隐藏图标（notify_toast_window.cpp） ----
    void enqueue(const char* topic, const char* data, std::uint32_t len);
    void drainEvents();
    bool ensureWindowClass(HINSTANCE instance);
    static bool registerWindowClass(HINSTANCE instance);
    void unregisterWindowClass(HINSTANCE instance);
    bool addIcon(HWND wnd);
    void removeIcon(HWND wnd);
    LRESULT handleMessage(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);

    // ---- 气泡文本与发送（notify_toast_balloon.cpp） ----
    void onCaptureDone(const std::string& payload);
    void onCaptureFailed(const std::string& payload);
    const char* localized(const char* key, const char* fallback) const;
    /**
     * @brief 取最终通知标题：配置了 `title` 就用展开后的模板，否则用默认多语言标题。
     * @param[in] defaultKey      默认标题的 i18n key（成功/失败各一条）。
     * @param[in] defaultFallback 语言表不可用时的中文原文。
     * @param[in] path            图片路径（UTF-8）；失败通知传空串。
     * @param[in] width           图片宽度（像素）；未知传 0。
     * @param[in] height          图片高度（像素）；未知传 0。
     * @return UTF-16 标题；配置为空、展开为空或不是合法 UTF-8 时返回空串（调用方回落）。
     */
    std::wstring configuredTitle(const char* defaultKey, const char* defaultFallback,
                                 const std::string& path, int width, int height) const;
    void showBalloon(const wchar_t* title, const wchar_t* text, DWORD infoFlags);

    // ---- 通知点击行为（notify_toast_click.cpp） ----
    void onNotifyIconEvent(LPARAM lparam);
    void openLastImage();
    void openImageFile();
    void openContainingFolder();
};

}  // namespace notify
