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
// 所以是物理像素；150% 缩放下标题栏 45px、100% 缩放下 31px，都是物理值）。
//
// 多显示器不同 DPI（真机实测 主屏 150% + 副屏 100%）：
//   · 保存的是内容区物理坐标，不保存"哪块显示器"——恢复时按坐标落在哪个显示器矩形
//     里重新找目标显示器（换显示器/拔屏后仍安全）；
//   · 边框尺寸随显示器 DPI 变化（150% → L11/T45/R11/B11，100% → L8/T31/R8/B8），
//     所以恢复时必须先让（仍然隐藏的）窗口进入目标显示器，再读 glfwGetWindowFrameSize：
//     实测 glfwSetWindowPos 返回时 GLFW 已同步更新 content scale 与 frame size，
//     不需要等 poll；反过来"先取 frame 再搬窗口"会拿到创建显示器的边框，clamp 偏大。
//   · 实测 GLFW 不按 WM_DPICHANGED 的建议矩形改窗口尺寸（内容区保持 760x620），
//     只在 DPI 切换瞬间对位置有几十像素的微调；因此最后一步重新 setPos 即可稳定。
// 旧版本（Win32 时代）存的可能是虚拟化的外层矩形，按内容区 + 当前 DPI 恢复会有少量
// 偏移，但 clamp 保证它仍完整可见。

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

/**
 * @brief 找包含某个屏幕坐标点的显示器（按整块显示器矩形，不是工作区）。
 *
 * 用显示器矩形而不是工作区：保存的位置可能落在任务栏那条里（y > 工作区高度），
 * 那仍然属于这块显示器，应该恢复/夹到它的工作区，而不是掉回主显示器。
 *
 * @param[in] x 屏幕坐标 x（物理像素）。
 * @param[in] y 屏幕坐标 y（物理像素）。
 * @return 命中显示器；没有（点在显示器之间的缝隙 / 显示器被拔掉）返回 nullptr。
 * @note 只服务几何恢复/clamp，放匿名命名空间（STYLE.md §6.7）。
 */
GLFWmonitor* monitorAtPoint(int x, int y) {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int index = 0; index < count; ++index) {
        GLFWmonitor* monitor = monitors[index];
        int monitorX = 0;
        int monitorY = 0;
        glfwGetMonitorPos(monitor, &monitorX, &monitorY);
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (mode == nullptr || mode->width <= 0 || mode->height <= 0) {
            continue;
        }
        if (x >= monitorX && x < monitorX + mode->width && y >= monitorY &&
            y < monitorY + mode->height) {
            return monitor;
        }
    }
    return nullptr;
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
    // 存内容区物理坐标：不存"第几块显示器"（索引会随插拔/启用禁用变化），恢复时按
    // 坐标重新找目标显示器。跨 DPI 屏的边框差异在恢复侧按目标显示器重新取。
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

    // 目标显示器：保存位置落在哪块屏就用哪块；第一次运行 / 显示器被拔掉时用主显示器。
    GLFWmonitor* target = hasSaved ? monitorAtPoint(geometry.x, geometry.y) : nullptr;
    if (target == nullptr) {
        target = glfwGetPrimaryMonitor();
    }
    WindowGeometry work;
    const bool hasWorkArea = monitorWorkArea(target, work);
    if (!hasSaved) {
        // 第一次运行：在目标显示器工作区里居中，别用 GLFW 的默认摆放位置。
        geometry.width = kDefaultWindowWidth;
        geometry.height = kDefaultWindowHeight;
        if (hasWorkArea) {
            geometry.x = work.x + (work.width - geometry.width) / 2;
            geometry.y = work.y + (work.height - geometry.height) / 2;
        }
    }

    // 先把（仍隐藏的）窗口搬进目标显示器的工作区。跨 DPI 屏时这一步会触发
    // WM_DPICHANGED，GLFW 同步更新 content scale 与 glfwGetWindowFrameSize（实测：
    // setPos 返回时就是目标屏的边框），后面的 clamp 才能按目标屏的边框来夹。
    if (hasWorkArea) {
        glfwSetWindowPos(window, work.x + 100, work.y + 100);
    }

    const WindowGeometry clamped = clampToVisibleArea(geometry);
    // 先定尺寸再定位置：DPI 切换瞬间窗口管理器可能对位置做过微调，最后一次 setPos
    // 把位置钉死；窗口已经在目标显示器上，这两步不会再触发 DPI 变化。
    glfwSetWindowSize(window, clamped.width, clamped.height);
    glfwSetWindowPos(window, clamped.x, clamped.y);
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

    // 找保存位置所在显示器的工作区；找不到（比如显示器被拔掉）就用主显示器。
    GLFWmonitor* target = monitorAtPoint(geometry.x, geometry.y);
    if (target == nullptr) {
        target = glfwGetPrimaryMonitor();
    }
    WindowGeometry work;
    if (!monitorWorkArea(target, work) || work.width <= 0 || work.height <= 0) {
        return result;  // 拿不到工作区：保持原样，至少别崩
    }

    // 坐标语义（实测确认，别再猜）：GLFW 的窗口位置 / 尺寸 / 工作区 / 边框是同一个坐标
    // 空间。Windows 上 GLFW 把进程设为 per-monitor DPI aware，所以这里全是物理像素；
    // 150% 缩放下一个 760x620 的内容区外层是 782x676、标题栏 45px，100% 缩放下是
    // 776x651、标题栏 31px（本机双屏实测）。
    //
    // 但保存的是【内容区】坐标，而"窗口完整可见"要求【外层矩形】也落在工作区内：
    //   外层左 = 内容区 x - frameLeft，外层右 = 内容区 x + width + frameRight（上/下同理）。
    // 只夹内容区会让标题栏顶出屏幕、或右边框/底边框被工作区裁掉（旧实现就漏了这一步）。
    //
    // 边框必须按【目标显示器】的 DPI 取：restoreGeometry 在调用本函数之前已经把隐藏
    // 窗口搬进目标显示器，所以这里的 glfwGetWindowFrameSize 返回的是目标屏的边框
    // （多 DPI 的关键，实测 setPos 后同步生效，不需要 poll）。
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
    return result;
}

}  // namespace settings_ui
