// region_geometry_x11 —— Linux/X11 的平台几何层：X screen 几何、光标位置、显示器矩形、
// 原生窗口句柄、`_NET_WORKAREA` → 面板占位 inset，以及首次框选时把实测数据写进宿主日志。
//
// 与 Windows 版（region_geometry.cpp）的差别：
//   · Windows 有"虚拟桌面"概念（多显示器可以摆成负原点），region 发布前要减虚拟
//     桌面原点；X11 的坐标原点就是 root 窗口的 (0,0)，所有显示器都挂在同一块
//     X screen 上，所以虚拟桌面原点恒为 (0,0)，不做减法。
//   · 光标位置用 XQueryPointer（root 坐标）；显示器矩形交给 GLFW（它内部用 RandR
//     查询输出布局，和 X screen 坐标同一套）。**不引入 xrandr 依赖**：本插件只需要
//     "显示器在哪、多大"，GLFW 已经提供了。
//   · 原生句柄是 X11 Window（glfwGetX11Window），交给窗口枚举排除覆盖层自己。
//   · GNOME 的 shell 顶栏 / dock 不是客户端窗口（枚举不到），只能读 root 的
//     `_NET_WORKAREA`（去掉面板后的工作区）与显示器矩形相减，得到四条边的面板
//     占位厚度（见 x11ReadPanelInsets；悬停命中在 region_overlay.cpp）。
//
// 坐标语义：X 像素就是物理像素（Xft 的 DPI 只影响字体渲染）；content scale 只用于
// 放大字号/折角，绝不参与坐标换算（与 Windows 版同一条纪律，见 README §4）。
//
// monitorRectOf 在 region_geometry_wayland.cpp 里定义（Linux 两个分支共用：GLFW 在
// X11 下返回 RandR 的输出位置/尺寸，在 Wayland 下位置恒为 0,0）。
//
// 多 X screen：本文件与整个插件都不支持（X screen 之间不能互相覆盖/枚举，
// 见 README「Linux（X11）」）。GLFW 只使用默认 screen。
//
// 只有平台层文件包含 Xlib / glfw3native.h；内部头保持干净。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

// 自己包含 Xlib 头；glfw3native.h 会连带声明 RRCrtc/RROutput 并拉 <X11/extensions/Xrandr.h>，
// 所以编译期需要 libxrandr-dev（GLFW 运行时本来也用它枚举显示器）。
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

/** @brief `_NET_WORKAREA` 最多读多少个虚拟桌面（每个桌面 4 个 CARDINAL）。 */
constexpr unsigned long kMaxWorkAreaDesktops = 64;

/**
 * @brief 读 root 上一个 CARDINAL 数组属性（format=32）。
 *
 * `_NET_WORKAREA` / `_NET_CURRENT_DESKTOP` 都是 EWMH 的 CARDINAL。Xlib 对 format=32
 * 的属性统一返回 `long` 数组（每项占 8 字节，按 32 位无符号解释），调用方按
 * `const long*` 读即可。
 *
 * @param[in]  display   X11 连接。
 * @param[in]  window    属性所在窗口（这里是 root）。
 * @param[in]  name      属性名。
 * @param[in]  maxItems  最多读多少项。
 * @param[out] data      Xlib 分配的缓冲区（成功时非空，调用方负责 XFree）。
 * @param[out] itemCount 实际读到的项数（成功时 > 0）。
 * @return true = 属性存在且类型是 CARDINAL/32 位；false = 不存在 / 类型不对 / 查询失败。
 */
bool readCardinalArray(Display* display, Window window, const char* name, unsigned long maxItems,
                       unsigned char** data, unsigned long& itemCount) {
    *data = nullptr;
    itemCount = 0;
    const Atom atom = XInternAtom(display, name, True);
    if (atom == None) {
        return false;
    }
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long bytesAfter = 0;
    const int status = XGetWindowProperty(display, window, atom, 0, static_cast<long>(maxItems),
                                          False, XA_CARDINAL, &actualType, &actualFormat,
                                          &itemCount, &bytesAfter, data);
    if (status != Success || actualType != XA_CARDINAL || actualFormat != 32 ||
        *data == nullptr || itemCount == 0) {
        if (*data != nullptr) {
            XFree(*data);
            *data = nullptr;
        }
        itemCount = 0;
        return false;
    }
    return true;
}

}  // namespace

std::uintptr_t RegionWindow::Impl::x11NativeWindowHandle() const {
    // 覆盖层自己是铺满显示器的置顶窗口：枚举时必须按 XID 排除，否则命中测试
    // 永远选中它（或它被 WM 包出来的 frame，见 window_target_x11.cpp）。
    if (window == nullptr) {
        return 0;
    }
    return static_cast<std::uintptr_t>(glfwGetX11Window(window));
}

