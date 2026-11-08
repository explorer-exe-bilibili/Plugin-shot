// settings_graphics —— SettingsWindow 的图形层：WGL 上下文、ImGui 后端、字体与 DPI、
// 每帧渲染（glViewport + ImGui NewFrame/Render/RenderDrawData/SwapBuffers）。
//
// 这些函数只在插件自己的 UI 线程上执行；窗口/线程/消息循环在 settings_window.cpp，
// 窗口几何在 settings_geometry.cpp。

#include <cstdio>
#include <string>

#include "settings_window_impl.h"  // 先于 <GL/gl.h>：它提供 <windows.h>

#include <GL/gl.h>  // 必须在 <windows.h> 之后

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "shot_plugin_helpers.h"

namespace settings_ui {
namespace {

// 这些 WGL 函数只能通过 wglGetProcAddress 拿到，opengl32.lib 不导出。
using PFNWGLCreateContextAttribsARB = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using PFNWGLSwapIntervalEXT = BOOL(WINAPI*)(int);

// WGL_ARB_create_context 的属性号：正规做法是包含 glext.h，但本插件只用 4 个常量，
// 不想为一个常量把整套扩展头拖进来，按扩展规范手写数值。
constexpr int kWglContextMajorVersionArb = 0x2091;
constexpr int kWglContextMinorVersionArb = 0x2092;
constexpr int kWglContextProfileMaskArb = 0x9126;
constexpr int kWglContextCoreProfileBitArb = 0x00000001;

// 中文字体按"显示效果从好到差"回退：微软雅黑 -> 雅黑粗体 -> 黑体 -> 宋体 -> 西文。
// 全失败才退回 ImGui 内置字体：中文会显示成方块，但功能不受影响（SETTINGS.md §6）。
constexpr float kFontSizePixels = 16.0f;  // 逻辑字号；DPI 用 style.FontScaleMain 再放大
const char* const kFontCandidates[] = {
    "C:\\Windows\\Fonts\\msyh.ttc",
    "C:\\Windows\\Fonts\\msyhbd.ttc",
    "C:\\Windows\\Fonts\\simhei.ttf",
    "C:\\Windows\\Fonts\\simsun.ttc",
    "C:\\Windows\\Fonts\\segoeui.ttf",
};

/**
 * @brief 从 GL_VERSION 字符串提出版本号，编码成 major*100 + minor*10。
 *
 * 例："3.3.0 NVIDIA 566.36" -> 330，"4.6.0" -> 460。解析失败返回 0。
 * @note 只服务本文件的 initOpenGl，与设置页模型/窗口生命周期无关，所以留在本 TU
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

bool SettingsWindow::Impl::initOpenGl(HWND wnd) {
    windowDc = GetDC(wnd);
    if (windowDc == nullptr) {
        shot::Logger(host).error("[settings_ui] 取窗口 DC 失败");
        return false;
    }

    PIXELFORMATDESCRIPTOR pixelFormat{};
    pixelFormat.nSize = sizeof(pixelFormat);
    pixelFormat.nVersion = 1;
    pixelFormat.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pixelFormat.iPixelType = PFD_TYPE_RGBA;
    pixelFormat.cColorBits = 32;
    pixelFormat.cDepthBits = 24;
    pixelFormat.cStencilBits = 8;
    pixelFormat.iLayerType = PFD_MAIN_PLANE;
    const int chosenFormat = ChoosePixelFormat(windowDc, &pixelFormat);
    if (chosenFormat == 0 || SetPixelFormat(windowDc, chosenFormat, &pixelFormat) == FALSE) {
        shot::Logger(host).error("[settings_ui] 选择/设置 OpenGL 像素格式失败");
        return false;
    }

    // 先建一个基础上下文，才能用 wglGetProcAddress 拿到 wglCreateContextAttribsARB；
    // 老驱动没有 WGL_ARB_create_context 时就一直用这个兼容上下文（不算失败）。
    HGLRC legacyContext = wglCreateContext(windowDc);
    if (legacyContext == nullptr || wglMakeCurrent(windowDc, legacyContext) == FALSE) {
        if (legacyContext != nullptr) {
            wglDeleteContext(legacyContext);
        }
        shot::Logger(host).error("[settings_ui] 创建基础 OpenGL 上下文失败");
        return false;
    }
    glContext = legacyContext;
    auto createContextAttribs = reinterpret_cast<PFNWGLCreateContextAttribsARB>(
        wglGetProcAddress("wglCreateContextAttribsARB"));
    if (createContextAttribs != nullptr) {
        // 优先 3.3 core：ImGui 1.92 的 OpenGL3 后端在 core profile 下最省心。
        const int attribs[] = {
            kWglContextMajorVersionArb, 3,
            kWglContextMinorVersionArb, 3,
            kWglContextProfileMaskArb,  kWglContextCoreProfileBitArb,
            0,
        };
        if (HGLRC coreContext = createContextAttribs(windowDc, nullptr, attribs)) {
            if (wglMakeCurrent(windowDc, coreContext) != FALSE) {
                wglDeleteContext(legacyContext);
                glContext = coreContext;
            } else {
                // 切不过去（极老驱动）：继续用兼容上下文，别把窗口判死。
                wglDeleteContext(coreContext);
                wglMakeCurrent(windowDc, legacyContext);
            }
        }
    }

    // 有 WGL_EXT_swap_control 就开垂直同步：可见时消息循环持续重绘，没有 vsync
    // 会全速空转占满一个核。
    auto swapInterval =
        reinterpret_cast<PFNWGLSwapIntervalEXT>(wglGetProcAddress("wglSwapIntervalEXT"));
    if (swapInterval != nullptr) {
        vsyncEnabled = (swapInterval(1) != FALSE);
    }

    // GLSL 版本按实际 GL 版本选：3.2+（含 3.3 core）用 150，3.0/3.1 用 130。
    // ImGui 后端在 NewFrame 里建 shader 失败会 IM_ASSERT，所以低于 3.0 直接判失败，
    // 让 init 干净地返回失败，而不是等第一帧把 Debug 版宿主断言掉。
    const char* versionText = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const int glVersion = parseGlVersion(versionText);
    const std::string versionLabel = (versionText != nullptr) ? versionText : "unknown";
    if (glVersion < 300) {
        shot::Logger(host).error("[settings_ui] OpenGL 版本过低（" + versionLabel +
                                 "），设置窗口需要 OpenGL 3.0+");
        return false;
    }
    glslVersion = (glVersion >= 320) ? "#version 150" : "#version 130";
    shot::Logger(host).info("[settings_ui] OpenGL " + versionLabel + "，GLSL " + glslVersion);
    return true;
}

bool SettingsWindow::Impl::initImGui(HWND wnd) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // 窗口几何由宿主配置持久化（settings_ui.window），不在插件目录旁边生成 imgui.ini；
    // io.LogFilename 同理置空，免得把 imgui_log.txt 写到宿主的工作目录。
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    applyDpiScale(wnd);
    loadFonts();

    // 失败时把已经建到一半的 ImGui 资源在本函数内收干净：后端的 Shutdown 不能对
    // 没初始化成功的后端调用（那边是 IM_ASSERT），所以逐层回退。
    if (!ImGui_ImplWin32_Init(wnd)) {
        shot::Logger(host).error("[settings_ui] ImGui Win32 后端初始化失败");
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        shot::Logger(host).error("[settings_ui] ImGui OpenGL3 后端初始化失败");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    // 提前建一次设备对象：NewFrame 里的同名失败路径是 IM_ASSERT，Debug 宿主上会直接
    // 断言退出。先建一遍就把"驱动不支持这套 shader"变成干净的初始化失败。
    if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
        shot::Logger(host).error("[settings_ui] OpenGL 着色器初始化失败（驱动不支持？）");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
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

void SettingsWindow::Impl::applyDpiScale(HWND wnd) {
    const UINT dpi = GetDpiForWindow(wnd);  // 拿不到时返回 0（老系统）
    dpiScale = (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);
    // ImGui 1.92 把 io.FontGlobalScale 移到了 style.FontScaleMain（旧字段 > 1 时会触发
    // 断言），语义相同：只放大字号，不重复放大间距。
    style.FontScaleMain = dpiScale;
    // TODO: 只按窗口创建时的 DPI 缩放了这一份 style。窗口被拖到另一个 DPI 的显示器
    // （WM_DPICHANGED）时不会重算——需要保存初始 style 再整体重建，等有双 DPI 显示器
    // 的实测环境再补，避免现在凭猜测写一套没验证过的重建逻辑。
}

void SettingsWindow::Impl::shutdownGraphics() {
    if (imguiInitialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }
    if (glContext != nullptr) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(glContext);
        glContext = nullptr;
    }
    if (windowDc != nullptr) {
        HWND wnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            wnd = hwnd;
        }
        if (wnd != nullptr) {
            ReleaseDC(wnd, windowDc);
        }
        windowDc = nullptr;
    }
}

bool SettingsWindow::Impl::renderFrame() {
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        wnd = hwnd;
    }
    if (wnd == nullptr || windowDc == nullptr || !imguiInitialized) {
        return false;
    }
    RECT clientRect{};
    if (GetClientRect(wnd, &clientRect) == FALSE) {
        return false;
    }
    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0) {
        return false;  // 最小化时客户区为 0：glViewport 和 NewFrame 都不该跑
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    panel.render();
    ImGui::Render();

    glViewport(0, 0, width, height);
    glClearColor(0.11f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SwapBuffers(windowDc);
    return true;
}

}  // namespace settings_ui
