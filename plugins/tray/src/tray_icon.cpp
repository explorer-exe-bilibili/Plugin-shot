// TrayIcon 的生命周期与线程握手：start / stop / post。
//
// 线程模型（对应 document/EVENT_BUS.md）：
//   · 宿主只调 init / deinit / post_to_plugin，从不接触 HWND；
//   · 事件总线回调运行在宿主分发线程上，只做"拷进队列 + PostMessage 唤醒"，
//     所有 Win32/托盘操作都在托盘线程上执行；
//   · start 先订阅再起线程：订阅之后到达的 lists.changed / config.applied 不会漏，
//     线程还没就绪时事件先入队，消息循环启动后统一 drain；
//   · stop 顺序有讲究：先退订（deinit 返回后宿主不再回调）→ 拒绝新投递 →
//     投递退出消息 → join。NIM_DELETE 与 DestroyWindow 在托盘线程收尾做，
//     这样 Shell 删除图标时窗口句柄一定还有效。

#include "tray_icon.h"

#include <cstring>
#include <string>
#include <utility>

#include "shot_plugin_helpers.h"
#include "tray_icon_impl.h"

namespace tray {

TrayIcon::TrayIcon() : m_impl(std::make_unique<Impl>()) {}

TrayIcon::~TrayIcon() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool TrayIcon::start(const ShotHostServices* host, TrayConfig* config, shot::I18n* i18n) {
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
        std::queue<QueuedEvent>().swap(m_impl->queue);
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->readyMutex);
        m_impl->ready = false;
        m_impl->failed = false;
    }
    m_impl->host = host;
    m_impl->config = config;
    m_impl->i18n = i18n;
    // 悬停提示是用户可见文案：按当前语言生成一次，图标 (重新) 添加时用。
    const std::string tooltip =
        (i18n != nullptr) ? i18n->tr("tray.tooltip", kTooltipFallback) : kTooltipFallback;
    m_impl->tooltip = m_impl->toWide(tooltip);

    // 先订阅再起线程：订阅之后到达的事件会被队列接住，不会因为线程没就绪而丢。
    shot::EventBus bus(host);
    if (bus.available()) {
        bool subscribedAny = false;
        // 菜单数据是宿主列表服务里的运行期快照：列表一变（插件启停 / lists.add）就重建。
        subscribedAny =
            bus.subscribe(kTopicListsChanged, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        // 设置页应用后宿主发 config.applied：重读 icon 配置，必要时换图标。
        subscribedAny =
            bus.subscribe(kTopicConfigApplied, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        m_impl->busSubscribed = subscribedAny;
        if (!subscribedAny) {
            shot::Logger(host).warn(
                "[tray] 订阅 lists.changed/config.applied 失败，菜单与图标刷新不可用");
        }
    } else {
        // 旧版宿主没有事件总线：托盘图标照常出现，只是菜单/图标不会随运行期数据刷新。
        shot::Logger(host).warn("[tray] 宿主未提供事件总线（旧版宿主？），托盘只显示不刷新");
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

void TrayIcon::requestIconRefresh() {
    if (!m_impl) {
        return;
    }
    // 与 post() 相同的线程纪律：调用方可能是宿主 main 线程（config_set），这里只
    // 入队 + PostMessage；LoadImageW / NIM_MODIFY 全部留给托盘线程执行。
    try {
        m_impl->enqueue(kEventRefreshIcon, nullptr, 0);
    } catch (...) {
        // 分配失败等：图标刷新失败不影响托盘其余功能，异常绝不越过 DLL 边界。
    }
}

void TrayIcon::stop() {
    if (!m_impl) {
        return;
    }
    // 1. 先退订事件总线：deinit 返回后宿主不再回调本插件（EVENT_BUS.md §5）。
    if (m_impl->busSubscribed) {
        shot::EventBus bus(m_impl->host);
        bus.unsubscribe(m_impl.get());
        m_impl->busSubscribed = false;
    }
    // 2. 拒绝后续投递（post_to_plugin / 在途的总线回调都会在这里止步）。
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
        wnd = m_impl->hwnd;
    }
    // 3. 让托盘线程退出消息循环，然后 join：NIM_DELETE 与 DestroyWindow 在
    //    run() 收尾做，返回时图标与线程一定都已经收干净。
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

std::int32_t TrayIcon::post(const char* name, const char* data, std::uint32_t len) {
    if (!m_impl || name == nullptr) {
        return -1;
    }
    // 宿主卸载 UI 插件前先投 host.shutdown 再 deinit（SETTINGS.md §3.1）：收到就
    // 立刻把线程停掉，不等 deinit——deinit 里的 stop() 因此变成幂等空操作。
    if (std::strcmp(name, kEventHostShutdown) == 0) {
        stop();
        return 0;
    }
    try {
        std::string payload;
        if (data != nullptr && len > 0) {
            payload.assign(data, len);  // 契约要求：返回前把 data_len 字节拷走
        }
        HWND wnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (!m_impl->acceptingEvents) {
                return 1;  // 托盘不在（还没起好 / 正在关闭）：丢弃
            }
            m_impl->queue.push(QueuedEvent{name, std::move(payload)});
            wnd = m_impl->hwnd;
        }
        if (wnd != nullptr && PostMessageW(wnd, kMsgDrain, 0, 0) == 0) {
            return 2;
        }
        return 0;
    } catch (...) {
        return 2;  // 分配失败等：异常绝不跨越 DLL 边界
    }
}

}  // namespace tray
