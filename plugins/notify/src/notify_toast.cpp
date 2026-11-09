// NotifyToast 的生命周期与线程握手：start / stop。
//
// 线程模型（对应 document/EVENT_BUS.md）：
//   · 本插件只实现 shot.configurable，没有 ui_provider 的 init/deinit/post_to_plugin；
//     宿主的 create(index, host) 是唯一能拿到服务表的时机，所以订阅在 start()（实例
//     创建路径）里建立、退订在 stop()（destroy 路径）里做；
//   · 事件总线回调运行在宿主分发线程上，只做"拷进队列 + PostMessage 唤醒"，
//     所有 Win32/Shell 操作都在本插件的通知线程上执行；
//   · start 先订阅再起线程：订阅之后到达的 capture.done / capture.failed 不会漏，
//     线程还没就绪时事件先入队，消息循环启动后统一 drain；
//   · stop 顺序有讲究：先退订（destroy 返回后宿主不再回调）→ 拒绝新投递 →
//     投递退出消息 → join。隐藏图标删除与窗口销毁在通知线程收尾做，保证 Shell
//     删除图标时窗口句柄一定还有效。
//
// 本插件没有 post_to_plugin，收不到 host.shutdown；订阅 app.quit 事件提前收线程，
// destroy 路径的 stop() 仍是最后兜底（宿主卸载插件前一定会调用 destroy）。

#include "notify_toast.h"

#include <string>

#include "notify_toast_impl.h"
#include "shot_plugin_helpers.h"

namespace notify {

NotifyToast::NotifyToast() : m_impl(std::make_unique<Impl>()) {}

NotifyToast::~NotifyToast() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool NotifyToast::start(const ShotHostServices* host, NotifyConfig* config) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->thread.joinable()) {
        return true;  // 已经在跑：重复调用是空操作（宿主包装类本身也是幂等的）
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = true;
        m_impl->hwnd = nullptr;
        std::queue<QueuedEvent>().swap(m_impl->queue);
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->readyMutex);
        m_impl->ready = false;
        m_impl->failed = false;
    }
    m_impl->host = host;
    m_impl->config = config;

    shot::EventBus bus(host);
    if (!bus.available()) {
        // 老宿主没有事件总线：收不到任何截屏结果，本插件安静降级为不可用。
        shot::Logger(host).warn("[notify] 宿主未提供事件总线（旧版宿主？），消息提示不可用");
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
        return false;
    }
    bool subscribedAny = false;
    subscribedAny =
        bus.subscribe(kTopicCaptureDone, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
    subscribedAny =
        bus.subscribe(kTopicCaptureFailed, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
    // app.quit：托盘"退出"时提前收线程，不等宿主走完卸载流程；收不到也不影响正确性。
    subscribedAny =
        bus.subscribe(kTopicAppQuit, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
    m_impl->busSubscribed = subscribedAny;
    if (!subscribedAny) {
        shot::Logger(host).warn("[notify] 订阅 capture.done / capture.failed 失败，消息提示不可用");
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
        return false;
    }

    try {
        m_impl->thread = std::thread(&Impl::run, m_impl.get());
    } catch (...) {
        // 线程资源耗尽等：异常绝不跨越 DLL 边界，把已做的订阅撤回。
        shot::EventBus unsubscribeBus(host);
        unsubscribeBus.unsubscribe(m_impl.get());
        m_impl->busSubscribed = false;
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

void NotifyToast::stop() {
    if (!m_impl) {
        return;
    }
    // 1. 先退订事件总线：destroy 返回后宿主不再回调本插件（EVENT_BUS.md §5）。
    if (m_impl->busSubscribed) {
        shot::EventBus bus(m_impl->host);
        bus.unsubscribe(m_impl.get());
        m_impl->busSubscribed = false;
    }
    // 2. 拒绝后续投递（在途的总线回调也会在这里止步）。
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
        wnd = m_impl->hwnd;
    }
    // 3. 让通知线程退出消息循环，然后 join：隐藏图标删除与 DestroyWindow 在线程
    //    收尾做，返回时图标与线程一定都已经收干净。
    if (wnd != nullptr) {
        PostMessageW(wnd, kMsgQuit, 0, 0);
    }
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->hwnd = nullptr;
        std::queue<QueuedEvent>().swap(m_impl->queue);
    }
}

}  // namespace notify
