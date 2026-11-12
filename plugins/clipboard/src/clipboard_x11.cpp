// Linux/X11 剪贴板的生命周期：起停专属线程、事件循环、取得 CLIPBOARD 所有权。
//
// 为什么必须是常驻线程而不是"写完就返回"：X11 的 selection 是**拉模型**。插件
// 调 XSetSelectionOwner 只是告诉 X server "以后有人要 CLIPBOARD 就来找我"；别的
// 程序粘贴时会发 SelectionRequest，owner 必须在自己的连接上应答，否则对方拿到
// 的是失败/超时。宿主事件分发回调不能常驻等事件，所以这里开一个线程，用
// poll(X 连接 + 唤醒管道) 阻塞等待（空闲不占 CPU），主线程只投递最新负载。
//
// 为什么所有 Xlib 调用都在这个线程：Xlib 的 Display 不是可重入对象；宿主进程里
// 还可能有 GLFW/settings_ui 在用 X11。把本插件的 Display、窗口、Atom 全部关在
// 一个线程里，至少保证本插件内部没有并发 Xlib 调用（跨组件的风险见 README）。
//
// 失败语义：无 X11 Display（Wayland 会话）时 start() 返回 false + 中文原因，调用
// 方只记 WARN；截屏文件已经落盘，剪贴板不可用不影响其它插件。

#include "clipboard_x11.h"

#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include "clipboard_image.h"
#include "clipboard_x11_impl.h"
#include "shot_plugin_helpers.h"

namespace clipboard {
namespace {

/**
 * @brief 把唤醒管道两端设为非阻塞：主线程写、X11 线程读，都不能因为管道满而卡住。
 * @param[in]  fd    管道描述符。
 * @param[out] error 失败时写入中文原因。
 * @return true = 设置完成。
 */
bool makeNonBlocking(int fd, std::string& error) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        error = "设置 X11 剪贴板唤醒管道为非阻塞失败";
        return false;
    }
    return true;
}

}  // namespace

ClipboardX11::ClipboardX11() : m_impl(std::make_unique<Impl>()) {}

ClipboardX11::~ClipboardX11() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool ClipboardX11::start(const ShotHostServices* host, std::string& error) {
    error.clear();
    if (!m_impl) {
        return false;
    }
    Impl& impl = *m_impl;
    // 宿主不会并发调 start/stop；这把锁只是防御设置页/卸载路径同时进来时重复起线程。
    std::lock_guard<std::mutex> lifecycle(impl.lifecycleMutex);
    if (impl.thread.joinable()) {
        return true;  // 已经在跑：重复调用是空操作
    }
    impl.host = host;
    {
        std::lock_guard<std::mutex> lock(impl.stateMutex);
        impl.ready = false;
        impl.failed = false;
        impl.failure.clear();
    }
    {
        std::lock_guard<std::mutex> lock(impl.commandMutex);
        impl.pending = ClipboardImage::Payload{};
        impl.hasPending = false;
        impl.accepting = true;
    }

    if (::pipe(impl.wakePipe) != 0) {
        error = "创建 X11 剪贴板唤醒管道失败";
        impl.wakePipe[0] = -1;
        impl.wakePipe[1] = -1;
        return false;
    }
    if (!makeNonBlocking(impl.wakePipe[0], error) ||
        !makeNonBlocking(impl.wakePipe[1], error)) {
        impl.closeWakePipe();
        return false;
    }

    impl.running = true;
    try {
        impl.thread = std::thread(&Impl::run, &impl);
    } catch (...) {
        // 线程资源耗尽等：异常绝不跨越 DLL 边界，管道与队列都撤回。
        impl.running = false;
        impl.closeWakePipe();
        {
            std::lock_guard<std::mutex> lock(impl.commandMutex);
            impl.accepting = false;
        }
        error = "创建 X11 剪贴板线程失败";
        return false;
    }

    std::unique_lock<std::mutex> lock(impl.stateMutex);
    impl.readyCv.wait(lock, [&impl]() { return impl.ready || impl.failed; });
    if (!impl.failed) {
        return true;
    }
    error = impl.failure.empty() ? "X11 剪贴板初始化失败" : impl.failure;
    lock.unlock();
    // 线程在 failed 之后会自己收尾退出；join 不会阻塞太久，但必须等它把 Display
    // 关掉再释放管道，否则管道 fd 可能在 X11 线程收尾期间被复用。
    impl.thread.join();
    impl.running = false;
    impl.closeWakePipe();
    return false;
}

