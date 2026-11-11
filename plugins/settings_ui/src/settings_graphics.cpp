// settings_graphics —— SettingsWindow 的图形层：ImGui 后端、字体与 DPI、每帧渲染
// （glViewport + ImGui NewFrame/Render/RenderDrawData + glfwSwapBuffers）。
//
// GLFW 窗口与 GL 上下文在 settings_window.cpp 创建；这些函数只在插件的 UI 线程上执行。
// DPI：启动时按窗口 content scale 放大一次 ImGui 样式与字号；运行中窗口在
// 150%/100% 两块显示器之间拖动时，content scale 回调置脏标记，下一帧从保存的
// 默认样式基线重新放大（见 applyContentScale / refreshDpiScale）。
// 跨平台要点：
//   · <GLFW/glfw3.h> 在非 Apple 平台会带 <GL/gl.h>（Windows 上 GLFW 自己定义 APIENTRY，
//     所以不会为了几个 GL 1.1 函数把 <windows.h> 拖进来）；
//   · 字体按平台候选回退：中文字体缺字形时再用 MergeMode 合并一套西文字体（见 loadFonts）。

#include <cmath>
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

// content scale 变化小于这个阈值就当作没变：GLFW 的 content scale 是 float，
// 同一显示器上抖动（例如 DPI 切换动画中间值）不应该触发重建。
constexpr float kContentScaleEpsilon = 0.01f;

/**
 * @brief 把 content scale 格式化成两位小数的日志文本（`1.50` / `1.00`）。
 * @note 只服务 refreshDpiScale 的日志，与任何类的状态无关，所以放匿名命名空间。
 */
std::string contentScaleText(float scale) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(scale));
    return text;
}

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

// ---------------------------------------------------------------------------
//  字形覆盖：为什么光有"中文字体"不够
//
//  GetGlyphRangesChineseSimplifiedCommon() 只含 Latin-1 + CJK + 日文假名 + 全角标点，
//  而 document/I18N.md 注册的 27 个语种里还有 Cyrillic（ru-RU / uk-UA / bg-BG）、
//  Greek（el-GR）、Latin Extended-A（pl-PL / cs-CZ / sk-SK / tr-TR / hu-HU / hr-HR /
//  ro-RO），这些字母都不在中文字体区间里。区间必须显式补齐：本插件的 OpenGL3 后端
//  支持动态字体图集（ImGuiBackendFlags_RendererHasTextures），1.92 于是按需烘焙字形、
//  根本不读这份区间；但同一份代码也可能跑在不支持动态图集的后端/版本上，那时
//  ImFontAtlasBuildLegacyPreloadAllGlyphRanges 只按这份区间预烘焙，缺的语种就渲染成
//  '?'。可显示字符不能押在某个后端的实现细节上，所以这里把区间写全。
// ---------------------------------------------------------------------------

// 追加到中文常用字区间上的其它区间（2 个 ImWchar 一组，0 结尾）。
// 只放"界面文案实际会用到"的：整个 Latin Extended-B 有 208 个码位，注册语种只用到
// 罗马尼亚语的 Ș ș Ț ț（U+0218-U+021B），所以只加这 4 个，不为好看把区间撑大。
constexpr ImWchar kExtraGlyphRanges[] = {
    0x0020, 0x00FF,  // Latin-1 Supplement：西欧语种的重音字母与 « » 等标点
    0x0100, 0x017F,  // Latin Extended-A：ą č ę ł ő ş đ …
    0x0218, 0x021B,  // Latin Extended-B：Ș ș Ț ț（ro-RO）
    0x0370, 0x03FF,  // Greek and Coptic：el-GR
    0x0400, 0x052F,  // Cyrillic + Cyrillic Supplement：ru-RU / uk-UA / bg-BG
    0x2000, 0x206F,  // 常用标点：… — – • « » 等（宿主与插件的语种文案里已在用）
    0x3000, 0x30FF,  // CJK 标点 + 平假名/片假名：zh-CN / zh-TW / ja-JP
    0xFF00, 0xFFEF,  // 全角字母、全角标点（中文界面里的（）「」等）
    0,               // 区间表以 0 结尾（ImGui 的约定）
};

// 补字字体：主字体缺字形时用 ImFontConfig::MergeMode 合进来。选 Segoe UI 是因为它随
//  Windows 自带（不用装）、且 Latin Extended-A/B、Greek、Cyrillic 全部覆盖；而中文候选
//  里的 simhei/simsun 恰恰大面积缺 Latin Extended-A 与 Greek（实测，见 loadFonts 日志），
//  换一台只有 simhei 的机器时没有它，这些语种就是问号/方块。
// TODO: POSIX 的补字字体路径未在真机验证，找不到就跳过合并（只是少补几个字形）。
#if defined(_WIN32)
const char* const kSupplementFontCandidates[] = {
    "C:\\Windows\\Fonts\\segoeui.ttf",
    "C:\\Windows\\Fonts\\arial.ttf",
};
#elif defined(__APPLE__)
const char* const kSupplementFontCandidates[] = {
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
};
#else
const char* const kSupplementFontCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
};
#endif

