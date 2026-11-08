// settings_geometry —— 窗口几何的持久化与屏幕内 clamp。
//
// 关闭（X / Esc）或宿主退出时把 x,y,w,h 写进宿主配置的 settings_ui.window；
// 下次显示前恢复，并按显示器工作区 clamp——换分辨率、拔掉副显示器之后窗口
// 不会跑到屏幕外。不涉及渲染，也不碰 ImGui。

#include <algorithm>
#include <cstdio>
#include <string>

#include "settings_window_impl.h"  // 提供 <windows.h> 与 Impl 声明

#include "shot_plugin_helpers.h"

namespace settings_ui {
namespace {

// 窗口几何存在宿主配置里（不是插件配置目录），键名与 SETTINGS.md §6 的约定一致。
constexpr char kWindowGeometryKey[] = "settings_ui.window";

}  // namespace

void SettingsWindow::Impl::saveGeometry() {
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        wnd = hwnd;
    }
    if (wnd == nullptr) {
        return;
    }
    RECT rect{};
    if (GetWindowRect(wnd, &rect) == FALSE) {
        return;
    }
    // 用宿主配置服务（shot.host.config）持久化，不是插件配置目录：几何是"设置页自己"
    // 的界面状态，不占用任何插件的配置节。
    shot::Config config(host);
    if (!config.available()) {
        return;  // 老宿主没有配置服务：只在本次会话里保留几何
    }
    char text[64] = {};
    std::snprintf(text, sizeof(text), "%d,%d,%d,%d", static_cast<int>(rect.left),
                  static_cast<int>(rect.top), static_cast<int>(rect.right - rect.left),
                  static_cast<int>(rect.bottom - rect.top));
    if (config.setString(kWindowGeometryKey, text)) {
        config.save();  // 几何是低频写（关窗时一次），直接同步落盘
    }
}

void SettingsWindow::Impl::restoreGeometry() {
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        wnd = hwnd;
    }
    if (wnd == nullptr) {
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
        // 第一次运行：在光标所在显示器的工作区里居中，别用 CreateWindow 的默认位置。
        const HMONITOR monitor = MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo) != FALSE) {
            geometry.width = kDefaultWindowWidth;
            geometry.height = kDefaultWindowHeight;
            geometry.x = monitorInfo.rcWork.left + ((monitorInfo.rcWork.right -
                                                     monitorInfo.rcWork.left - geometry.width) /
                                                    2);
            geometry.y = monitorInfo.rcWork.top + ((monitorInfo.rcWork.bottom -
                                                    monitorInfo.rcWork.top - geometry.height) /
                                                   2);
        }
    }
    const WindowGeometry clamped = clampToVisibleArea(geometry);
    SetWindowPos(wnd, nullptr, clamped.x, clamped.y, clamped.width, clamped.height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
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

WindowGeometry SettingsWindow::Impl::clampToVisibleArea(const WindowGeometry& geometry) {
    WindowGeometry result = geometry;
    const RECT savedRect{geometry.x, geometry.y, geometry.x + geometry.width,
                         geometry.y + geometry.height};
    const HMONITOR monitor = MonitorFromRect(&savedRect, MONITOR_DEFAULTTONEAREST);
    RECT workArea{};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo) != FALSE) {
        workArea = monitorInfo.rcWork;
    } else {
        workArea.left = 0;
        workArea.top = 0;
        workArea.right = GetSystemMetrics(SM_CXSCREEN);
        workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int workWidth = static_cast<int>(workArea.right - workArea.left);
    const int workHeight = static_cast<int>(workArea.bottom - workArea.top);
    if (workWidth <= 0 || workHeight <= 0) {
        return result;  // 拿不到工作区：保持原样，至少别崩
    }
    // 至少留出最小尺寸；比屏幕还大时以屏幕为准。位置再夹进工作区，这样换了分辨率、
    // 拔掉副显示器之后，上一次存下的几何也不会把窗口藏到屏幕外面。
    result.width = std::min(std::max(result.width, kMinimumWindowWidth), workWidth);
    result.height = std::min(std::max(result.height, kMinimumWindowHeight), workHeight);
    result.x = std::clamp(result.x, static_cast<int>(workArea.left),
                          static_cast<int>(workArea.right) - result.width);
    result.y = std::clamp(result.y, static_cast<int>(workArea.top),
                          static_cast<int>(workArea.bottom) - result.height);
    return result;
}

}  // namespace settings_ui