void ClipboardX11::stop() {
    if (!m_impl) {
        return;
    }
    Impl& impl = *m_impl;
    std::lock_guard<std::mutex> lifecycle(impl.lifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(impl.commandMutex);
        impl.accepting = false;
        impl.hasPending = false;
        impl.pending = ClipboardImage::Payload{};
    }
    impl.running = false;
    impl.wake();
    if (impl.thread.joinable()) {
        impl.thread.join();
    }
    impl.closeWakePipe();
    {
        std::lock_guard<std::mutex> lock(impl.commandMutex);
        impl.pending = ClipboardImage::Payload{};
    }
    impl.host = nullptr;
}

bool ClipboardX11::copyImageFile(const std::string& utf8Path, bool alsoFileDrop,
                                 std::string& error) {
    error.clear();
    if (!m_impl) {
        return false;
    }
    // 线程已经退出（X 连接断开 / start 失败）时不要先花时间解码一整张图：
    // 失败原因直接返回，编码交给下一次真正可用的截图。
    if (!m_impl->running.load()) {
        error = "X11 剪贴板线程未运行";
        return false;
    }
    // 读图 + PNG 编码留在调用线程（宿主 main）：与 Windows 版本一样是几十毫秒的
    // 同步工作；X11 线程只负责协议应答，不做文件 IO，避免拖慢粘贴响应。
    ClipboardImage::Payload payload;
    if (!ClipboardImage::build(utf8Path, alsoFileDrop, payload, error)) {
        return false;
    }
    Impl& impl = *m_impl;
    {
        std::lock_guard<std::mutex> lock(impl.commandMutex);
        if (!impl.accepting || !impl.running.load()) {
            error = "X11 剪贴板线程未运行";
            return false;
        }
        // 只保留最新一次截图：连续截屏时旧图已经被新图取代，再花时间应答旧数据
        // 没有意义（Windows 的 EmptyClipboard 也是同样的"后一张覆盖前一张"语义）。
        impl.pending = std::move(payload);
        impl.hasPending = true;
    }
    impl.wake();
    return true;
}

bool ClipboardX11::Impl::setup() {
    // XInitThreads 必须是进程里第一个 Xlib 调用才有效。宿主可能先加载了
    // settings_ui（GLFW 会打开自己的 Display），所以这里失败只 WARN 不放弃：
    // 本插件自己的 Xlib 调用仍然只在单线程里发生。
    if (XInitThreads() == 0) {
        shot::Logger(host).warn(
            "[clipboard] XInitThreads 失败（Xlib 可能已被其它组件无锁初始化），跨组件并发调用有风险");
    }
    display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        failure = "没有可用的 X11 Display（Wayland 会话或 DISPLAY 未设置）";
        return false;
    }
    window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    if (window == 0) {
        failure = "创建 X11 剪贴板 owner 窗口失败";
        return false;
    }
    // 只订阅 PropertyChange：serverTime() 要借一次 PropertyNotify 拿服务器时间。
    // SelectionRequest / SelectionClear 是直接投给 owner 的，不需要事件掩码。
    XSelectInput(display, window, PropertyChangeMask);

    clipboardAtom = XInternAtom(display, "CLIPBOARD", False);
    targetsAtom = XInternAtom(display, "TARGETS", False);
    timestampAtom = XInternAtom(display, "TIMESTAMP", False);
    multipleAtom = XInternAtom(display, "MULTIPLE", False);
    pngAtom = XInternAtom(display, "image/png", False);
    uriListAtom = XInternAtom(display, "text/uri-list", False);
    timeAtom = XInternAtom(display, "_PLUGIN_SHOT_TIME", False);
    selfCheckAtom = XInternAtom(display, "_PLUGIN_SHOT_SELF_CHECK", False);
    if (clipboardAtom == None || targetsAtom == None || timestampAtom == None ||
        multipleAtom == None || pngAtom == None || uriListAtom == None || timeAtom == None ||
        selfCheckAtom == None) {
        failure = "注册 X11 剪贴板 atom 失败";
        return false;
    }
    XFlush(display);
    return true;
}

