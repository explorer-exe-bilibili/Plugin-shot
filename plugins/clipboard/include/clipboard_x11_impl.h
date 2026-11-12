#pragma once

// ============================================================================
//  include/clipboard_x11_impl.h —— 内部头：ClipboardX11::Impl 与 X11 线程状态
//
//  **不是插件对外接口**：只有本插件的 X11 相关 .cpp 包含它。公共头
//  clipboard_x11.h 不包含任何 Xlib 类型；这里出现 Display / Window / Atom，
//  因为 X11 线程确实要持有它们（与 notify 的 Windows 内部头同一做法）。
//
//  定义拆成两个 .cpp：
//    clipboard_x11.cpp           —— 生命周期 / 线程握手 / 事件循环 / 取得所有权
//    clipboard_x11_selection.cpp —— SelectionRequest / TARGETS / MULTIPLE / 自检
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <X11/Xlib.h>

#include "clipboard_image.h"
#include "clipboard_x11.h"

namespace clipboard {

/**
 * @brief ClipboardX11 的实现：一个常驻 X11 线程 + 最近一次剪贴板内容。
 *
 * 为什么大部分字段只在 X11 线程访问：Xlib 的 Display 不是可重入对象，所有协议
 * 处理都放在同一个线程里；主线程只通过 commandMutex 下的 pending 负载和一根
 * 唤醒管道跟它通信。
 */
struct ClipboardX11::Impl {
    // ---- start() / stop() 之间共享 ----
    const ShotHostServices* host = nullptr;  /**< 宿主服务表，只用于日志；生命周期覆盖本对象。 */
    std::thread thread;                      /**< X11 线程；joinable 表示 start 成功过。 */
    std::mutex lifecycleMutex;               /**< 让 start/stop 串行（宿主不会并发调，防御性加锁）。 */
    std::atomic<bool> running{false};        /**< false = 事件循环应退出。 */
    int wakePipe[2] = {-1, -1};              /**< 主线程 → X11 线程的唤醒管道（poll 用）。 */

    // ---- 线程就绪握手（start 等待，线程里写入） ----
    std::mutex stateMutex;               /**< 保护 ready / failed / failure。 */
    std::condition_variable readyCv;     /**< start 等待 X11 线程就绪。 */
    bool ready = false;                  /**< Display、窗口与 atom 已准备好。 */
    bool failed = false;                 /**< 初始化失败，start 应返回 false。 */
    std::string failure;                 /**< failed 时的中文原因。 */

    // ---- 主线程 → X11 线程的发布队列（只保留最新一次截图） ----
    std::mutex commandMutex;             /**< 保护 pending / hasPending / accepting。 */
    ClipboardImage::Payload pending;     /**< 等待 X11 线程接管的负载。 */
    bool hasPending = false;             /**< pending 是否有效。 */
    bool accepting = false;              /**< stop 开始后拒绝新负载。 */

    // ---- 以下字段只在 X11 线程访问 ----
    Display* display = nullptr;          /**< 本插件自己的 X11 连接（只在本线程使用）。 */
    Window window = 0;                   /**< 隐藏的 owner 窗口；不 map，只用来挂 selection。 */
    Atom clipboardAtom = None;           /**< CLIPBOARD（不是 PRIMARY）。 */
    Atom targetsAtom = None;             /**< TARGETS。 */
    Atom timestampAtom = None;           /**< TIMESTAMP。 */
    Atom multipleAtom = None;            /**< MULTIPLE。 */
    Atom pngAtom = None;                 /**< image/png。 */
    Atom uriListAtom = None;             /**< text/uri-list。 */
    Atom timeAtom = None;                /**< 内部用：借 PropertyNotify 取服务器时间。 */
    Atom selfCheckAtom = None;           /**< 内部用：TARGETS 自检的 property。 */
    Time ownershipTime = 0;              /**< 取得所有权时的 server timestamp。 */
    bool owned = false;                  /**< 当前是否仍拥有 CLIPBOARD。 */
    bool awaitingSelfCheck = false;      /**< 已发出 TARGETS 自检，等 SelectionNotify。 */
    ClipboardImage::Payload payload;     /**< 当前正在提供的数据。 */
    bool hasPayload = false;             /**< payload 是否有效（SelectionClear 后清空）。 */

    // ---- 生命周期（clipboard_x11.cpp） ----
    bool setup();                        /**< 打开 Display、建 owner 窗口与 atom；失败写 failure。 */
    void run();                          /**< 线程入口：setup → 事件循环 → 清理。 */
    void eventLoop();                    /**< poll(X fd + wakePipe) + 处理 X 事件。 */
    void shutdownDisplay();              /**< 放弃所有权、销毁窗口、关闭 Display。 */
    void notifyReady();                  /**< 置 ready 并唤醒 start。 */
    void notifyFailed(const std::string& reason);  /**< 置 failed + reason 并唤醒 start。 */
    bool takePending(ClipboardImage::Payload& out);  /**< 取走最新负载；没有则返回 false。 */
    bool claim(const ClipboardImage::Payload& next);  /**< 设为 owner 并发起 TARGETS 自检。 */
    void releaseSelection();             /**< 主动放弃所有权（退出时）。 */
    void releasePayload();               /**< 清掉当前提供的字节。 */
    void wake();                         /**< 往唤醒管道写一个字节。 */
    void drainWakePipe();                /**< 读完唤醒管道里的字节。 */
    void closeWakePipe();                /**< 关闭管道两端；重复调用安全。 */

    // ---- selection 协议（clipboard_x11_selection.cpp） ----
    void handleEvent(const XEvent& event);  /**< 按事件类型分发。 */
    void handleSelectionRequest(const XSelectionRequestEvent& request);  /**< 应答粘贴请求。 */
    void handleSelectionClear(const XSelectionClearEvent& event);  /**< 别人抢走所有权。 */
    void handleSelectionNotify(const XSelectionEvent& event);  /**< 收到自检结果。 */
    bool writeTarget(Window requestor, Atom property, Atom target);  /**< 写一种 target 的属性。 */
    bool writeMultiple(const XSelectionRequestEvent& request);  /**< 处理 MULTIPLE 原子对。 */
    void sendSelectionNotify(const XSelectionRequestEvent& request, Atom property);  /**< 回 SelectionNotify。 */
    Time serverTime();                   /**< 借 PropertyNotify 取一个真实 server timestamp。 */
};

}  // namespace clipboard
