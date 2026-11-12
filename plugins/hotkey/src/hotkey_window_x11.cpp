// Linux/X11 全局热键的生命周期：起停热键线程、请求分发与线程就绪握手。
//
// 事件循环与唤醒管道在 hotkey_window_x11_loop.cpp：热键线程用 poll(X 连接 fd +
// self-pipe) 阻塞等待（空闲不占 CPU），主线程只置 pending 标记并写一个字节叫醒它。
// 抓取与撤销在 hotkey_window_x11_register.cpp。
//
// 为什么所有 Xlib 调用都在热键线程：Xlib 的 Display 不是可重入对象；宿主进程里
// 还有 GLFW/settings_ui 与 clipboard 插件在用 X11。把本插件的连接、抓取与事件处理
// 全部关在一个线程里，至少保证本插件内部没有并发 Xlib 调用（跨组件的风险见 README）。
//
// 没有可用的 X11 Display（纯 Wayland 会话、没有 DISPLAY）时按降级处理：只打一条
// WARN 指向 session_control 的 D-Bus 入口，start() 仍然返回 true——插件照常提供
// hotkey_list 与配置项，和 hotkey_window_stub.cpp 的行为一致，不让插件消失。
//
// 失败语义：唤醒管道建不出来 / 线程起不来时 start() 返回 false（宿主按"这个 UI
// 插件初始化失败"处理）；X11 连接打不开不算失败，只降级。

#include "hotkey_window_x11_impl.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <X11/keysym.h>

#include "shot_plugin_helpers.h"

namespace hotkey {

HotkeyWindow::HotkeyWindow() : m_impl(std::make_unique<Impl>()) {}

HotkeyWindow::~HotkeyWindow() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool HotkeyWindow::start(const ShotHostServices* host, HotkeyConfig* config,
                         HotkeyCatalog* catalog) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->thread.joinable()) {
        return true;  // 已经在跑：重复 init 是空操作（宿主包装类本身也是幂等的）
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = true;
        m_impl->reapplyPending = false;
        m_impl->suspendPending = false;
        m_impl->suspendValue = false;
        m_impl->quitting = false;
    }
    // suspended 与注册表只在热键线程上读写；这里线程还没起来（stop 已 join 过），
    // 可以直接复位。注册表清空也放在这里，避免上一次失败残留半条记录。
    m_impl->suspended = false;
    m_impl->hotkeys.clear();
    {
        std::lock_guard<std::mutex> lock(m_impl->readyMutex);
        m_impl->ready = false;
        m_impl->failed = false;
    }
    m_impl->host = host;
    m_impl->config = config;
    m_impl->catalog = catalog;

    // 唤醒管道必须在起线程前建好：热键线程靠 poll 它来处理重注册/暂停/退出，建不
    // 出来就只能让 init 失败（没有管道，stop() 会永远等不到线程醒来）。
    if (!m_impl->createWakePipe()) {
        shot::Logger(host).error("[hotkey] 创建热键线程唤醒管道失败，X11 全局热键不可用");
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
        return false;
    }

    // 先订阅再起线程：订阅之后到达的 config.applied 会被接住，不会因为线程没就绪而丢。
    // hotkey.suspend / hotkey.resume 是设置页录入组合键时的"暂停/恢复"握手：一个主题
    // 失败不影响另外两个，退订按 user 一次清掉。
    shot::EventBus bus(host);
    if (bus.available()) {
        bool subscribedAny = bus.subscribe(kTopicConfigApplied, &Impl::onBusEvent, m_impl.get());
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
        shot::Logger(host).warn(
            "[hotkey] 宿主未提供事件总线（旧版宿主？），配置变化只在重新加载时生效");
    }

    try {
        m_impl->thread = std::thread(&Impl::run, m_impl.get());
    } catch (...) {
        // 线程资源耗尽等：异常绝不跨越 DLL 边界，把已做的订阅与管道撤回。
        if (m_impl->busSubscribed) {
            shot::EventBus unsubscribeBus(host);
            unsubscribeBus.unsubscribe(m_impl.get());
            m_impl->busSubscribed = false;
        }
        m_impl->closeWakePipe();
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
    m_impl->closeWakePipe();
    if (m_impl->busSubscribed) {
        shot::EventBus unsubscribeBus(host);
        unsubscribeBus.unsubscribe(m_impl.get());
        m_impl->busSubscribed = false;
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
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
        m_impl->reapplyPending = false;
        m_impl->quitting = true;
    }
    // 2. 叫醒热键线程：它看到 quitting 后自己撤销全部抓取、关连接并退出。
    m_impl->wake();
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    // 管道只能在 join 之后关：线程还在 poll 它的时候关掉，poll 会得到 POLLNVAL。
    m_impl->closeWakePipe();
    // 3. 最后退订事件总线：deinit 返回前必须完成（EVENT_BUS.md §5）。
    if (m_impl->busSubscribed) {
        shot::EventBus bus(m_impl->host);
        bus.unsubscribe(m_impl.get());
        m_impl->busSubscribed = false;
    }
}

