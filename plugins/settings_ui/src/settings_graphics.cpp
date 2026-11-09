// settings_graphics —— SettingsWindow 的图形层：ImGui 后端、字体与 DPI、每帧渲染
// （glViewport + ImGui NewFrame/Render/RenderDrawData + glfwSwapBuffers）。
//
// GLFW 窗口与 GL 上下文在 settings_window.cpp 创建；这些函数只在插件的 UI 线程上执行。
// 跨平台要点：
//   · <GLFW/glfw3.h> 在非 Apple 平台会带 <GL/gl.h>（Windows 上 GLFW 自己定义 APIENTRY，
//     所以不会为了几个 GL 1.1 函数把 <windows.h> 拖进来）；
//   · 中文/日文字体按平台候选回退，全部缺失才退回 ImGui 内置字体。

#include <cstdio>
#include <string>

#include "settings_window_impl.h"  // 提供 Impl 声明（不含任何平台头）

#include <GLFW/glfw3.h>  // 跨平台窗口库：这里主要用它提供的 GL 1.1 声明

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "shot_plugin_helpers.h"

namespace settings_ui {
namespace {

// 逻辑字号；DPI 由 style.FontScaleMain 再放大（见 applyContentScale）。
constexpr float kFontSizePixels = 16.0f;

// 中文字体按"显示效果从好到差"回退；不同平台放的位置不一样。
// TODO: POSIX（尤其 Linux）的字体路径未在真机验证，找不到就退回 ImGui 内置字体；
//       如果目标发行版路径不同，改这里的候选表即可，不影响功能。
#if defined(_WIN32)
const char* const kFontCandidates[] = {
    "C:\\Windows\\Fonts\\msyh.ttc",
    "C:\\Windows\\Fonts\\msyhbd.ttc",
    "C:\\Windows\\Fonts\\simhei.ttf",
    "C:\\Windows\\Fonts\\simsun.ttc",
};
#elif defined(__APPLE__)
const char* const kFontCandidates[] = {
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
};
#else
const char* const kFontCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
};
#endif

/**
 * @brief 从 GL_VERSION 字符串提出版本号，编码成 major*100 + minor*10。
 *
 * 例："3.3.0 NVIDIA 566.36" -> 330，"4.6.0" -> 460。解析失败返回 0。
 * @note 只服务本文件的 initImGui，与设置页模型/窗口生命周期无关，所以留在本 TU
 *       的匿名命名空间（STYLE.md §6.7）。
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

bool SettingsWindow::Impl::initImGui() {
    // GL 上下文已在 createWindow 里 make current；先确认版本，再建 ImGui。
    // 版本过低时干净失败：否则 ImGui 后端会在 NewFrame 里建 shader 失败并 IM_ASSERT，
    // 把 Debug 版宿主直接断言掉。
    const char* versionText = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const int glVersion = parseGlVersion(versionText);
    const std::string versionLabel = (versionText != nullptr) ? versionText : "unknown";
    if (glVersion < 300) {
        shot::Logger(host).error("[settings_ui] OpenGL 版本过低（" + versionLabel +
                                 "），设置窗口需要 OpenGL 3.0+");
        return false;
    }
    // GLSL 版本按实际 GL 版本选：3.2+（含 3.3 core）用 150，3.0/3.1 用 130。
    const char* glslVersion = (glVersion >= 320) ? "#version 150" : "#version 130";
    shot::Logger(host).info("[settings_ui] OpenGL " + versionLabel + "，GLSL " + glslVersion);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // 窗口几何由宿主配置持久化（settings_ui.window），不在插件目录旁边生成 imgui.ini；
    // io.LogFilename 同理置空，免得把 imgui_log.txt 写到宿主的工作目录。
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    applyContentScale();
    loadFonts();

    // 失败时把已经建到一半的 ImGui 资源在本函数内收干净：后端的 Shutdown 不能对
    // 没初始化成功的后端调用（那边是 IM_ASSERT），所以逐层回退。
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        shot::Logger(host).error("[settings_ui] ImGui GLFW 后端初始化失败");
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        shot::Logger(host).error("[settings_ui] ImGui OpenGL3 后端初始化失败");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    // 提前建一次设备对象：NewFrame 里的同名失败路径是 IM_ASSERT，Debug 宿主上会直接
    // 断言退出。先建一遍就把"驱动不支持这套 shader"变成干净的初始化失败。
    if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
        shot::Logger(host).error("[settings_ui] OpenGL 着色器初始化失败（驱动不支持？）");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    imguiInitialized = true;
    return true;
}

void SettingsWindow::Impl::loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const ImWchar* glyphRanges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
    for (const char* path : kFontCandidates) {
        // 文件不存在 / 读不了时返回 nullptr（不抛异常），正好用来做"按顺序尝试"。
        if (io.Fonts->AddFontFromFileTTF(path, kFontSizePixels, nullptr, glyphRanges) != nullptr) {
            shot::Logger(host).info("settings_ui：中文字体 " + std::string(path));
            return;
        }
    }
    // 一个系统字体都没有（极简系统）：退回内置字体。只是显示效果降级，不影响功能。
    io.Fonts->AddFontDefault();
    shot::Logger(host).warn("settings_ui：系统中文字体都不可用，退回 ImGui 内置字体（中文会显示成方块）");
}

void SettingsWindow::Impl::applyContentScale() {
    float xScale = 1.0f;
    float yScale = 1.0f;
    glfwGetWindowContentScale(window, &xScale, &yScale);
    float scale = (xScale > 0.0f) ? xScale : yScale;
    if (scale <= 0.0f) {
        // 窗口所在显示器拿不到时退回主显示器（GLFW 在窗口可见前可能给 0）。
        if (GLFWmonitor* primary = glfwGetPrimaryMonitor()) {
            glfwGetMonitorContentScale(primary, &xScale, &yScale);
            scale = (xScale > 0.0f) ? xScale : yScale;
        }
    }
    if (scale <= 0.0f) {
        scale = 1.0f;
    }
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    // ImGui 1.92 把 io.FontGlobalScale 移到了 style.FontScaleMain（旧字段 > 1 时会触发
    // 断言），语义相同：只放大字号，不重复放大间距。
    style.FontScaleMain = scale;
    // TODO: 只按窗口显示前的 content scale 缩放了这一份 style。运行中把窗口拖到另一个
    // DPI 的显示器时不会重算——需要保存初始 style 再整体重建，等有双 DPI 显示器的
    // 实测环境再补，避免现在凭猜测写一套没验证过的重建逻辑。
}

void SettingsWindow::Impl::shutdownGraphics() {
    if (imguiInitialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }
}

bool SettingsWindow::Impl::renderFrame() {
    if (window == nullptr || !imguiInitialized) {
        return false;
    }
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        return false;  // 最小化时 framebuffer 为 0：glViewport 和 NewFrame 都不该跑
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    panel.render();
    ImGui::Render();

    glViewport(0, 0, framebufferWidth, framebufferHeight);
    glClearColor(0.11f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
    return true;
}

}  // namespace settings_ui
