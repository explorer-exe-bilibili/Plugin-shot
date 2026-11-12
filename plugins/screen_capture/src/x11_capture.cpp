// Linux/X11 平台层：只做 X11 系统调用，不判断契约返回码。
//
// 一次 capture 的流水线：
//   XOpenDisplay                  每次调用独立连接（Display 不跨线程共享）
//   XShmQueryExtension            MIT-SHM 可用才走共享内存路径
//   shmget / shmat / XShmCreateImage
//   XShmAttach + XSync            错误是异步的，临时接管错误处理器判断成败
//   XShmGetImage                  server 直接写共享内存，少一次协议拷贝
//   convertToBgra                 XImage → BGRA8（x11_image_convert.cpp）
//   compositeCursor               可选 XFixes 光标合成（x11_cursor.cpp）
//   XShmDetach / shmdt / XDestroyImage
//   XCloseDisplay
// 失败或扩展不可用时整段回退 XGetImage，错误语义与 Windows 一致：本文件返回 false，
// 由契约层映射成返回码 3。
//
// 为什么每次 XOpenDisplay：Xlib 的 Display 不是线程安全的，而宿主进程里
// settings_ui（GLFW）与 clipboard（X11 selection）也可能各开连接。插件选择
// "一次调用一个连接"，Display 不逃出函数，也就不需要插件内的锁、不需要在析构里
// 收连接；截图是低频操作，本地 socket 的连接成本可以接受。
//
// 也不调用 XInitThreads：它必须是进程里第一个 Xlib 调用，而宿主里 settings_ui /
// clipboard 可能已经先用过 Xlib；用"一次调用一个连接"规避本插件内部的并发，
// 进程级的 Xlib 线程安全由各组件各自负责（见 README §6.5）。

#include "x11_capture.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>

#include <sys/ipc.h>
#include <sys/shm.h>

