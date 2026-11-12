// Linux/X11 热键线程的事件循环与唤醒管道：poll(X 连接 fd + self-pipe) → XPending/XNextEvent。
//
// 为什么用 poll 而不是"定时检查标志位"：热键线程空闲时必须零 CPU，而主线程的
// 重注册/暂停/退出请求又不能等下一次按键才有反应，所以用一根 self-pipe 叫醒；
// X 事件本身由 X 连接的 fd 唤醒。两条都不可读时才阻塞，没有任何轮询间隔魔数。
//
// 为什么还要 XPending/XNextEvent：Xlib 自己管理连接的读缓冲与协议解析，poll 只负责
// 等"下一次可读"，事件仍然必须交给 Xlib 取。唤醒管道里的字节没有含义，只表示
// "主线程有请求，去取一次 pending 标记"。
//
// 唤醒管道的 fd 由 hotkey_window_x11.cpp 在起线程前建好、stop() join 之后关掉；
// 这里只负责读写，不改变它的生命周期。

#include "hotkey_window_x11_impl.h"

#include <cerrno>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "shot_plugin_helpers.h"

namespace hotkey {

bool HotkeyWindow::Impl::createWakePipe() {
    closeWakePipe();
    if (::pipe(wakePipe) != 0) {
        wakePipe[0] = -1;
        wakePipe[1] = -1;
        return false;
    }
    // 两端都设非阻塞：wake() 不能因为管道满而卡住主线程，热键线程读空时也不能阻塞。
    for (int index = 0; index < 2; ++index) {
        const int flags = ::fcntl(wakePipe[index], F_GETFL, 0);
        if (flags < 0 || ::fcntl(wakePipe[index], F_SETFL, flags | O_NONBLOCK) < 0) {
            closeWakePipe();
            return false;
        }
    }
    return true;
}

void HotkeyWindow::Impl::closeWakePipe() {
    for (int& fd : wakePipe) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
}

void HotkeyWindow::Impl::wake() {
    if (wakePipe[1] < 0) {
        return;
    }
    const char byte = 1;
    // 管道满（EAGAIN）说明至少还有一个未消费的字节，唤醒目的已经达到，忽略即可。
    (void)::write(wakePipe[1], &byte, 1);
}

void HotkeyWindow::Impl::drainWakePipe() {
    if (wakePipe[0] < 0) {
        return;
    }
    char buffer[64];
    for (;;) {
        const ssize_t got = ::read(wakePipe[0], buffer, sizeof(buffer));
        if (got > 0) {
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return;  // 0 = 写端已关；EAGAIN = 读空了
    }
}

void HotkeyWindow::Impl::eventLoop() {
    for (;;) {
        // 1. 先把 X 已经排队的按键事件处理完：XPending 不阻塞，XNextEvent 只取已有事件。
        //    单条事件处理里的事件发布/日志会分配内存，异常在这里就地兜住——线程入口
        //    是原生线程帧，异常穿出去会 std::terminate（对应 Windows 的 WndProc 兜底）。
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            try {
                handleEvent(event);
            } catch (...) {
                shot::Logger(host).warn("[hotkey] 处理 X11 按键事件发生异常，已忽略这一条");
            }
        }
        // 2. 再处理主线程投递的请求（重注册 / 暂停 / 退出）。
        try {
            if (!processRequests()) {
                return;
            }
        } catch (...) {
            shot::Logger(host).warn("[hotkey] 处理热键请求发生异常，已忽略这一轮");
        }
        // 3. 都处理完才阻塞：X 连接可读（按键事件）或唤醒管道可读（主线程请求）。
        pollfd fds[2];
        fds[0].fd = ConnectionNumber(display);
        fds[0].events = POLLIN;
        fds[1].fd = wakePipe[0];
        fds[1].events = POLLIN;
        const int result = ::poll(fds, 2, -1);
        if (result < 0) {
            if (errno == EINTR) {
                continue;  // 被信号打断不是错误：回到循环顶部再看一遍 pending
            }
            shot::Logger(host).warn("[hotkey] 等待 X11 事件失败（errno " +
                                    std::to_string(errno) + "），热键线程退出");
            return;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            drainWakePipe();
        }
        if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            shot::Logger(host).warn("[hotkey] X11 连接已断开，热键线程退出");
            return;
        }
    }
}

void HotkeyWindow::Impl::handleEvent(const XEvent& event) {
    if (event.type == KeyPress) {
        publishHotkey(event.xkey.keycode, event.xkey.state);
    }
    // MappingNotify（键盘映射变化）不处理：抓取按 keycode 生效，换布局后同一个
    // 物理键仍然触发；改成"每次映射变化就重抓"反而会在切换输入法时抖动。
    // TODO: X11 没有 MOD_NOREPEAT 的等价物，按住不放会按服务器的自动重复连续触发；
    //       抑制需要维护"按下中的 keycode 集合"并在 KeyRelease 里移除，但丢一次
    //       release（别的 client 抢走键盘抓取）会让该热键永久失效，所以先不实现。
}

}  // namespace hotkey
