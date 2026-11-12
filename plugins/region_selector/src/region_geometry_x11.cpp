// region_geometry_x11 —— Linux/X11 的平台几何层：X screen 几何、光标位置、显示器矩形、
// 原生窗口句柄，以及首次框选时把实测数据写进宿主日志。
//
// 与 Windows 版（region_geometry.cpp）的差别：
//   · Windows 有"虚拟桌面"概念（多显示器可以摆成负原点），region 发布前要减虚拟
//     桌面原点；X11 的坐标原点就是 root 窗口的 (0,0)，所有显示器都挂在同一块
//     X screen 上，所以虚拟桌面原点恒为 (0,0)，不做减法。
//   · 光标位置用 XQueryPointer（root 坐标）；显示器矩形交给 GLFW（它内部用 RandR
//     查询输出布局，和 X screen 坐标同一套）。**不引入 xrandr 依赖**：本插件只需要
//     "显示器在哪、多大"，GLFW 已经提供了。
//   · 原生句柄是 X11 Window（glfwGetX11Window），交给窗口枚举排除覆盖层自己。
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

#include <cstdint>
#include <cstdio>

#include <X11/Xlib.h>

// 自己包含 Xlib 头；glfw3native.h 会连带声明 RRCrtc/RROutput 并拉 <X11/extensions/Xrandr.h>，
// 所以编译期需要 libxrandr-dev（GLFW 运行时本来也用它枚举显示器）。
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {

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

}  // namespace region_selector