bool RegionWindow::Impl::x11MonitorUnderCursor(ScreenRect& out, float& contentScale) const {
    contentScale = 1.0f;
    Display* display = glfwGetX11Display();
    if (display == nullptr) {
        // GLFW 还没初始化 / X11 连接不可用：调用方退回主显示器并 WARN。
        return false;
    }
    // XQueryPointer 的 root_x/root_y 就是 X screen 坐标（= 像素），不需要任何缩放。
    Window rootReturn = 0;
    Window childReturn = 0;
    int rootX = 0;
    int rootY = 0;
    int windowX = 0;
    int windowY = 0;
    unsigned int mask = 0;
    if (XQueryPointer(display, DefaultRootWindow(display), &rootReturn, &childReturn, &rootX,
                      &rootY, &windowX, &windowY, &mask) == False) {
        return false;
    }
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (monitors == nullptr) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        const ScreenRect rect = monitorRectOf(monitors[index]);
        if (rect.width <= 0 || rect.height <= 0) {
            continue;
        }
        if (rootX >= rect.x && rootX < rect.x + rect.width && rootY >= rect.y &&
            rootY < rect.y + rect.height) {
            out = rect;
            float scaleX = 1.0f;
            float scaleY = 1.0f;
            glfwGetMonitorContentScale(monitors[index], &scaleX, &scaleY);
            contentScale = (scaleX > 0.0f) ? scaleX : ((scaleY > 0.0f) ? scaleY : 1.0f);
            return true;
        }
    }
    return false;
}

bool RegionWindow::Impl::x11LeftButtonHeld() const {
    // 覆盖层显示时左键可能还按着（从菜单/托盘触发）：Windows 版用 GetAsyncKeyState
    // 判 waitRelease，X11 没有全局按键状态，但 XQueryPointer 的 mask 里带当前修饰键/
    // 按钮状态（Button1Mask），等价可用。
    Display* display = glfwGetX11Display();
    if (display == nullptr) {
        return false;
    }
    Window rootReturn = 0;
    Window childReturn = 0;
    int rootX = 0;
    int rootY = 0;
    int windowX = 0;
    int windowY = 0;
    unsigned int mask = 0;
    if (XQueryPointer(display, DefaultRootWindow(display), &rootReturn, &childReturn, &rootX,
                      &rootY, &windowX, &windowY, &mask) == False) {
        return false;
    }
    return (mask & Button1Mask) != 0;
}

void RegionWindow::Impl::x11VirtualDesktopOrigin(int& x, int& y) const {
    // X11 只有一块 X screen，root 窗口的左上角就是 (0,0)；所有输出都在这块 screen 里。
    // 保留这个函数只是为了让提交逻辑与 Windows 版保持同形（多 X screen 不支持）。
    x = 0;
    y = 0;
}

void RegionWindow::Impl::x11LogMonitorMetrics() const {
    const shot::Logger log(host);
    Display* display = glfwGetX11Display();
    if (display == nullptr) {
        log.warn("region_selector：X11 Display 不可用，无法记录显示器几何");
        return;
    }
    const int screen = DefaultScreen(display);
    char summary[224] = {};
    std::snprintf(summary, sizeof(summary),
                  "region_selector：X11 screen=%d root=%dx%d（X screen 坐标即物理像素，origin=(0,0)）",
                  screen, DisplayWidth(display, screen), DisplayHeight(display, screen));
    log.info(summary);

    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (monitors == nullptr || count == 0) {
        log.warn("region_selector：GLFW 没有报告任何显示器（X11）");
        return;
    }
    for (int index = 0; index < count; ++index) {
        GLFWmonitor* monitor = monitors[index];
        const ScreenRect rect = monitorRectOf(monitor);
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        glfwGetMonitorContentScale(monitor, &scaleX, &scaleY);
        const char* name = glfwGetMonitorName(monitor);
        char line[320] = {};
        std::snprintf(line, sizeof(line),
                      "region_selector：显示器[%d] %s 矩形=(%d,%d) %dx%d content_scale=%.2fx%.2f%s",
                      index, (name != nullptr) ? name : "?", rect.x, rect.y, rect.width,
                      rect.height, scaleX, scaleY,
                      (monitor == glfwGetPrimaryMonitor()) ? " (主显示器)" : "");
        log.info(line);
    }
}