void HotkeyWindow::requestReapply() {
    if (!m_impl) {
        return;
    }
    // 调用方可能是宿主 main 线程（config_set 回调 / 事件总线回调），这里只置标记
    // + 写唤醒管道；解析与 XGrabKey 全部留给热键线程执行。
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

// ---- 请求处理：pending 标记的权威状态在 mutex 下，唤醒只是"来取一次"的信号 ----

void HotkeyWindow::Impl::requestReapply() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!acceptingEvents) {
            return;  // 还没启动 / 正在关闭：丢弃，启动时会按最新配置注册
        }
        reapplyPending = true;
    }
    wake();
}

void HotkeyWindow::Impl::requestSuspend(bool suspend) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!acceptingEvents) {
            // 还没启动 / 正在关闭：启动时会按最新配置注册，这里不需要"暂停"。
            return;
        }
        suspendPending = true;
        suspendValue = suspend;
    }
    wake();
}

bool HotkeyWindow::Impl::applyPendingSuspend() {
    // 一次唤醒可能合并了多条暂停/恢复通知：以共享状态里的"最后一次期望"为准。
    // 这样 suspend→resume 紧挨着到达时，最终一定回到"按配置注册"（不会停在暂停）。
    bool pending = false;
    bool want = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending = suspendPending;
        want = suspendValue;
        suspendPending = false;
    }
    if (!pending) {
        return false;
    }
    setSuspended(want);
    return true;
}

bool HotkeyWindow::Impl::processRequests() {
    bool quit = false;
    bool reapply = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        quit = quitting;
        reapply = reapplyPending;
        reapplyPending = false;
    }
    if (quit) {
        return false;
    }
    if (reapply) {
        shot::Logger(host).info("[hotkey] 配置变化，重新解析并注册全部热键");
        applyHotkeys();
    }
    applyPendingSuspend();
    return true;
}

void HotkeyWindow::Impl::setSuspended(bool suspend) {
    if (suspended == suspend) {
        return;  // 幂等：重复的 suspend/resume 不重复撤/抓
    }
    suspended = suspend;
    if (suspend) {
        // 立即撤销：设置页已经进入"等待按键"，此刻用户按下的旧组合键不能再触发动作。
        ungrabAllHotkeys();
        shot::Logger(host).info(
            "[hotkey] 收到 hotkey.suspend：已撤销全部热键（设置页正在录入组合键）");
        return;
    }
    // 恢复：重新解析当前配置再抓取（录入期间配置可能被别处改过，不能复用旧注册表）。
    shot::Logger(host).info("[hotkey] 收到 hotkey.resume：按当前配置重新注册热键");
    applyHotkeys();
}

// ---- 热键线程：打开连接 → 抓取一次 → 阻塞事件循环 → 收尾 ----

void HotkeyWindow::Impl::run() {
    // 线程入口是原生线程帧：异常穿出去会 std::terminate，所以初始化期的分配都兜一层。
    try {
        runBody();
    } catch (...) {
        if (display != nullptr) {
            XCloseDisplay(display);
            display = nullptr;
        }
        setAccepting(false);
        shot::Logger(host).error("[hotkey] 热键线程初始化异常，X11 全局热键不可用");
        std::lock_guard<std::mutex> lock(readyMutex);
        failed = true;
        readyCv.notify_all();
    }
}

void HotkeyWindow::Impl::runBody() {
    if (!openDisplay()) {
        // 纯 Wayland（没有 DISPLAY）或 X11 连接打不开：按降级处理，不让插件失败。
        // 文案与 hotkey_window_stub.cpp 保持一致，方便用户/文档按关键词搜到。
        shot::Logger(host).warn(
            "[hotkey] 没有可用的 X11 Display（Wayland 不支持全局热键），"
            "请用 session_control 的 D-Bus 入口或桌面自定义快捷键");
        setAccepting(false);
        {
            std::lock_guard<std::mutex> lock(readyMutex);
            ready = true;
        }
        readyCv.notify_all();
        return;
    }
    computeLockMasks();

    // 启动时按当前目录快照抓一次：默认值，或宿主 apply_stored 之后更新的已存值。
    // 抓取会分配字符串（动态条目数量/负载不是编译期常量），线程入口不能抛异常。
    try {
        applyHotkeys();
    } catch (...) {
        shot::Logger(host).warn("[hotkey] 启动时注册热键发生异常，本轮按没有热键继续");
    }
    // 订阅先于线程就绪：启动期间到达的 hotkey.suspend 只置了 pending（那时还没有
    // 事件循环可以投递），抓取完补一次，避免"设置页正在录入、热键却先注册上"。
    try {
        applyPendingSuspend();
    } catch (...) {
        shot::Logger(host).warn("[hotkey] 应用暂停/恢复状态失败，已保持启动时的注册状态");
    }
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
    }
    readyCv.notify_all();
    shot::Logger(host).info("[hotkey] 热键线程就绪（X11）");

    eventLoop();

    // 事件循环退出（stop() 或 X 连接断开）：撤掉全部抓取并关连接。连接关闭时服务器
    // 也会释放本 client 的 grab，但显式撤销能让"已退出"这件事在日志与排障里有据可查。
    try {
        ungrabAllHotkeys();
    } catch (...) {
        shot::Logger(host).warn("[hotkey] 线程退出时撤销热键发生异常，继续关闭 X11 连接");
    }
    closeDisplay();
    setAccepting(false);
    shot::Logger(host).info("[hotkey] 热键线程已退出，X11 抓取已全部撤销");
}