/**
 * @brief 统计零结尾区间表覆盖的码位总数。
 * @param[in] ranges 零结尾的 Unicode 区间表（2 个 ImWchar 一组）。
 * @return 区间内码位总数。
 * @note 只服务 loadFonts 的启动日志（"到底要求了多大的字符集"），与类状态无关。
 */
int countRangeCodepoints(const ImWchar* ranges) {
    int total = 0;
    for (const ImWchar* range = ranges; range[0] != 0; range += 2) {
        total += static_cast<int>(range[1]) - static_cast<int>(range[0]) + 1;
    }
    return total;
}

/**
 * @brief 统计字体缺多少个"区间内应有的字形"。
 *
 * 用途是把"主字体缺多少、合并补字字体后还剩多少"写进启动日志：I18N.md 声称的语种
 * 覆盖要能自证，否则以后又只能靠猜。这里读的就是 ImGui 预烘焙时用的同一份区间表。
 *
 * @param[in] font   已加入图集的字体（AddFontFromFileTTF 返回非空之后才可调用）。
 * @param[in] ranges 零结尾的 Unicode 区间表。
 * @return 缺字形的码位数；font 为空时返回 0。
 * @note ImFont::IsGlyphInFont 不是 const 成员（要遍历内部 source），所以参数不是 const。
 */
int countMissingGlyphs(ImFont* font, const ImWchar* ranges) {
    if (font == nullptr) {
        return 0;
    }
    int missing = 0;
    for (const ImWchar* range = ranges; range[0] != 0; range += 2) {
        for (unsigned int codepoint = range[0]; codepoint <= range[1]; ++codepoint) {
            if (!font->IsGlyphInFont(static_cast<ImWchar>(codepoint))) {
                ++missing;
            }
        }
    }
    return missing;
}

/**
 * @brief 组装设置页字体的完整字形区间：中文常用字 + kExtraGlyphRanges。
 *
 * ImFontConfig::GlyphRanges 只保存指针、不拷贝内容，所以区间表必须活到字体图集销毁；
 * 这里用函数内 static（调用只发生在 UI 线程的 initImGui 里，C++11 起的 static 初始化
 * 本身是线程安全的）。重复调用返回同一份存储。
 *
 * @return 零结尾的区间表。
 */
const ImWchar* buildGlyphRanges() {
    static ImVector<ImWchar> ranges;
    if (!ranges.empty()) {
        return ranges.Data;
    }
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon());
    builder.AddRanges(kExtraGlyphRanges);
    builder.BuildRanges(&ranges);
    return ranges.Data;
}

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

    // 先记下未缩放的默认样式：applyContentScale 是乘法缩放，运行中跨屏换 DPI 时
    // 必须从这份基线重新放大，否则 spacing/圆角/边框会按次数累积。
    baseStyle = ImGui::GetStyle();
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
    // 区间表是函数内 static，指针在字体图集销毁前一直有效（ImFontConfig 只存指针）。
    const ImWchar* glyphRanges = buildGlyphRanges();

    // 候选表里的路径不一定存在（精简系统 / 别的平台）：NoLoadError 让 AddFont 返回
    // nullptr 而不是在 Debug 版里断言——文件缺失是正常回退路径，不是错误。
    ImFontConfig primaryConfig;
    primaryConfig.Flags |= ImFontFlags_NoLoadError;
    ImFont* primary = nullptr;
    std::string primaryPath;
    for (const char* path : kFontCandidates) {
        // 文件不存在 / 读不了时返回 nullptr（不抛异常），正好用来做"按顺序尝试"。
        primary = io.Fonts->AddFontFromFileTTF(path, kFontSizePixels, &primaryConfig, glyphRanges);
        if (primary != nullptr) {
            primaryPath = path;
            break;
        }
    }
    if (primary == nullptr) {
        // 一个系统字体都没有（极简系统）：退回内置字体。只是显示效果降级，不影响功能。
        io.Fonts->AddFontDefault();
        shot::Logger(host).warn(
            "settings_ui：系统中文字体都不可用，退回 ImGui 内置字体（中文会显示成方块）");
        return;
    }

    // 主字体缺字形才合并补字字体：ImGui 按 source 顺序找字形，主字体有的优先，
    // 所以中英文的观感与合并前完全一致，只有主字体没有的字形才落到补字字体上。
    // 实测 msyh.ttc 在扩展区间缺 U+021B（ț）等少量字形；simhei/simsun 缺得多得多。
    const int missingBefore = countMissingGlyphs(primary, glyphRanges);
    std::string supplementPath;
    if (missingBefore > 0) {
        for (const char* path : kSupplementFontCandidates) {
            ImFontConfig mergeConfig;
            mergeConfig.MergeMode = true;  // 合进上一个字体，而不是新建一个 ImFont
            mergeConfig.Flags |= ImFontFlags_NoLoadError;
            if (io.Fonts->AddFontFromFileTTF(path, kFontSizePixels, &mergeConfig, glyphRanges) !=
                nullptr) {
                supplementPath = path;
                break;
            }
        }
    }
    const int missingAfter = countMissingGlyphs(primary, glyphRanges);

    // 图集大小：1.92 的字体图集按需烘焙，后端支持 RendererHasTextures 时还会自动扩容
    // （见 applyContentScale 的说明），所以不需要手工设 TexDesiredWidth；真正光栅化的
    // 只有界面上出现过的字形，合并一套西文字体不会把图集撑爆。
    std::string fontList = "settings_ui：中文字体 " + primaryPath;
    if (!supplementPath.empty()) {
        fontList += " + 补字字体 " + supplementPath;
    }
    shot::Logger(host).info(fontList);

    // 覆盖统计只用于自证（document/I18N.md §3.1 声称的语种覆盖）：主字体缺多少、
    // 合并后还剩多少。剩下的通常是 C1 控制符、未分配码位与 ‣ ⁂ 这类生僻排版符号。
    std::string coverage = "settings_ui：字形覆盖：区间 " +
                           std::to_string(countRangeCodepoints(glyphRanges)) + " 个码位，主字体缺 " +
                           std::to_string(missingBefore) + " 个字形";
    if (!supplementPath.empty()) {
        if (missingAfter == 0) {
            coverage += "，合并后无缺字";
        } else {
            coverage += "，合并后仍缺 " + std::to_string(missingAfter) +
                        " 个（控制符/未分配码位/生僻标点）";
        }
    } else if (missingBefore > 0) {
        coverage += "，没有可用的补字字体（这些字形会显示成 ?）";
    }
    shot::Logger(host).info(coverage);
}