bool RegionWindow::Impl::x11ReadPanelInsets(const ScreenRect& monitor, PanelInsets& out) const {
    out = PanelInsets{};
    const shot::Logger log(host);
    if (monitor.width <= 0 || monitor.height <= 0) {
        log.debug("region_selector：显示器矩形无效，跳过 X11 _NET_WORKAREA 面板占位读取");
        return false;
    }
    Display* display = glfwGetX11Display();
    if (display == nullptr) {
        log.debug("region_selector：X11 Display 不可用，_NET_WORKAREA 面板占位按 0 处理");
        return false;
    }
    const Window root = DefaultRootWindow(display);
    unsigned char* workAreaData = nullptr;
    unsigned long workAreaItems = 0;
    if (!readCardinalArray(display, root, "_NET_WORKAREA", kMaxWorkAreaDesktops * 4,
                           &workAreaData, workAreaItems)) {
        log.debug(
            "region_selector：读不到 X11 _NET_WORKAREA（root 上没有该 CARDINAL 属性），"
            "面板占位按 0 处理");
        return false;
    }

    // 取当前虚拟桌面的那一组 x,y,w,h；`_NET_CURRENT_DESKTOP` 读不到 / 越界时退回桌面 0。
    unsigned long desktop = 0;
    unsigned char* desktopData = nullptr;
    unsigned long desktopItems = 0;
    if (readCardinalArray(display, root, "_NET_CURRENT_DESKTOP", 1, &desktopData,
                          desktopItems) &&
        desktopItems >= 1) {
        const long current = reinterpret_cast<const long*>(desktopData)[0];
        if (current > 0) {
            desktop = static_cast<unsigned long>(current);
        }
        XFree(desktopData);
    }
    if ((desktop + 1) * 4 > workAreaItems) {
        desktop = 0;  // 越界：退回第一个桌面的工作区（与 EWMH 属性顺序一致）
    }
    const auto* workArea = reinterpret_cast<const long*>(workAreaData);
    const std::size_t base = static_cast<std::size_t>(desktop) * 4;
    const long workX = workArea[base + 0];
    const long workY = workArea[base + 1];
    const long workWidth = workArea[base + 2];
    const long workHeight = workArea[base + 3];
    XFree(workAreaData);
    workAreaData = nullptr;

    const int screen = DefaultScreen(display);
    const long screenWidth = DisplayWidth(display, screen);
    const long screenHeight = DisplayHeight(display, screen);
    char summary[384] = {};
    if (workX < 0 || workY < 0 || workWidth <= 0 || workHeight <= 0 ||
        workX + workWidth > screenWidth || workY + workHeight > screenHeight) {
        std::snprintf(summary, sizeof(summary),
                      "region_selector：X11 _NET_WORKAREA=(%ld,%ld) %ldx%ld（桌面 %lu）数值异常"
                      "（X screen %ldx%ld），面板占位按 0 处理",
                      workX, workY, workWidth, workHeight, desktop, screenWidth, screenHeight);
        log.debug(summary);
        return false;
    }
    // 与显示器矩形相减得到四条边被面板占掉的厚度。减法用 long 做、最后再检查范围：
    // 工作区落在显示器外时 inset 会大于等于屏宽/屏高，那种数据一律按全 0 处理。
    const long top = std::max(0L, workY - monitor.y);
    const long left = std::max(0L, workX - monitor.x);
    const long right =
        std::max(0L, (static_cast<long>(monitor.x) + monitor.width) - (workX + workWidth));
    const long bottom =
        std::max(0L, (static_cast<long>(monitor.y) + monitor.height) - (workY + workHeight));
    if (top >= monitor.height || bottom >= monitor.height || left >= monitor.width ||
        right >= monitor.width) {
        std::snprintf(summary, sizeof(summary),
                      "region_selector：X11 _NET_WORKAREA=(%ld,%ld) %ldx%ld（桌面 %lu）与显示器 "
                      "(%d,%d) %dx%d 不相交 / inset 越界，面板占位按 0 处理",
                      workX, workY, workWidth, workHeight, desktop, monitor.x, monitor.y,
                      monitor.width, monitor.height);
        log.debug(summary);
        return false;
    }
    out.top = static_cast<int>(top);
    out.left = static_cast<int>(left);
    out.right = static_cast<int>(right);
    out.bottom = static_cast<int>(bottom);
    std::snprintf(summary, sizeof(summary),
                  "region_selector：X11 _NET_WORKAREA=(%ld,%ld) %ldx%ld（桌面 %lu）→ 显示器 "
                  "(%d,%d) %dx%d 面板 inset top=%d left=%d right=%d bottom=%d",
                  workX, workY, workWidth, workHeight, desktop, monitor.x, monitor.y,
                  monitor.width, monitor.height, out.top, out.left, out.right, out.bottom);
    log.info(summary);
    return true;
}

}  // namespace region_selector
