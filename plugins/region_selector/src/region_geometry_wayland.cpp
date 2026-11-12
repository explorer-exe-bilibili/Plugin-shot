// region_geometry_wayland —— Linux/Wayland 的平台几何层（显示器、坐标基准、日志）。
//
// 与 Windows 版（region_geometry.cpp）的差别全部来自 Wayland 的硬限制，不是实现偷懒：
//   · **没有全局光标位置**：客户端只有在自己的窗口里才拿得到指针坐标，所以开窗口
//     之前无法判断"光标在哪块屏"。这里退回当前/主显示器；窗口最终落在哪个输出由
//     合成器决定（README「Linux / Wayland」一节）。
//   · **没有显示器在虚拟桌面里的位置**：GLFW 的 glfwGetMonitorPos 在 Wayland 上恒为
//     (0,0)。本插件不发布屏幕坐标（裁剪用截图图像像素），所以这两个值只用于日志。
//   · **没有能交给窗口枚举的原生句柄**：nativeWindowHandle() 恒为 0，覆盖层因此
//     不会去枚举全局窗口（Wayland 不允许），悬停吸附关闭、单击=取消。
//
// 窗口尺寸/缩放由合成器最终决定：本文件只提供"请求用的"显示器矩形与 content scale。

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include <GLFW/glfw3.h>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {

std::uintptr_t RegionWindow::Impl::nativeWindowHandle() const {
    // Wayland 没有全局窗口枚举，也没有可以传给枚举函数的原生句柄：
    // 恒返回 0 就是"悬停吸附不可用"的开关（updateHover 在句柄为 0 时直接返回）。
    return 0;
}

ScreenRect RegionWindow::Impl::monitorRectOf(GLFWmonitor* monitor) {
    ScreenRect rect;
    if (monitor == nullptr) {
        return rect;
    }
    // 位置在 Wayland 上恒为 (0,0)（协议不提供输出位置），尺寸取当前视频模式。
    glfwGetMonitorPos(monitor, &rect.x, &rect.y);
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    if (mode != nullptr) {
        rect.width = mode->width;
        rect.height = mode->height;
    }
    return rect;
}

bool RegionWindow::Impl::monitorUnderCursor(ScreenRect& out, float& contentScale) const {
    contentScale = 1.0f;
    // Wayland 拿不到"光标所在输出"：优先用窗口当前（或将来）全屏的输出，其次主显示器。
    // 真正显示时仍由合成器决定实际位置，所以这里的矩形只用于日志与窗口尺寸的初值。
    GLFWmonitor* monitor = (window != nullptr) ? glfwGetWindowMonitor(window) : nullptr;
    if (monitor == nullptr) {
        monitor = glfwGetPrimaryMonitor();
    }
    if (monitor == nullptr) {
        return false;
    }
    out = monitorRectOf(monitor);
    if (out.width <= 0 || out.height <= 0) {
        return false;
    }
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    glfwGetMonitorContentScale(monitor, &scaleX, &scaleY);
    // content scale 只放大字号/折角，绝不乘坐标（与 Windows 版同一条纪律）。
    contentScale = (scaleX > 0.0f) ? scaleX : ((scaleY > 0.0f) ? scaleY : 1.0f);
    return true;
}

void RegionWindow::Impl::virtualDesktopOrigin(int& x, int& y) const {
    // Wayland 分支不发布屏幕坐标（裁剪用截图图像像素），这个换算没有使用点。
    // 保留实现只是为了与 Windows 版的 Impl 接口一致；恒返回 (0,0)。
    x = 0;
    y = 0;
}

void RegionWindow::Impl::logMonitorMetrics() const {
    const shot::Logger log(host);
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (monitors == nullptr || count == 0) {
        log.warn("region_selector：GLFW 没有报告任何显示器");
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
                      "region_selector：显示器[%d] %s 视频模式=%dx%d content_scale=%.2fx%.2f%s"
                      "（Wayland 不提供输出位置，位置恒为 0,0）",
                      index, (name != nullptr) ? name : "?", rect.width, rect.height, scaleX,
                      scaleY, (monitor == glfwGetPrimaryMonitor()) ? " (主显示器)" : "");
        log.info(line);
    }
}

}  // namespace region_selector
