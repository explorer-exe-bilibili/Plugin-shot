#pragma once

// ============================================================================
//  settings_window_impl.h —— 内部头：SettingsWindow 的 PIMPL 与窗口内部类型
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。这里会出现 Win32 类型
//  （HWND / HDC / HGLRC），因为窗口线程确实要持有它们；但公共头
//  settings_window.h 不包含 <windows.h>，宿主与其它插件看不到任何 Win32 句柄。
//
//  定义按职责拆成三个 .cpp：
//    settings_window.cpp   —— 生命周期 / 窗口线程 / 消息循环 / 事件队列 / WndProc
//    settings_graphics.cpp —— WGL + ImGui + 字体与 DPI + 每帧渲染
//    settings_geometry.cpp —— 窗口几何持久化与屏幕内 clamp
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX  // 防止 <windows.h> 的 min/max 宏把 std::min / std::max 顶掉
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "plugin_api.h"
#include "settings_panel.h"
#include "settings_window.h"

namespace settings_ui {

/** @brief 窗口默认宽度（逻辑像素）。 */
inline constexpr int kDefaultWindowWidth = 760;

/** @brief 窗口默认高度（逻辑像素）。 */
inline constexpr int kDefaultWindowHeight = 620;

/** @brief 窗口允许缩到的最小宽度。 */
inline constexpr int kMinimumWindowWidth = 560;

/** @brief 窗口允许缩到的最小高度。 */
inline constexpr int kMinimumWindowHeight = 420;

/** @brief 队列里的一个事件：主题名 + 负载字节（总线负载在上限 64 KiB 以内）。 */
struct QueuedEvent {
    std::string name;
    std::string payload;
};

/** @brief 窗口几何（屏幕坐标，单位像素）。 */
struct WindowGeometry {
    int x = 0;
    int y = 0;
    int width = kDefaultWindowWidth;
    int height = kDefaultWindowHeight;
};

/**
 * @brief SettingsWindow 的实现：窗口句柄、GL 上下文、事件队列、线程握手全在这里。
 *
 * 声明集中在这个结构里，定义按职责分散到多个 .cpp（STYLE.md §6.7）。
 */
struct SettingsWindow::Impl {
    // ---- start() / stop() 跨线程共享的状态（mutex 保护） ----
    std::thread thread;
    std::mutex mutex;
    std::queue<QueuedEvent> queue;
    HWND hwnd = nullptr;
    bool acceptingEvents = false;  // start 成功拉起线程后为 true；stop 一开始就置 false

    // ---- start() 与 UI 线程之间的"窗口 + ImGui 就绪了没有"握手 ----
    std::mutex readyMutex;
    std::condition_variable readyCv;
    bool ready = false;
    bool failed = false;

    const ShotHostServices* host = nullptr;
    bool busSubscribed = false;

    SettingsPanel panel;

    // ---- 以下只在 UI 线程上访问（线程创建后到退出前） ----
    HDC windowDc = nullptr;
    HGLRC glContext = nullptr;
    const char* glslVersion = "#version 130";
    bool imguiInitialized = false;
    bool vsyncEnabled = false;
    bool visible = false;
    float dpiScale = 1.0f;

    // ---- 生命周期与窗口线程（settings_window.cpp） ----
    void run();
    void messageLoop();
    void drainEvents();
    void enqueue(const char* name, const char* data, std::uint32_t len);
    void showWindow();
    void hideWindow();
    static LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);

    // ---- WGL / ImGui / 每帧渲染（settings_graphics.cpp） ----
    bool initOpenGl(HWND wnd);
    bool initImGui(HWND wnd);
    void loadFonts();
    void applyDpiScale(HWND wnd);
    void shutdownGraphics();
    bool renderFrame();

    // ---- 窗口几何（settings_geometry.cpp） ----
    void saveGeometry();
    void restoreGeometry();
    static bool parseGeometry(const std::string& text, WindowGeometry& out);
    static WindowGeometry clampToVisibleArea(const WindowGeometry& geometry);
};

}  // namespace settings_ui