namespace screen_capture {
namespace {

// MIT-SHM 的 XShmAttach 失败是**异步**错误：Xlib 默认错误处理器遇到协议错误会
// exit(1)，一次远程 DISPLAY 或容器禁用 shm 就能把宿主带走。所以只在
// attach + XSync 的短窗口里临时接管处理器，把错误码记下来再恢复。
//
// 处理器是进程级状态：宿主采集流水线本来就串行调用本插件，但 settings_ui /
// clipboard 的 Xlib 调用可能并发，所以只认自己的 Display，其它 Display 的错误
// 转交给上一个处理器（没有上一个时也不杀宿主——这个窗口只有一次 round trip）。
std::mutex g_shmErrorMutex;
std::atomic<Display*> g_shmErrorDisplay{nullptr};
std::atomic<int> g_shmErrorCode{0};
std::atomic<XErrorHandler> g_previousErrorHandler{nullptr};

/**
 * @brief 只截获本插件 Display 上的 X 错误，其它连接的错误交还给上一个处理器。
 * @param[in] display 出错连接。
 * @param[in] event   错误事件；Xlib 不会传空，但防御性判一下。
 * @return 0 = 已消费；其它值来自上一个处理器。
 */
int shmErrorHandler(Display* display, XErrorEvent* event) {
    if (event != nullptr && display == g_shmErrorDisplay.load(std::memory_order_acquire)) {
        g_shmErrorCode.store(static_cast<int>(event->error_code), std::memory_order_relaxed);
        return 0;
    }
    const XErrorHandler previous = g_previousErrorHandler.load(std::memory_order_acquire);
    if (previous != nullptr) {
        return previous(display, event);
    }
    return 0;
}

/**
 * @brief 执行 XShmAttach 并用 XSync 等到异步错误返回，报告是否真的 attach 成功。
 *
 * 不能只看 XShmAttach 的返回值：请求只是写进输出缓冲，BadAccess 要等 XSync 读到
 * server 的错误回复才知道。返回值里的 errorCode 让调用方能区分"只返回 False"与
 * "X server 拒绝（例如 BadAccess）"。
 *
 * @param[in]  display 本线程刚打开的连接。
 * @param[in]  segment 已 shmat 的共享内存段。
 * @param[out] errorCode X 协议错误码；0 = 没有错误。
 * @return true = XShmAttach 成功且没有 X 错误；false = 失败。
 */
bool attachShmSegment(Display* display, XShmSegmentInfo* segment, int& errorCode) {
    // 进程级处理器 + 本插件的全局状态：用互斥锁把"安装→attach→恢复"串起来，
    // 防止同一进程里两个 capture（理论上不会并发）互相覆盖处理器。
    std::lock_guard<std::mutex> lock(g_shmErrorMutex);
    g_shmErrorCode.store(0, std::memory_order_relaxed);
    g_shmErrorDisplay.store(display, std::memory_order_release);
    const XErrorHandler previous = XSetErrorHandler(&shmErrorHandler);
    g_previousErrorHandler.store(previous, std::memory_order_release);

    const Bool attached = XShmAttach(display, segment);
    XSync(display, False);  // 异步错误在这里被 shmErrorHandler 收下

    XSetErrorHandler(previous);
    g_previousErrorHandler.store(nullptr, std::memory_order_release);
    g_shmErrorDisplay.store(nullptr, std::memory_order_release);
    errorCode = g_shmErrorCode.load(std::memory_order_relaxed);
    return attached != 0 && errorCode == 0;
}

/**
 * @brief 判断当前 DISPLAY 是否指向本机 X server（MIT-SHM 只对同机连接有效）。
 *
 * 远程 DISPLAY（ssh -X 的 host:10.0）即使扩展在位，attach 也会被 server 拒绝；
 * 提前跳过可以避免一次无谓的失败往返。空的 DISPLAY 按本机处理：XOpenDisplay(NULL)
 * 默认就是本机 :0。
 *
 * @return true = 可以尝试 MIT-SHM；false = 明显是远程连接。
 */
bool isLocalDisplay() {
    const char* displayName = std::getenv("DISPLAY");
    if (displayName == nullptr || displayName[0] == '\0') {
        return true;
    }
    return displayName[0] == ':' || std::strncmp(displayName, "unix:", 5) == 0;
}

}  // namespace

Display* X11Capture::openDisplay(shot::Logger log) {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        // 无头环境 / X server 未启动 / DISPLAY 指向不存在的显示：这是"采集通道
        // 不可用"（返回 3），不是"没有可采集的屏幕"（返回 1）。
        log.error("screen_capture: 无法打开 X11 Display（DISPLAY 未设置，或 X server 不可达）");
    }
    return display;
}

void X11Capture::closeDisplay(Display* display) {
    if (display != nullptr) {
        XCloseDisplay(display);
    }
}

std::int32_t X11Capture::queryGeometry(ShotCaptureInfo& info, shot::Logger log) {
    Display* display = openDisplay(log);
    if (display == nullptr) {
        return 3;
    }
    const int screen = DefaultScreen(display);
    const int width = DisplayWidth(display, screen);
    const int height = DisplayHeight(display, screen);
    if (width <= 0 || height <= 0) {
        log.warn("screen_capture: X11 当前没有可采集的屏幕（DisplayWidth/DisplayHeight <= 0）");
        closeDisplay(display);
        return 1;
    }
    // 只在 describe 时打一行显示器布局：普通日志看不到（debug 级），真机排查多屏
    // 合并与坐标问题时再打开。失败不影响采集。
    logMonitorLayout(display, DefaultRootWindow(display), log);
    closeDisplay(display);

    info.width = width;
    info.height = height;
    // 整个 X screen 的原点就是 root window 的 (0,0)。多显示器是合并到同一个 root
    // 坐标里的，不需要再把某块屏的偏移减掉。
    info.origin_x = 0;
    info.origin_y = 0;
    return 0;
}

