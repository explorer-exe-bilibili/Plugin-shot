// region_geometry —— 平台几何层：显示器矩形、光标位置、物理虚拟桌面坐标换算，
// 以及启动时把实测 DPI 数据写进宿主日志。
//
// 坐标系（本机实测，README「坐标语义与 DPI 实测」有逐条数据）：
//   · 宿主 main() 在任何窗口/采集之前把进程声明成 PER_MONITOR_AWARE_V2，所以
//     GetSystemMetrics(SM_*VIRTUALSCREEN)、GetCursorPos、GLFW 的 monitor/window/
//     cursor 坐标都是同一套物理像素：150% 屏上整屏窗口就是 2560x1600 物理像素，
//     content scale = 1.5 只表示 DPI，绝不能拿来乘坐标（乘了会得到 1.5 倍错误区域）。
//   · 虚拟桌面左上角不一定是 (0,0)：左侧/上方的显示器会让 SM_XVIRTUALSCREEN /
//     SM_YVIRTUALSCREEN 变成负数。capture.request 的 region 原点 = 虚拟桌面左上角，
//     所以发布前要做 absolute - virtualDesktopOrigin 的减法（本机两块屏都在原点的
//     右侧/下方，减法结果为 0，但代码不能省）。
//
// 只有平台层文件包含 <windows.h>（本文件 / region_background.cpp / window_target.cpp）：
// Win32 类型留在平台层，公共头与其它 .cpp 不碰它。本文件里的函数只服务覆盖层的
// 显示与换算，与窗口生命周期无关。

#include <algorithm>
#include <cstdint>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GLFW/glfw3.h>  // 跨平台窗口库：显示器位置、视频模式、内容缩放

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>  // glfwGetWin32Window：把覆盖层 HWND 交给窗口枚举排除

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {

std::uintptr_t RegionWindow::Impl::nativeWindowHandle() const {
    // 覆盖层自己是全屏置顶窗口，枚举时如果不按 HWND 排除，命中测试永远选中它。
    // HWND 只在平台层出现；交给窗口枚举时转成整数，避免 Impl 头拖入 windows.h。
    if (window == nullptr) {
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(glfwGetWin32Window(window));
}

ScreenRect RegionWindow::Impl::monitorRectOf(GLFWmonitor* monitor) {
    ScreenRect rect;
    if (monitor == nullptr) {
        return rect;
    }
    // 整块显示器的矩形 = 位置 + 当前视频模式尺寸（不是工作区：覆盖层要盖住任务栏）。
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
    // 用 Win32 GetCursorPos 而不是 GLFW：这里还没有窗口，glfwGetCursorPos 需要窗口参数。
    // 进程是 PMv2，GetCursorPos 与 GLFW 的屏幕坐标同一套物理像素（README 有实测）。
    POINT cursor = {};
    if (GetCursorPos(&cursor) == FALSE) {
        return false;
    }
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int index = 0; index < count; ++index) {
        const ScreenRect rect = monitorRectOf(monitors[index]);
        if (rect.width <= 0 || rect.height <= 0) {
            continue;
        }
        if (cursor.x >= rect.x && cursor.x < rect.x + rect.width && cursor.y >= rect.y &&
            cursor.y < rect.y + rect.height) {
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

void RegionWindow::Impl::virtualDesktopOrigin(int& x, int& y) const {
    x = 0;
    y = 0;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (monitors == nullptr || count == 0) {
        return;
    }
    const ScreenRect first = monitorRectOf(monitors[0]);
    x = first.x;
    y = first.y;
    for (int index = 1; index < count; ++index) {
        const ScreenRect rect = monitorRectOf(monitors[index]);
        if (rect.width <= 0 || rect.height <= 0) {
            continue;
        }
        // 虚拟桌面 origin = 所有显示器矩形的最小 x / 最小 y，与 screen_capture 的
        // describe 用 GetSystemMetrics(SM_XVIRTUALSCREEN/SM_YVIRTUALSCREEN) 一致。
        x = std::min(x, rect.x);
        y = std::min(y, rect.y);
    }
}

void RegionWindow::Impl::logMonitorMetrics() const {
    const shot::Logger log(host);
    // 启动时把实测数据写一行日志：DPI/多显示器问题的第一现场，排查时不用再跑探针。
    // GetSystemMetrics 在 PMv2 进程里是物理像素，与 screen_capture 的 describe 同源。
    char summary[192] = {};
    std::snprintf(summary, sizeof(summary),
                  "region_selector：虚拟桌面(物理像素) origin=(%d,%d) size=%dx%d",
                  GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                  GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    log.info(summary);

    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int index = 0; index < count; ++index) {
        GLFWmonitor* monitor = monitors[index];
        const ScreenRect rect = monitorRectOf(monitor);
        int workX = 0;
        int workY = 0;
        int workWidth = 0;
        int workHeight = 0;
        glfwGetMonitorWorkarea(monitor, &workX, &workY, &workWidth, &workHeight);
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        glfwGetMonitorContentScale(monitor, &scaleX, &scaleY);
        const char* name = glfwGetMonitorName(monitor);
        char line[320] = {};
        std::snprintf(line, sizeof(line),
                      "region_selector：显示器[%d] %s 矩形=(%d,%d) %dx%d 工作区=(%d,%d) %dx%d "
                      "content_scale=%.2fx%.2f%s",
                      index, (name != nullptr) ? name : "?", rect.x, rect.y, rect.width,
                      rect.height, workX, workY, workWidth, workHeight, scaleX, scaleY,
                      (monitor == glfwGetPrimaryMonitor()) ? " (主显示器)" : "");
        log.info(line);
    }
}

}  // namespace region_selector
