// settings_geometry —— 窗口几何的持久化与屏幕内 clamp。
//
// 关闭（X / Esc）或宿主退出时把 x,y,w,h 写进宿主配置的 settings_ui.window；
// 下次显示前恢复，并按显示器工作区 clamp——换分辨率、拔掉副显示器之后窗口
// 不会跑到屏幕外。不涉及渲染，也不碰 ImGui。
//
// 坐标语义：GLFW 的"窗口位置/大小"指内容区（不含窗口边框），与大小一起存进同一个
// "x,y,w,h" 字符串；clamp 时用 glfwGetWindowFrameSize 把标题栏/边框一起算进去，
// 保证【外层矩形】完整落在工作区内（含底边与标题栏）。
// 坐标空间：GLFW 的屏幕坐标（Windows 上进程被 GLFW 设为 per-monitor DPI aware，
// 所以是物理像素，150% 缩放下标题栏 45px 即为物理值）；跨 DPI 多显示器未验证（见
// clampToVisibleArea 末尾的多 DPI 说明）。旧版本（Win32 时代）存的可能是虚拟化的外层矩形，
// 按内容区 + 当前 DPI 恢复会有少量偏移，但 clamp 保证它仍完整可见。

#include <algorithm>
#include <cstdio>
#include <string>

#include "settings_window_impl.h"  // 提供 Impl 声明（不含任何平台头）

#include <GLFW/glfw3.h>  // 跨平台窗口库：窗口位置/大小、显示器工作区

#include "shot_plugin_helpers.h"

