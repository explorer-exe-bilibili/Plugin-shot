# ============================================================================
#  region_selector —— 屏幕区域选择覆盖层插件（"shot.ui_provider"）
#
#  自己开线程、自己建 GLFW 无边框置顶窗口、用 OpenGL3 + ImGui 画暗色遮罩与选区；
#  宿主只调 init / deinit / post_to_plugin，从不接触窗口句柄。
#
#  触发：订阅 capture.request，负载 `{"mode":"region"}` 时开始框选。
#
#  ── Windows（Win32 实现，保持原样）──────────────────────────────────────
#  在鼠标所在显示器上铺覆盖层，背景是触发瞬间的 GDI 桌面快照；窗口悬停吸附 +
#  单击截窗口 + 拖动选区。确认后先隐藏覆盖层、等遮罩从合成器消失，再发布带
#  region 的 capture.request（坐标是物理虚拟桌面像素，与 screen_capture 的
#  describe 同一坐标系），由宿主流水线裁剪落盘。
#  几何/光标/窗口枚举依赖 Win32（GetCursorPos / GetSystemMetrics / EnumWindows /
#  DwmGetWindowAttribute），因此只有 Windows 编译 region_geometry.cpp /
#  region_background.cpp / window_target.cpp。
#
#  ── Linux / Wayland（方案 B）──────────────────────────────────────────
#  Wayland 不允许应用覆盖所有输出、读全局光标、枚举窗口，也不能自己抓屏，所以：
#    1. 用 xdg-desktop-portal 的 Screenshot 门户做一次整屏截图（首次弹系统授权框，
#       依赖 libportal，系统包 libportal-dev）；
#    2. 用自己的 GLFW/ImGui 窗口把这张定格图等比缩放居中显示；
#    3. 确认后直接从内存里的截图像素裁剪、写 PNG、发布 capture.done
#       {path,width,height,processors:0}——不再第二次采集、不再弹第二次授权。
#  没有窗口悬停吸附（Wayland 拿不到全局窗口列表），单击不拖 = 取消，Esc/右键取消。
#
#  ── Linux / X11 ───────────────────────────────────────────────────────
#  真正的全屏覆盖层：XGetImage 抓整块 X screen 当背景，GLFW 无边框置顶窗口铺满
#  鼠标所在显示器；悬停吸附 / 单击截窗 / 拖框 / 主题色折角 / Esc、右键取消与
#  Windows 完全一致。窗口命中用 XQueryTree 遍历 root 子窗口 + EWMH 类型过滤
#  （window_target_x11.cpp）。确认后发布带 region 的 capture.request（普通框选）
#  或 region.selected（purpose 选择）。
#  依赖系统包 libx11-dev（pkg-config 模块 x11）。**不需要 xrandr**：显示器布局交给
#  GLFW，X11 的 native 头用 GLFW_NATIVE_INCLUDE_NONE 关掉了 Xrandr 头依赖。
#  同一份 .so 同时编 Wayland 与 X11 两套实现，启动时按 WAYLAND_DISPLAY /
#  XDG_SESSION_TYPE 运行期选择（见 README「Linux（X11）」）。缺任一依赖都会在
#  configure 阶段 FATAL_ERROR，并给出 Ubuntu 包名。
#
#  窗口/上下文/事件循环：glfw3（与 settings_ui 同一套依赖）。
#  渲染：vcpkg 的 imgui 静态库 —— glfw-binding + opengl3-binding 已经把
#  imgui_impl_glfw.cpp / imgui_impl_opengl3.cpp 编进 imgui.lib，所以这里只需要
#  find_package(imgui) 再链接 imgui::imgui，不用把后端源码抄进本工程。
#  JSON：nlohmann-json（负载解析 + 组包，header-only）。
#  Linux 截图：libportal（pkg-config 模块 libportal-1.0）+ stb（解码/裁剪/编码）；
#  X11 快照/窗口枚举：libX11（XGetImage / XQueryTree / XTranslateCoordinates）。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME region_selector)

# 平台：Windows 与 Linux（Wayland / X11 两套实现）有实现；macOS 与其它平台直接跳过，
# 不产出未验证的伪实现。被宿主 add_subdirectory 时用 PARENT_SCOPE 告诉根工程"跳过"。
if(NOT WIN32 AND NOT (UNIX AND NOT APPLE))
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "region_selector：当前平台没有实现（Windows / Linux 之外跳过）")
    return()
endif()

# 独立构建时才自成一个 CMake 项目；被宿主 add_subdirectory 时不重复声明。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(${SHOT_PLUGIN_NAME} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# 宿主仓库内构建时下面的路径指向 <repo>/cmake；SDK 独立构建时把 <SDK>/cmake
# 加进 CMAKE_MODULE_PATH 即可（见上方说明）。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")

if(NOT COMMAND shot_add_plugin)
    include(ShotPlugin)
endif()

find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

