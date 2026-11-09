// region_graphics —— 覆盖层的图形层：ImGui 后端、中文字体、DPI 字号缩放、每帧渲染。
//
// GLFW 窗口与 GL 上下文在 region_window.cpp 创建；这些函数只在插件的 UI 线程上执行
// （GLFW 与 ImGui 都不是线程安全的）。
//
// DPI 与字号：进程是 PMv2，窗口的 framebuffer == 窗口大小（物理像素），所以 ImGui
// 的坐标天然就是物理像素。content scale 只用来放大【字号】：150% 屏上用 1.5 倍字号，
// 字看起来才和 100% 屏一样大；绝不能用它去乘坐标（坐标已经是物理像素）。
//
// 和 settings_ui 一样，imgui 后端已经编进 vcpkg 的 imgui.lib（glfw-binding +
// opengl3-binding），这里只包含后端头文件，不抄实现。

#include <cstdio>
#include <string>

#include "region_window_impl.h"  // 提供 Impl 声明（不含任何平台头）

#include <GLFW/glfw3.h>  // 跨平台窗口库：这里主要用它提供的 GL 1.1 声明

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

// 逻辑字号；覆盖层的字号再乘显示器 content scale（见 applyMonitorScale）。
constexpr float kFontSizePixels = 18.0f;

// 中文字体按"显示效果从好到差"回退。覆盖层只有提示/尺寸两类短文本，缺字体时
// 退回 ImGui 内置字体（中文会显示成方块），但框选功能不受影响。
const char* const kFontCandidates[] = {
    "C:\\Windows\\Fonts\\msyh.ttc",
    "C:\\Windows\\Fonts\\msyhbd.ttc",
    "C:\\Windows\\Fonts\\simhei.ttf",
    "C:\\Windows\\Fonts\\simsun.ttc",
};

/**
 * @brief 从 GL_VERSION 字符串提出版本号，编码成 major*100 + minor*10。
 *
 * 例："3.3.0 NVIDIA 566.36" -> 330，"4.6.0" -> 460。解析失败返回 0。
 * @note 只服务本文件的 initImGui，与覆盖层逻辑无关，所以留在本 TU 的匿名命名空间
 *       （STYLE.md §6.7）。
 */
int parseGlVersion(const char* versionText) {
    int major = 0;
    int minor = 0;
    if (versionText == nullptr || std::sscanf(versionText, "%d.%d", &major, &minor) != 2) {
        return 0;
    }
    return major * 100 + minor * 10;
}

}  // namespace

bool RegionWindow::Impl::initImGui() {
    // GL 上下文已在 createWindow 里 make current；先确认版本，再建 ImGui。
    // 版本过低时干净失败：否则 ImGui 后端会在 NewFrame 里建 shader 失败并 IM_ASSERT，
    // 把 Debug 版宿主直接断言掉（settings_ui 踩过同样的坑）。
    const char* versionText = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const int glVersion = parseGlVersion(versionText);
    const std::string versionLabel = (versionText != nullptr) ? versionText : "unknown";
    if (glVersion < 300) {
        shot::Logger(host).error("[region_selector] OpenGL 版本过低（" + versionLabel +
                                 "），覆盖层需要 OpenGL 3.0+");
        return false;
    }
    const char* glslVersion = (glVersion >= 320) ? "#version 150" : "#version 130";
    shot::Logger(host).info("[region_selector] OpenGL " + versionLabel + "，GLSL " + glslVersion);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // 覆盖层不落任何 ImGui 状态文件：不要在宿主工作目录生成 imgui.ini / imgui_log.txt。
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    applyMonitorScale(1.0f);
    loadFonts();

    // 失败时把已经建到一半的 ImGui 资源在本函数内收干净：后端的 Shutdown 不能对
    // 没初始化成功的后端调用（那边是 IM_ASSERT），所以逐层回退。
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        shot::Logger(host).error("[region_selector] ImGui GLFW 后端初始化失败");
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        shot::Logger(host).error("[region_selector] ImGui OpenGL3 后端初始化失败");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    // 不让 ImGui 改系统光标：覆盖层要一直显示十字光标（glfwCreateStandardCursor 设的），
    // 否则后端每帧按 io.MouseCursor 把光标重置成箭头。
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // 提前建一次设备对象：NewFrame 里的同名失败路径是 IM_ASSERT，Debug 宿主上会直接
    // 断言退出。先建一遍就把"驱动不支持这套 shader"变成干净的初始化失败。
    if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
        shot::Logger(host).error("[region_selector] OpenGL 着色器初始化失败（驱动不支持？）");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    imguiInitialized = true;
    return true;
}

void RegionWindow::Impl::loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const ImWchar* glyphRanges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
    for (const char* path : kFontCandidates) {
        // 文件不存在 / 读不了时返回 nullptr（不抛异常），正好用来做"按顺序尝试"。
        if (io.Fonts->AddFontFromFileTTF(path, kFontSizePixels, nullptr, glyphRanges) != nullptr) {
            shot::Logger(host).info(std::string("region_selector：中文字体 ") + path);
            return;
        }
    }
    // 一个系统字体都没有（极简系统）：退回内置字体。只是显示效果降级，不影响功能。
    io.Fonts->AddFontDefault();
    shot::Logger(host).warn("region_selector：系统中文字体都不可用，退回 ImGui 内置字体（中文会显示成方块）");
}

void RegionWindow::Impl::applyMonitorScale(float scale) {
    // 只放大字号（提示/尺寸标签），不缩放布局：覆盖层用的是绝对坐标绘制，
    // 没有 ImGui 控件需要 ScaleAllSizes。1.92 用 style.FontScaleMain 代替了
    // 旧版 io.FontGlobalScale（旧字段 > 1 时会触发断言）。
    ImGui::GetStyle().FontScaleMain = (scale > 0.0f) ? scale : 1.0f;
}

void RegionWindow::Impl::shutdownGraphics() {
    if (imguiInitialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }
}

bool RegionWindow::Impl::renderFrame() {
    if (window == nullptr || !imguiInitialized) {
        return false;
    }
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        return false;  // 窗口还没有可用 framebuffer：viewport 与 NewFrame 都不该跑
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    drawOverlay();  // region_overlay.cpp：整屏遮罩 + 十字线 + 选区高亮 + 尺寸标签
    ImGui::Render();

    glViewport(0, 0, framebufferWidth, framebufferHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
    return true;
}

}  // namespace region_selector
