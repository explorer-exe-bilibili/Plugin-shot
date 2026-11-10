// HotkeyWindow 的生命周期与线程握手：start / stop / post / requestReapply。
//
// 线程模型（对应 document/EVENT_BUS.md）：
//   · 宿主只调 init / deinit / post_to_plugin，从不接触 HWND；
//   · 事件总线回调运行在宿主分发线程上，只置"需要重注册 / 需要暂停"标记 + PostMessage，
//     解析与 RegisterHotKey / UnregisterHotKey 全部在热键线程上执行；
//   · start 先订阅 config.applied / hotkey.suspend / hotkey.resume 再起线程：订阅之后
//     到达的通知不会漏，线程还没就绪时只置 pending，窗口建好后统一处理；
//   · stop 顺序：拒绝新投递 → 让热键线程撤掉全部注册并退出消息循环 → join →
//     退订事件总线。返回时热键、窗口与线程一定都已经收干净。

#include "hotkey_window.h"

#include <cstring>
#include <string>
#include <utility>

#include "hotkey_config.h"
#include "hotkey_window_impl.h"
#include "shot_plugin_helpers.h"

namespace hotkey {

HotkeyWindow::HotkeyWindow() : m_impl(std::make_unique<Impl>()) {}

HotkeyWindow::~HotkeyWindow() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool HotkeyWindow::start(const ShotHostServices* host, HotkeyConfig* config) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->thread.joinable()) {
        return true;  // 已经在跑：重复 init 是空操作（宿主包装类本身也是幂等的）
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = true;
        m_impl->hwnd = nullptr;
        m_impl->reapplyPending = false;
        m_impl->suspendPending = false;
        m_impl->suspendValue = false;
    }
    // suspended 只在热键线程上读写；这里线程还没起来（stop 已 join 过），可以直接复位。
    m_impl->suspended = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->readyMutex);
        m_impl->ready = false;
        m_impl->failed = false;
    }
    m_impl->host = host;
    m_impl->config = config;

    // 先订阅再起线程：订阅之后到达的 config.applied 会被接住，不会因为线程没就绪而丢。
    // hotkey.suspend / hotkey.resume 是设置页录入组合键时的"暂停/恢复"握手（见
    // plugins/settings_ui 的录入控件）：一个主题失败不影响另外两个，退订按 user 一次清掉。
    shot::EventBus bus(host);
    if (bus.available()) {
        bool subscribedAny =
            bus.subscribe(kTopicConfigApplied, &Impl::onBusEvent, m_impl.get());
        subscribedAny =
            bus.subscribe(kTopicHotkeySuspend, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicHotkeyResume, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        m_impl->busSubscribed = subscribedAny;
        if (!subscribedAny) {
            shot::Logger(host).warn(
                "[hotkey] 订阅事件失败（config.applied / hotkey.suspend / hotkey.resume），"
                "配置变化与设置页录入都不会自动生效");
        }
    } else {
        // 旧版宿主没有事件总线：热键仍然按配置注册，只是"应用配置后自动生效"不可用；
        // 重新启用插件或重启宿主时会重新读取配置，所以不算致命。
        shot::Logger(host).warn("[hotkey] 宿主未提供事件总线（旧版宿主？），配置变化只在重新加载时生效");
    }

    try {
        m_impl->thread = std::thread(&Impl::run, m_impl.get());
    } catch (...) {
        // 线程资源耗尽等：异常绝不跨越 DLL 边界，把已做的订阅撤回。
        if (m_impl->busSubscribed) {
            shot::EventBus unsubscribeBus(host);
            unsubscribeBus.unsubscribe(m_impl.get());
            m_impl->busSubscribed = false;
        }
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(m_impl->readyMutex);
        m_impl->readyCv.wait(lock, [this]() { return m_impl->ready || m_impl->failed; });
        if (!m_impl->failed) {
            return true;
        }
    }
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();  // 已经 failed 返回了，这里不会阻塞
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
    }
    return false;
}

void HotkeyWindow::stop() {
    if (!m_impl) {
        return;
    }
    // 1. 拒绝后续投递（post_to_plugin / 在途的总线回调都会在这里止步）。
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
        m_impl->reapplyPending = false;
        wnd = m_impl->hwnd;
    }
    // 2. 让热键线程先撤掉全部 RegisterHotKey，再退出消息循环并 join
    //    （撤销必须在注册它的线程上做；窗口/类的收尾在线程 run() 里做）。
    if (wnd != nullptr) {
        PostMessageW(wnd, kMsgQuit, 0, 0);
    }
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    // 3. 最后退订事件总线：deinit 返回前必须完成（EVENT_BUS.md §5）。
    if (m_impl->busSubscribed) {
        shot::EventBus bus(m_impl->host);
        bus.unsubscribe(m_impl.get());
        m_impl->busSubscribed = false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->hwnd = nullptr;
    }
}

void HotkeyWindow::requestReapply() {
    if (!m_impl) {
        return;
    }
    // 调用方可能是宿主 main 线程（config_set 回调 / 事件总线回调），这里只置标记
    // + PostMessage；解析与注册全部留给热键线程执行。
    try {
        m_impl->requestReapply();
    } catch (...) {
        // 分配失败等：下次配置变化或重新加载时还会再试，异常绝不越过 DLL 边界。
    }
}

std::int32_t HotkeyWindow::post(const char* name, const char* data, std::uint32_t len) {
    if (!m_impl || name == nullptr) {
        return -1;
    }
    // 宿主卸载 UI 插件前先投 host.shutdown 再 deinit（SETTINGS.md §3.1）：收到就
    // 立刻把线程停掉，不等 deinit——deinit 里的 stop() 因此变成幂等空操作。
    if (std::strcmp(name, kEventHostShutdown) == 0) {
        stop();
        return 0;
    }
    bool accepting = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        accepting = m_impl->acceptingEvents;
    }
    if (!accepting) {
        return 1;  // 热键线程还没就绪或正在关闭：丢弃
    }
    // 热键插件不需要 post 通道做业务：配置走 config.applied、退出走 host.shutdown，
    // 其它事件（如果有）只记 DEBUG，避免在日志里刷不存在的问题。
    shot::Logger(m_impl->host)
        .debug(std::string("[hotkey] 忽略 post_to_plugin 事件 ") + name + "（负载 " +
               std::to_string(len) + " 字节）");
    (void)data;
    return 0;
}

}  // namespace hotkey