void ClipboardX11::Impl::run() {
    bool initialized = false;
    try {
        initialized = setup();
    } catch (...) {
        failure = "初始化 X11 剪贴板时异常";
        initialized = false;
    }
    if (!initialized) {
        notifyFailed(failure);
        shutdownDisplay();
        running = false;
        return;
    }

    notifyReady();
    shot::Logger(host).info(
        "[clipboard] X11 剪贴板线程就绪（收到截图后取得 CLIPBOARD selection，提供 image/png + text/uri-list）");
    try {
        eventLoop();
    } catch (...) {
        // 线程函数里逃出异常会 std::terminate；协议处理里的容器分配等异常在这里兜住。
        shot::Logger(host).warn("[clipboard] X11 剪贴板事件循环异常，线程退出");
    }

    // 退出前主动放弃所有权：否则 X server 会留下一个指向已销毁窗口的 owner，
    // 用户之后粘贴会超时而不是"剪贴板为空"。
    releaseSelection();
    shutdownDisplay();
    running = false;
    shot::Logger(host).info("[clipboard] X11 剪贴板线程已退出");
}

void ClipboardX11::Impl::eventLoop() {
    while (running.load()) {
        // 先把队列里的 X 事件处理干净，再阻塞在 poll 上；不要用空转轮询。
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            handleEvent(event);
            if (!running.load()) {
                break;
            }
        }
        if (!running.load()) {
            break;
        }

        struct pollfd fds[2] = {};
        fds[0].fd = ConnectionNumber(display);
        fds[0].events = POLLIN;
        fds[1].fd = wakePipe[0];
        fds[1].events = POLLIN;
        const int result = ::poll(fds, 2, -1);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            shot::Logger(host).warn("[clipboard] poll 失败，X11 剪贴板线程退出");
            break;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            drainWakePipe();
            ClipboardImage::Payload next;
            if (takePending(next)) {
                claim(next);
            }
        }
        if ((fds[0].revents & (POLLHUP | POLLERR)) != 0) {
            shot::Logger(host).warn("[clipboard] X11 连接已断开，剪贴板线程退出");
            break;
        }
    }
}

void ClipboardX11::Impl::shutdownDisplay() {
    if (display != nullptr) {
        if (owned) {
            XSetSelectionOwner(display, clipboardAtom, None, CurrentTime);
            owned = false;
        }
        if (window != 0) {
            XDestroyWindow(display, window);
            window = 0;
        }
        XCloseDisplay(display);
        display = nullptr;
    }
    releasePayload();
}

void ClipboardX11::Impl::notifyReady() {
    std::lock_guard<std::mutex> lock(stateMutex);
    ready = true;
    readyCv.notify_all();
}

void ClipboardX11::Impl::notifyFailed(const std::string& reason) {
    std::lock_guard<std::mutex> lock(stateMutex);
    failure = reason;
    failed = true;
    readyCv.notify_all();
}

bool ClipboardX11::Impl::takePending(ClipboardImage::Payload& out) {
    std::lock_guard<std::mutex> lock(commandMutex);
    if (!hasPending) {
        return false;
    }
    out = std::move(pending);
    pending = ClipboardImage::Payload{};
    hasPending = false;
    return true;
}

