#pragma once

// ============================================================================
//  include/hotkey_window_x11_impl.h —— 内部头：Linux/X11 下的 HotkeyWindow::Impl
//
//  **不是插件对外接口**：只有本插件的 X11 .cpp 包含它。公共头 hotkey_window.h
//  不含任何 Xlib 类型；这里出现 Display / Window / KeyCode，因为热键线程确实要
//  持有它们（与 plugins/clipboard 的 clipboard_x11_impl.h 同一做法）。
//
//  定义按职责拆成三个 .cpp：
//    hotkey_window_x11.cpp          —— 生命周期 / 线程握手 / 请求分发
//    hotkey_window_x11_loop.cpp     —— 事件循环（poll + self-pipe）与唤醒管道
//    hotkey_window_x11_register.cpp —— keysym 映射 / XGrabKey / 锁修饰符 / 事件发布
//
//  线程模型与 Windows 实现一致：宿主只调 start / stop / post / requestReapply；
//  事件总线回调运行在宿主分发线程上，只置 pending 并写唤醒管道；XGrabKey /
//  XUngrabKey 全部在热键线程上执行——Xlib 的 Display 不是可重入对象，本插件的
//  连接与抓取只在这一个线程里用。
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <X11/Xlib.h>

#include "hotkey_binding.h"
#include "hotkey_topics.h"
#include "hotkey_window.h"
#include "plugin_api.h"

namespace hotkey {

class HotkeyCatalog;
class HotkeyConfig;

/**
 * @brief 一个已成功 XGrabKey 的热键（容器里只放成功的项，所以没有 registered 标志）。
 *
 * 只在热键线程上访问，不需要加锁。
 */
struct X11Hotkey {
    unsigned int keycode = 0;   /**< X11 keycode（XKeysymToKeycode 的结果）。 */
    unsigned int modifiers = 0; /**< 基准修饰位（不含锁变体）；事件匹配时按它比较。 */
    std::string label;          /**< 规范化显示名，如 "Ctrl+Alt+S"（日志用）。 */
    std::string action;         /**< 条目 id（日志用），如 "capture"。 */
    std::string topic;          /**< 要发布的事件主题。 */
    std::string payload;        /**< 事件负载；空串 = 空负载。 */
};

/**
 * @brief HotkeyWindow 在 Linux/X11 下的实现：一个 Display + 一个热键线程。
 *
 * 字段与 Windows 的 Impl 一一对应（线程握手 / pending 标记 / 注册表），平台相关
 * 的部分换成 Display、root window 与锁修饰位列表。
 */
struct HotkeyWindow::Impl {
    // ---- start() / stop() 跨线程共享的状态（mutex 保护） ----
    std::thread thread;           /**< 热键线程；joinable 表示 start 成功过。 */
    std::mutex mutex;             /**< 保护 acceptingEvents / pending 标记 / quitting。 */
    bool acceptingEvents = false; /**< start 拉起线程后为 true；stop 一开始就置 false。 */
    bool reapplyPending = false;  /**< 收到过重注册请求（热键线程取走并清零）。 */
    bool suspendPending = false;  /**< 收到过暂停/恢复请求（热键线程取走并清零）。 */
    bool suspendValue = false;    /**< 与 suspendPending 成对：true = 暂停，false = 恢复。 */
    bool quitting = false;        /**< stop() 请求退出事件循环。 */

    // ---- start() 与热键线程之间"X11 准备好了没有"的握手 ----
    std::mutex readyMutex;           /**< 保护 ready / failed。 */
    std::condition_variable readyCv; /**< start 等待线程就绪。 */
    bool ready = false;              /**< 连接已建立、启动时的热键注册已完成（或已降级）。 */
    bool failed = false;             /**< 线程初始化异常，start 应返回 false。 */

    // ---- 唤醒管道（主线程写 / 热键线程 poll 读） ----
    int wakePipe[2] = {-1, -1}; /**< 之所以不用 XPending 定时轮询：空闲时零 CPU，也没有魔数超时。 */

    const ShotHostServices* host = nullptr; /**< 宿主服务表；生命周期覆盖插件实例。 */
    HotkeyConfig* config = nullptr;         /**< 配置；只在热键线程上读。 */
    HotkeyCatalog* catalog = nullptr;       /**< hotkey_list 快照来源；只在热键线程上读。 */
    bool busSubscribed = false;             /**< 是否成功订阅过事件总线。 */

    // ---- 以下字段只在热键线程访问 ----
    Display* display = nullptr;              /**< 本插件自己的 X11 连接（只在本线程使用）。 */
    Window root = 0;                         /**< 抓取窗口（默认 screen 的 root）。 */
    std::vector<unsigned int> lockMasks;     /**< CapsLock/NumLock/ScrollLock 的修饰位。 */
    std::vector<X11Hotkey> hotkeys;          /**< 当前这批抓取，下标不代表序号。 */
    bool suspended = false;                  /**< true = 设置页正在录入：全部撤销，不重新抓。 */

    // ---- 生命周期与线程握手（hotkey_window_x11.cpp） ----
    void run();
    void runBody();
    bool openDisplay();
    void closeDisplay();
    void computeLockMasks();
    bool processRequests();
    bool applyPendingSuspend();
    void requestReapply();
    void requestSuspend(bool suspend);
    void setSuspended(bool suspend);
    void setAccepting(bool accepting);
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);

    // ---- 事件循环与唤醒管道（hotkey_window_x11_loop.cpp） ----
    bool createWakePipe();
    void closeWakePipe();
    void wake();
    void drainWakePipe();
    void eventLoop();
    void handleEvent(const XEvent& event);

    // ---- 热键注册与事件发布（hotkey_window_x11_register.cpp） ----
    void applyHotkeys();
    void ungrabAllHotkeys();
    std::vector<unsigned int> lockVariants() const;
    bool grabHotkey(const Binding& binding, unsigned int keycode, const std::string& action,
                    const std::string& topic, const std::string& payload);
    void publishHotkey(unsigned int keycode, unsigned int state);
};

}  // namespace hotkey
