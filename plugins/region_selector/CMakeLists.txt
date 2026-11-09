# ============================================================================
#  region_selector —— 屏幕区域选择覆盖层插件（"shot.ui_provider"）
#
#  自己开线程、自己建 GLFW 无边框置顶窗口、用 OpenGL3 + ImGui 画暗色遮罩与选区；
#  宿主只调 init / deinit / post_to_plugin，从不接触窗口句柄。
#
#  触发：订阅 capture.request，负载 `{"mode":"region"}` 时在鼠标所在显示器上框选；
#  确认后先隐藏覆盖层、等遮罩从合成器消失，再发布带 region 的 capture.request
#  （坐标是物理虚拟桌面像素，与 screen_capture 的 describe 同一坐标系）。
#
#  窗口/上下文/事件循环：glfw3（与 settings_ui 同一套依赖）。
#  渲染：vcpkg 的 imgui 静态库 —— glfw-binding + opengl3-binding 已经把
#  imgui_impl_glfw.cpp / imgui_impl_opengl3.cpp 编进 imgui.lib，所以这里只需要
#  find_package(imgui) 再链接 imgui::imgui，不用把后端源码抄进本工程。
#  JSON：nlohmann-json（负载解析 + 组包，header-only）。
#
#  仅 Windows：覆盖层几何、光标位置与"物理虚拟桌面像素"依赖 Win32 坐标语义
#  （GetCursorPos / GetSystemMetrics(SM_*VIRTUALSCREEN)），非 Windows 平台直接跳过，
#  不产出未验证的伪实现（与 screen_capture 同一写法）。
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

# 本插件是 Windows 实现：非 Windows 平台直接跳过，不产出伪实现。
# 被宿主 add_subdirectory 时用 PARENT_SCOPE 告诉根工程"这个插件被跳过了"。
if(NOT WIN32)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "region_selector：仅 Windows（Win32 物理坐标 + GLFW 覆盖层），当前平台跳过")
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

# user32 用于 GetCursorPos / GetSystemMetrics（物理屏幕坐标，见 region_geometry.cpp）；
# opengl32 用于 ImGui OpenGL3 后端的 GL 1.x 入口（与 settings_ui 同一套链接）。
shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/region_window.cpp
                 src/region_overlay.cpp
                 src/region_graphics.cpp
                 src/region_geometry.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 imgui::imgui
                 nlohmann_json::nlohmann_json
                 glfw
                 opengl32
                 user32
)