bool ClipboardX11::Impl::claim(const ClipboardImage::Payload& next) {
    if (!next.hasUriList && next.png.empty()) {
        shot::Logger(host).warn("[clipboard] 剪贴板负载为空，跳过");
        return false;
    }
    // 先保存数据再抢所有权：如果先 XSetSelectionOwner 再保存，中间的 SelectionRequest
    // 会拿到半成品/空数据。
    payload = next;
    hasPayload = true;

    const Time acquisitionTime = serverTime();
    XSetSelectionOwner(display, clipboardAtom, window, acquisitionTime);
    if (XGetSelectionOwner(display, clipboardAtom) != window) {
        // 被另一个程序抢回去了（例如剪贴板管理器在同一时刻也在 claim）：本次放弃，
        // 下一次 capture.done 再试；不能把失败留给粘贴方去超时。owned 必须一并清掉，
        // 否则退出时会对一个已经不属于我们的 selection 调 XSetSelectionOwner(None)，
        // 把别人的剪贴板也清空。
        owned = false;
        awaitingSelfCheck = false;
        releasePayload();
        shot::Logger(host).warn("[clipboard] XSetSelectionOwner 失败（CLIPBOARD 已被其它程序占用）");
        return false;
    }
    owned = true;
    ownershipTime = acquisitionTime;

    // 自检：XSetSelectionOwner / XGetSelectionOwner 只能证明 X server 记下了 owner，
    // 不能证明数据链路真的可用。真正的数据交换是 SelectionRequest → SelectionNotify
    // 往返；剪贴板管理器（Klipper/clipman/桌面自带的 manager）会在 owner 变化后立刻
    // 发 TARGETS 或数据请求，如果 owner 没进事件循环就宣称"成功"，第一个请求会丢。
    // 所以这里向自己请求一次 TARGETS，并把收到 SelectionNotify 当作"selection 真正
    // 就绪"的标志（异步处理，不阻塞其它请求）。
    awaitingSelfCheck = true;
    XDeleteProperty(display, window, selfCheckAtom);
    XConvertSelection(display, clipboardAtom, targetsAtom, selfCheckAtom, window,
                      ownershipTime);
    XFlush(display);
    shot::Logger(host).info(
        std::string("[clipboard] 已取得 CLIPBOARD 所有权（image/png") +
        (hasPayload && payload.hasUriList ? " + text/uri-list" : "") +
        "），等待 TARGETS 自检的 SelectionNotify");
    return true;
}

void ClipboardX11::Impl::releaseSelection() {
    if (display != nullptr && owned) {
        XSetSelectionOwner(display, clipboardAtom, None, CurrentTime);
        XFlush(display);
    }
    owned = false;
    awaitingSelfCheck = false;
    releasePayload();
}

void ClipboardX11::Impl::releasePayload() {
    hasPayload = false;
    payload = ClipboardImage::Payload{};
}

void ClipboardX11::Impl::wake() {
    if (wakePipe[1] < 0) {
        return;
    }
    const char byte = 'x';
    // 管道满说明 X11 线程还没处理上一次唤醒，不必再写：它醒来后会取走最新负载。
    const ssize_t ignored = ::write(wakePipe[1], &byte, 1);
    (void)ignored;
}

void ClipboardX11::Impl::drainWakePipe() {
    if (wakePipe[0] < 0) {
        return;
    }
    char buffer[64];
    for (;;) {
        const ssize_t bytes = ::read(wakePipe[0], buffer, sizeof(buffer));
        if (bytes > 0) {
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        break;  // 0 = 写端已关；-1 且 EAGAIN = 读空了
    }
}

void ClipboardX11::Impl::closeWakePipe() {
    if (wakePipe[0] >= 0) {
        ::close(wakePipe[0]);
        wakePipe[0] = -1;
    }
    if (wakePipe[1] >= 0) {
        ::close(wakePipe[1]);
        wakePipe[1] = -1;
    }
}

}  // namespace clipboard