/**
 * @brief 读窗口当前所在显示器的 content scale；拿不到时退回主显示器，再不行按 1.0。
 *
 * GLFW 在窗口进入不同 DPI 的显示器时会更新这个值（Windows 上由 WM_DPICHANGED 驱动），
 * 所以它同时是"初始缩放"和"运行中是否需要重缩放"的判据。
 *
 * @return 缩放因子（正常是 1.0 / 1.25 / 1.5 / 2.0 这类值）；永远 > 0。
 */
float SettingsWindow::Impl::queryContentScale() const {
    float xScale = 1.0f;
    float yScale = 1.0f;
    if (window == nullptr) {
        return 1.0f;
    }
    glfwGetWindowContentScale(window, &xScale, &yScale);
    float scale = (xScale > 0.0f) ? xScale : yScale;
    if (scale <= 0.0f) {
        // 窗口所在显示器还拿不到（隐藏窗口刚创建时可能出现）：退回主显示器。
        if (GLFWmonitor* primary = glfwGetPrimaryMonitor()) {
            glfwGetMonitorContentScale(primary, &xScale, &yScale);
            scale = (xScale > 0.0f) ? xScale : yScale;
        }
    }
    return (scale > 0.0f) ? scale : 1.0f;
}

void SettingsWindow::Impl::applyContentScale() {
    const float scale = queryContentScale();
    ImGuiStyle& style = ImGui::GetStyle();
    // 从"未缩放的默认样式"重建，再按当前 DPI 放大一次。1.92 的 ScaleAllSizes 是
    // 乘法（间距/圆角/边框都乘），在已缩放的 style 上再调会累积；所以必须先还原基线。
    style = baseStyle;
    style.ScaleAllSizes(scale);
    // 1.92 把 io.FontGlobalScale 移到了 style.FontScaleMain（旧字段 > 1 时会触发断言），
    // 语义相同：只放大字号，不重复放大间距。字体按需动态烘焙，改完后下一次 NewFrame
    // 会请求新字号并重建字形纹理（后端实现了 RendererHasTextures），不需要手动重建图集。
    style.FontScaleMain = scale;
    contentScale = scale;
}

void SettingsWindow::Impl::refreshDpiScale() {
    dpiScaleDirty = false;
    if (window == nullptr || !imguiInitialized) {
        return;
    }
    const float scale = queryContentScale();
    if (std::fabs(scale - contentScale) < kContentScaleEpsilon) {
        return;
    }
    const float previous = contentScale;
    applyContentScale();
    shot::Logger(host).info("settings_ui：DPI 缩放 " + contentScaleText(previous) + " → " +
                            contentScaleText(contentScale));
}

void SettingsWindow::Impl::onContentScale(GLFWwindow* glfwWindow, float /*xScale*/,
                                          float /*yScale*/) {
    auto* self = static_cast<Impl*>(glfwGetWindowUserPointer(glfwWindow));
    if (self == nullptr) {
        return;
    }
    // 回调可能在 glfwSetWindowPos 内部同步触发（真机实测），也可能在用户拖动窗口时
    // 由 glfwWaitEvents 分发；这里只置标记，样式重建统一放到下一帧/下次显示的开头，
    // 避免在 GLFW 回调里改 ImGui 状态。
    self->dpiScaleDirty = true;
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
    // 上一帧之后窗口可能被拖到另一块 DPI 不同的显示器：在 ImGui::NewFrame 之前
    // 重建样式（不能在帧中间改）。
    if (dpiScaleDirty) {
        refreshDpiScale();
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