# 平台源文件与系统库：共享的窗口/交互/绘制代码所有平台都用；
# Windows 额外编译 Win32 几何、GDI 快照与窗口枚举；Linux 同时编译 Wayland（门户）
# 与 X11（Xlib）两套实现，运行期按会话类型二选一。
set(_region_selector_sources
    src/plugin_entry.cpp
    src/region_window.cpp
    src/region_selection.cpp
    src/region_overlay.cpp
    src/region_overlay_draw.cpp
    src/region_graphics.cpp
)
set(_region_selector_libraries
    imgui::imgui
    nlohmann_json::nlohmann_json
    glfw
)
set(_region_selector_include_dirs)

if(WIN32)
    # user32 用于 GetCursorPos / GetSystemMetrics / EnumWindows（物理屏幕坐标与窗口
    # 枚举，见 region_geometry.cpp / window_target.cpp）；dwmapi 用于
    # DWMWA_EXTENDED_FRAME_BOUNDS 与 DWMWA_CLOAKED（去掉不可见边框、排除 cloaked 窗口）；
    # opengl32 用于 ImGui OpenGL3 后端的 GL 1.x 入口（与 settings_ui 同一套链接）。
    list(APPEND _region_selector_sources
         src/region_background.cpp
         src/region_geometry.cpp
         src/window_target.cpp
    )
    list(APPEND _region_selector_libraries opengl32 user32 dwmapi)
else()
    # Linux：Wayland 与 X11 两套平台实现都编进同一个 .so（运行期按会话类型选择）。
    # 系统依赖用 pkg-config 查找；**缺依赖在 configure 阶段就 FATAL_ERROR**，并直接
    # 给出 Ubuntu 包名——运行期静默降级会变成"插件加载了但按了没反应"，很难排查。
    find_package(PkgConfig REQUIRED)
    # 模块名在不同发行版里不一样：Ubuntu 24.04 的 libportal-dev 提供 libportal.pc，
    # 上游/其它发行版可能是 libportal-1.0.pc，所以两个都试。
    pkg_search_module(LIBPORTAL QUIET IMPORTED_TARGET libportal libportal-1.0)
    if(NOT LIBPORTAL_FOUND)
        message(FATAL_ERROR
            "region_selector 需要 libportal（pkg-config 模块 libportal 或 libportal-1.0），"
            "但系统里找不到。Ubuntu/Debian: sudo apt-get install -y libportal-dev")
    endif()
    # X11：XGetImage / XQueryTree / XTranslateCoordinates。模块名 x11 由 libx11-dev 提供。
    pkg_check_modules(SHOT_X11 QUIET IMPORTED_TARGET x11)
    # glfw3native.h（GLFW_EXPOSE_NATIVE_X11）会声明 RRCrtc/RROutput 并拉 <X11/extensions/Xrandr.h>，
    # 所以头文件检查之外还要 libxrandr-dev；不链 Xrandr 函数，只借它的类型。
    pkg_check_modules(SHOT_XRANDR QUIET IMPORTED_TARGET xrandr)
    if(NOT SHOT_X11_FOUND)
        message(FATAL_ERROR
            "region_selector 需要 X11 开发库（pkg-config 模块 x11），但系统里找不到。"
            "Ubuntu/Debian: sudo apt-get install -y libx11-dev")
    endif()
    if(NOT SHOT_XRANDR_FOUND)
        message(FATAL_ERROR
            "region_selector 的 X11 分支需要 Xrandr 头（glfw3native.h 声明 RROutput），"
            "但系统里找不到。Ubuntu/Debian: sudo apt-get install -y libxrandr-dev")
    endif()
    find_package(Stb REQUIRED)
    find_package(OpenGL REQUIRED)
    list(APPEND _region_selector_sources
         # Wayland（方案 B）：portal 截图 / 定格图映射 / 落盘
         src/region_portal.cpp
         src/region_image.cpp
         src/region_output_linux.cpp
         src/region_geometry_wayland.cpp
         src/region_background_wayland.cpp
         src/region_overlay_wayland.cpp
         src/window_target_wayland.cpp
         # X11：X screen 几何 / XGetImage 快照 / XQueryTree 枚举 / 覆盖层提交
         src/region_geometry_x11.cpp
         src/region_background_x11.cpp
         src/region_overlay_x11.cpp
         src/window_target_x11.cpp
    )
    list(APPEND _region_selector_libraries PkgConfig::LIBPORTAL PkgConfig::SHOT_X11
         PkgConfig::SHOT_XRANDR OpenGL::GL)
    # stb 是 header-only：vcpkg 的 FindStb.cmake 只给包含目录，没有导入目标。
    list(APPEND _region_selector_include_dirs ${Stb_INCLUDE_DIR})
endif()

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      ${_region_selector_sources}
    INCLUDE_DIRS include ${_region_selector_include_dirs}
    LINK_LIBRARIES
                 ${_region_selector_libraries}
)