namespace settings_ui {
namespace {

// 窗口几何存在宿主配置里（不是插件配置目录），键名与 SETTINGS.md §6 的约定一致。
constexpr char kWindowGeometryKey[] = "settings_ui.window";

/**
 * @brief 取一个显示器的工作区；拿不到时退回它的视频模式（去掉任务栏/面板就更不到了）。
 * @param[in]  monitor 显示器；可为 nullptr（此时返回 false）。
 * @param[out] work    工作区矩形（屏幕坐标）。
 * @return true = 拿到了可用的矩形（宽高 > 0）。
 * @note 只是几何 clamp 的本地工具，不属于任何类、只在本 TU 用，所以放匿名命名空间
 *       （STYLE.md §6.7）。
 */
bool monitorWorkArea(GLFWmonitor* monitor, WindowGeometry& work) {
    if (monitor == nullptr) {
        return false;
    }
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    glfwGetMonitorWorkarea(monitor, &x, &y, &width, &height);
    if (width > 0 && height > 0) {
        work.x = x;
        work.y = y;
        work.width = width;
        work.height = height;
        return true;
    }
    // 某些平台/窗口管理器在窗口可见前拿不到工作区：退回视频模式分辨率。
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    if (mode == nullptr || mode->width <= 0 || mode->height <= 0) {
        return false;
    }
    work.x = 0;
    work.y = 0;
    work.width = mode->width;
    work.height = mode->height;
    return true;
}

}  // namespace

void SettingsWindow::Impl::saveGeometry() {
    if (window == nullptr) {
        return;
    }
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    glfwGetWindowPos(window, &x, &y);
    glfwGetWindowSize(window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    // 用宿主配置服务（shot.host.config）持久化，不是插件配置目录：几何是"设置页自己"
    // 的界面状态，不占用任何插件的配置节。
    shot::Config config(host);
    if (!config.available()) {
        return;  // 老宿主没有配置服务：只在本次会话里保留几何
    }
    char text[64] = {};
    std::snprintf(text, sizeof(text), "%d,%d,%d,%d", x, y, width, height);
    if (config.setString(kWindowGeometryKey, text)) {
        config.save();  // 几何是低频写（关窗时一次），直接同步落盘
    }
}

void SettingsWindow::Impl::restoreGeometry() {
    if (window == nullptr) {
        return;
    }

    WindowGeometry geometry;
    bool hasSaved = false;
    shot::Config config(host);
    if (config.available()) {
        WindowGeometry saved;
        if (parseGeometry(config.getString(kWindowGeometryKey, ""), saved)) {
            geometry = saved;
            hasSaved = true;
        }
    }
    if (!hasSaved) {
        // 第一次运行：在主显示器工作区里居中，别用 GLFW 的默认摆放位置。
        WindowGeometry work;
        if (monitorWorkArea(glfwGetPrimaryMonitor(), work)) {
            geometry.width = kDefaultWindowWidth;
            geometry.height = kDefaultWindowHeight;
            geometry.x = work.x + (work.width - geometry.width) / 2;
            geometry.y = work.y + (work.height - geometry.height) / 2;
        }
    }
    const WindowGeometry clamped = clampToVisibleArea(geometry);
    // 先定位再定尺寸：只改尺寸会保留旧位置，两次调用顺序无所谓，但分开更好读。
    glfwSetWindowPos(window, clamped.x, clamped.y);
    glfwSetWindowSize(window, clamped.width, clamped.height);
}

bool SettingsWindow::Impl::parseGeometry(const std::string& text, WindowGeometry& out) {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (std::sscanf(text.c_str(), "%d,%d,%d,%d", &x, &y, &width, &height) != 4) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }
    out.x = x;
    out.y = y;
    out.width = width;
    out.height = height;
    return true;
}

WindowGeometry SettingsWindow::Impl::clampToVisibleArea(const WindowGeometry& geometry) const {
    WindowGeometry result = geometry;

    // 找到保存位置所在的工作区；找不到（比如显示器被拔掉）就用主显示器。
    WindowGeometry work;
    bool hasWorkArea = false;
    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    for (int index = 0; index < monitorCount; ++index) {
        WindowGeometry candidate;
        if (!monitorWorkArea(monitors[index], candidate)) {
            continue;
        }
        const bool contains = geometry.x >= candidate.x && geometry.x < candidate.x + candidate.width &&
                              geometry.y >= candidate.y && geometry.y < candidate.y + candidate.height;
        if (contains) {
            work = candidate;
            hasWorkArea = true;
            break;
        }
    }
    if (!hasWorkArea) {
        hasWorkArea = monitorWorkArea(glfwGetPrimaryMonitor(), work);
    }
    if (!hasWorkArea || work.width <= 0 || work.height <= 0) {
        return result;  // 拿不到工作区：保持原样，至少别崩
    }

    // 坐标语义（实测确认，别再猜）：GLFW 的窗口位置 / 尺寸 / 工作区 / 边框是同一个坐标
    // 空间。Windows 上 GLFW 把进程设为 per-monitor DPI aware，所以这里全是物理像素；
    // 150% 缩放下一个 760x620 的内容区，外层是 782x676、标题栏 45px，都是物理像素。
    // 因此下面可以直接整数加减。
    //
    // 但保存的是【内容区】坐标，而"窗口完整可见"要求【外层矩形】也落在工作区内：
    //   外层左 = 内容区 x - frameLeft，外层右 = 内容区 x + width + frameRight（上/下同理）。
    // 只夹内容区会让标题栏顶出屏幕、或右边框/底边框被工作区裁掉（旧实现就漏了这一步）。
    int frameLeft = 0;
    int frameTop = 0;
    int frameRight = 0;
    int frameBottom = 0;
    if (window != nullptr) {
        glfwGetWindowFrameSize(window, &frameLeft, &frameTop, &frameRight, &frameBottom);
    }
    const int maxContentWidth = work.width - frameLeft - frameRight;
    const int maxContentHeight = work.height - frameTop - frameBottom;
    if (maxContentWidth <= 0 || maxContentHeight <= 0) {
        return result;  // 工作区比窗口边框还小（畸形环境）：保持原样，至少别崩
    }

    // 先定尺寸：内容区不小于最小尺寸，也不大到外层矩形容不下。
    result.width = std::min(std::max(result.width, kMinimumWindowWidth), maxContentWidth);
    result.height = std::min(std::max(result.height, kMinimumWindowHeight), maxContentHeight);
    // 再定位置：把内容区坐标夹到"外层矩形刚好落在工作区内"的取值范围。
    const int minX = work.x + frameLeft;
    const int minY = work.y + frameTop;
    const int maxX = work.x + work.width - frameRight - result.width;
    const int maxY = work.y + work.height - frameBottom - result.height;
    result.x = std::clamp(result.x, minX, std::max(minX, maxX));
    result.y = std::clamp(result.y, minY, std::max(minY, maxY));
    // TODO: 单机只验证了主显示器 150% 这一种 DPI。多显示器且各屏 DPI 不同时，窗口边框
    // 尺寸会随所在显示器变化（这里用的是窗口当前 DPI 的边框），跨屏情况等有双 DPI 屏
    // 的环境再验证并按目标显示器重新取 frame size。
    return result;
}

}  // namespace settings_ui
