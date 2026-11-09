#pragma once

// ============================================================================
//  settings_window_impl.h —— 内部头：SettingsWindow 的 PIMPL 与窗口内部类型
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。窗口层基于 GLFW，跨平台；
//  GLFW 只以 `struct GLFWwindow;` 前置声明出现在这里，本头不拖 <GLFW/glfw3.h>，
//  更不含任何平台头。公共头 settings_window.h 不包含任何平台类型。
//
//  ImGui 是跨平台库（不是平台头），这里为了保存"未缩放的默认样式" include 了
//  imgui.h；它本来就通过 settings_panel_impl.h 进入本插件的内部头（同一依赖）。
//
//  定义按职责拆成三个 .cpp：
//    settings_window.cpp   —— 生命周期 / 窗口线程 / GLFW 事件循环 / 事件队列
//    settings_graphics.cpp —— GLFW 窗口 + ImGui 后端 + 字体与 DPI + 每帧渲染
//    settings_geometry.cpp —— 窗口几何持久化与屏幕内 clamp
//  原生文件对话框在 settings_dialog.cpp（不依赖窗口句柄，单独一个内部模块）。
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "imgui.h"

#include "plugin_api.h"
#include "settings_panel.h"
#include "settings_window.h"

struct GLFWwindow;  // 前置声明：内部头不拖 GLFW/GL 头文件

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

/** @brief 窗口几何（屏幕坐标，单位像素；GLFW 的"窗口位置/大小"指客户区）。 */
struct WindowGeometry {
    int x = 0;
    int y = 0;
    int width = kDefaultWindowWidth;
    int height = kDefaultWindowHeight;
};

/**
 * @brief SettingsWindow 的实现：GLFW 窗口、事件队列、线程握手全在这里。
 *
 * 声明集中在这个结构里，定义按职责分散到多个 .cpp（STYLE.md §6.7）。
 */
struct SettingsWindow::Impl {
    // ---- start() / stop() 跨线程共享的状态（mutex 保护） ----
    std::thread thread;
    std::mutex mutex;
    std::queue<QueuedEvent> queue;
    bool acceptingEvents = false;  // start 成功拉起线程后为 true；stop 一开始就置 false
    bool glfwInitialized = false;  // 任意线程据此判断能否 glfwPostEmptyEvent 唤醒
    bool quitRequested = false;    // stop()/host.shutdown 要求事件循环退出

    // ---- start() 与 UI 线程之间的"窗口 + ImGui 就绪了没有"握手 ----
    std::mutex readyMutex;
    std::condition_variable readyCv;
    bool ready = false;
    bool failed = false;

    const ShotHostServices* host = nullptr;
    bool busSubscribed = false;

    SettingsPanel panel;

    // ---- 以下只在 UI 线程（GLFW 的主线程）上访问 ----
    GLFWwindow* window = nullptr;
    bool imguiInitialized = false;
    bool visible = false;
    bool escHideRequested = false;  // 键盘回调只置位，隐藏动作在事件循环里做

    // ---- DPI 缩放（settings_graphics.cpp） ----
    // 1.92 的 ScaleAllSizes 是乘法（还会乘间距/圆角/边框），所以必须保留一份
    // "未缩放的默认样式"当基线：跨屏 DPI 变化时从基线重新放一次，不能在上一次
    // 结果上继续放大。FontScaleMain 是 1.92 的全局字号缩放（字体按需动态烘焙）。
    ImGuiStyle baseStyle;       // 未缩放的默认样式（CreateContext 之后、缩放之前）
    float contentScale = 1.0f;  // 已经应用到 style 的 content scale
    bool dpiScaleDirty = false; // content scale 回调置位；下一帧 / 下次显示时重建

    // ---- 生命周期与事件循环（settings_window.cpp） ----
    void run();
    void messageLoop();
    void drainEvents();
    void enqueue(const char* name, const char* data, std::uint32_t len);
    bool shouldQuit();
    void requestQuit();  // 任意线程可调：置退出标记并唤醒事件循环
    void showWindow();
    void hideWindow();
    bool initGlfw();
    bool createWindow();
    void shutdownGlfw();
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);
    static void onGlfwError(int errorCode, const char* description);
    static void onKey(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void onContentScale(GLFWwindow* window, float xScale, float yScale);

    // ---- ImGui / 字体与 DPI / 每帧渲染（settings_graphics.cpp） ----
    bool initImGui();
    void loadFonts();
    void applyContentScale();
    void refreshDpiScale();
    float queryContentScale() const;
    void shutdownGraphics();
    bool renderFrame();

    // ---- 窗口几何（settings_geometry.cpp） ----
    void saveGeometry();
    void restoreGeometry();
    static bool parseGeometry(const std::string& text, WindowGeometry& out);
    WindowGeometry clampToVisibleArea(const WindowGeometry& geometry) const;
};

}  // namespace settings_ui