void X11Capture::logMonitorLayout(Display* display, Window root, shot::Logger log) {
    int eventBase = 0;
    int errorBase = 0;
    if (XRRQueryExtension(display, &eventBase, &errorBase) == 0) {
        return;
    }
    int major = 0;
    int minor = 0;
    if (XRRQueryVersion(display, &major, &minor) == 0 || major < 1 || (major == 1 && minor < 5)) {
        return;  // XRRGetMonitors 是 RandR 1.5 才有的接口，老 server 只跳过诊断日志
    }
    int monitorCount = 0;
    XRRMonitorInfo* monitors = XRRGetMonitors(display, root, True, &monitorCount);
    if (monitors == nullptr) {
        return;
    }
    if (monitorCount > 0) {
        std::string message = "screen_capture: X11 显示器布局：";
        for (int index = 0; index < monitorCount; ++index) {
            const XRRMonitorInfo& monitor = monitors[index];
            if (index > 0) {
                message += "；";
            }
            char* name = XGetAtomName(display, monitor.name);
            message += (name != nullptr) ? name : "<unnamed>";
            if (name != nullptr) {
                XFree(name);
            }
            message += " ";
            message += std::to_string(monitor.width);
            message += "x";
            message += std::to_string(monitor.height);
            message += " @";
            message += std::to_string(monitor.x);
            message += ",";
            message += std::to_string(monitor.y);
        }
        log.debug(message);
    }
    XRRFreeMonitors(monitors);
}

bool X11Capture::captureTo(const ShotImage& dst, const ShotCaptureInfo& target,
                           bool includeCursor, shot::Logger log) {
    // 调用方（ScreenCapture）已经校验过一次；这里只兜"Xlib 的宽高参数是 unsigned，
    // 负尺寸会被转成巨大值"这一层。
    if (target.width != dst.width || target.height != dst.height) {
        log.error("screen_capture: X11 目标尺寸与 describe 的 screen 尺寸不一致");
        return false;
    }
    if (target.width <= 0 || target.height <= 0) {
        log.error("screen_capture: X11 抓屏尺寸非法");
        return false;
    }

    Display* display = openDisplay(log);
    if (display == nullptr) {
        return false;
    }
    const Window root = DefaultRootWindow(display);
    bool ok = grabIntoBgra(display, root, target.width, target.height, dst, log);
    if (ok && includeCursor) {
        // 光标必须画在桌面像素转换完之后：合成直接读写 BGRA8 目标图，不再碰 XImage。
        compositeCursor(display, dst, log);
    }
    closeDisplay(display);
    return ok;
}

bool X11Capture::grabIntoBgra(Display* display, Window root, std::int32_t width,
                              std::int32_t height, const ShotImage& dst, shot::Logger log) {
    // XShm 不可用时 grabWithShm 静默返回 false（扩展缺失/远程 DISPLAY 都是正常
    // 情况）；只有 shm 初始化失败才写 WARN。随后统一走 XGetImage 兜底。
    if (grabWithShm(display, root, width, height, dst, log)) {
        return true;
    }
    return grabWithCore(display, root, width, height, dst, log);
}