bool HotkeyWindow::Impl::openDisplay() {
    display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return false;
    }
    root = DefaultRootWindow(display);
    if (root == 0) {
        XCloseDisplay(display);
        display = nullptr;
        return false;
    }
    // 抓取窗口是 root：被动抓取激活后按键事件投给 grab_window，所以必须在这里选
    // KeyPressMask 才会投递给我们（XSelectInput 只改本 client 的掩码，不影响别人）。
    // TODO: 只抓默认 screen 的 root；多 X screen（非 Xinerama/RandR）的旧式配置下
    //       其它 screen 的按键抓不到，等有真机环境再按 screen 逐个抓。
    XSelectInput(display, root, KeyPressMask);
    XSync(display, False);
    return true;
}

void HotkeyWindow::Impl::closeDisplay() {
    if (display != nullptr) {
        XCloseDisplay(display);
        display = nullptr;
    }
    root = 0;
}

void HotkeyWindow::Impl::computeLockMasks() {
    lockMasks.clear();
    // CapsLock 永远在 LockMask（core protocol 固定）上。
    lockMasks.push_back(LockMask);
    unsigned int numLockMask = 0;
    unsigned int scrollLockMask = 0;
    XModifierKeymap* mapping = XGetModifierMapping(display);
    if (mapping != nullptr) {
        // 不同发行版把 NumLock/ScrollLock 绑到哪个修饰位并不固定（Ubuntu 默认
        // Mod2/Mod5，也有把 ScrollLock 绑到 Mod3 的），所以运行时查一次 modifier map，
        // 查不到才按惯例兜底。抓取与匹配必须用同一份锁位，否则 CapsLock 一开热键就失效。
        const KeyCode numLockKey = XKeysymToKeycode(display, XK_Num_Lock);
        const KeyCode scrollLockKey = XKeysymToKeycode(display, XK_Scroll_Lock);
        for (int mod = 0; mod < 8; ++mod) {
            for (int slot = 0; slot < mapping->max_keypermod; ++slot) {
                const KeyCode code = mapping->modifiermap[mod * mapping->max_keypermod + slot];
                if (code == 0) {
                    continue;
                }
                const unsigned int mask = 1u << mod;
                if (numLockKey != 0 && code == numLockKey && numLockMask == 0) {
                    numLockMask = mask;
                }
                if (scrollLockKey != 0 && code == scrollLockKey && scrollLockMask == 0) {
                    scrollLockMask = mask;
                }
            }
        }
        XFreeModifiermap(mapping);
    }
    if (numLockMask == 0) {
        numLockMask = Mod2Mask;
    }
    if (scrollLockMask == 0) {
        scrollLockMask = Mod5Mask;
    }
    // 只接受"不和 Ctrl/Alt/Shift/Win 重叠"的位：真重叠时（改装过 keymap）宁可少抓
    // 一个锁变体，也不能把用户配置的修饰键从匹配里抹掉。
    const auto addIfUsable = [this](unsigned int mask) {
        if (mask == 0 || mask == ShiftMask || mask == ControlMask || mask == Mod1Mask ||
            mask == Mod4Mask) {
            return;
        }
        if (std::find(lockMasks.begin(), lockMasks.end(), mask) == lockMasks.end()) {
            lockMasks.push_back(mask);
        }
    };
    addIfUsable(numLockMask);
    addIfUsable(scrollLockMask);
}

void HotkeyWindow::Impl::setAccepting(bool accepting) {
    std::lock_guard<std::mutex> lock(mutex);
    acceptingEvents = accepting;
}

void HotkeyWindow::Impl::onBusEvent(void* user, const char* topic, const void* /*data*/,
                                    std::uint32_t /*len*/) {
    auto* self = static_cast<Impl*>(user);
    if (self == nullptr || topic == nullptr) {
        return;
    }
    // 回调运行在宿主分发线程上：只做"置 pending + 写唤醒管道"，不解析、不 XGrabKey
    // （EVENT_BUS.md §3 的回调规则）。
    const std::string_view topicView(topic);
    try {
        if (topicView == kTopicConfigApplied) {
            self->requestReapply();
        } else if (topicView == kTopicHotkeySuspend) {
            self->requestSuspend(true);
        } else if (topicView == kTopicHotkeyResume) {
            self->requestSuspend(false);
        }
        // 其它主题安静忽略：事件总线是共享的，不认识的主题不是错误。
    } catch (...) {
        shot::Logger(self->host).warn("[hotkey] 处理事件总线通知失败（内存不足？），已忽略这一条");
    }
}

}  // namespace hotkey