bool X11Capture::grabWithShm(Display* display, Window root, std::int32_t width,
                             std::int32_t height, const ShotImage& dst, shot::Logger log) {
    if (display == nullptr || XShmQueryExtension(display) == 0) {
        return false;  // 没有 MIT-SHM：正常回退 XGetImage，不写日志
    }
    if (!isLocalDisplay()) {
        return false;  // 远程 DISPLAY 不能共享内存，交给 XGetImage
    }

    const int screen = DefaultScreen(display);
    XShmSegmentInfo segment = {};
    XImage* image = XShmCreateImage(display, DefaultVisual(display, screen),
                                    static_cast<unsigned int>(DefaultDepth(display, screen)),
                                    ZPixmap, nullptr, &segment,
                                    static_cast<unsigned int>(width),
                                    static_cast<unsigned int>(height));
    if (image == nullptr) {
        log.warn("screen_capture: X11 XShmCreateImage 失败，回退 XGetImage");
        return false;
    }
    if (image->bytes_per_line <= 0) {
        XDestroyImage(image);
        log.warn("screen_capture: X11 XShmCreateImage 给出的行距非法，回退 XGetImage");
        return false;
    }

    // 段大小取自 XImage 的 bytes_per_line：server 可能给 24bpp 行尾补对齐，
    // 按 width*height*4 申请会少一块，XShmGetImage 就会写越界。
    const std::size_t segmentBytes = static_cast<std::size_t>(image->bytes_per_line) *
                                     static_cast<std::size_t>(height);
    segment.shmid = shmget(IPC_PRIVATE, segmentBytes, IPC_CREAT | 0600);
    if (segment.shmid < 0) {
        XDestroyImage(image);
        log.warn("screen_capture: X11 shmget 失败（容器可能禁用了 SysV 共享内存），"
                 "回退 XGetImage");
        return false;
    }
    void* address = shmat(segment.shmid, nullptr, 0);
    if (address == reinterpret_cast<void*>(static_cast<std::intptr_t>(-1))) {
        (void)shmctl(segment.shmid, IPC_RMID, nullptr);
        XDestroyImage(image);
        log.warn("screen_capture: X11 shmat 失败，回退 XGetImage");
        return false;
    }
    segment.shmaddr = image->data = static_cast<char*>(address);
    segment.readOnly = False;

    int attachError = 0;
    if (!attachShmSegment(display, &segment, attachError)) {
        // attach 失败时不能调 XShmDetach：server 根本没接上，detach 只会再报一个错。
        (void)shmctl(segment.shmid, IPC_RMID, nullptr);
        shmdt(segment.shmaddr);
        image->data = nullptr;  // 不能让 XDestroyImage 去 free() 共享内存
        XDestroyImage(image);
        if (attachError != 0) {
            log.warn("screen_capture: X11 XShmAttach 失败（X error " +
                     std::to_string(attachError) + "），回退 XGetImage");
        } else {
            log.warn("screen_capture: X11 XShmAttach 失败，回退 XGetImage");
        }
        return false;
    }

    // server 已经 attach 成功，现在把段标成"待删除"：本进程和 X server 的 attach
    // 都还在，最后一个 detach 后内核回收，抓屏中途崩溃也不会留下孤儿段。
    (void)shmctl(segment.shmid, IPC_RMID, nullptr);

    bool ok = false;
    if (XShmGetImage(display, root, image, 0, 0, AllPlanes) != 0) {
        ok = convertToBgra(*image, dst, log);
    } else {
        log.warn("screen_capture: X11 XShmGetImage 失败，回退 XGetImage");
    }

    // XShmGetImage 是同步请求，转换期间 server 不会再写这块内存；先 detach 再
    // shmdt，最后把 XImage 的 data 清空，避免 XDestroyImage 误 free 共享内存。
    XShmDetach(display, &segment);
    XSync(display, False);
    shmdt(segment.shmaddr);
    image->data = nullptr;
    XDestroyImage(image);
    return ok;
}

bool X11Capture::grabWithCore(Display* display, Window root, std::int32_t width,
                              std::int32_t height, const ShotImage& dst, shot::Logger log) {
    XImage* image = XGetImage(display, root, 0, 0, static_cast<unsigned int>(width),
                              static_cast<unsigned int>(height), AllPlanes, ZPixmap);
    if (image == nullptr) {
        log.error("screen_capture: X11 XGetImage 抓取 root window 失败");
        return false;
    }
    const bool ok = convertToBgra(*image, dst, log);
    XDestroyImage(image);
    return ok;
}

}  // namespace screen_capture
