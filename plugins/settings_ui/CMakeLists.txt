# ============================================================================
#  settings_ui —— 设置窗口插件（"shot.ui_provider"）
#
#  无主窗口架构下，设置页本身就是一个插件：它自己开线程、自己建 GLFW 窗口、
#  用 OpenGL3 画 ImGui 表单；宿主只调 init / deinit / post_to_plugin，从不接触
#  窗口句柄。配置项来自宿主列表服务（shot.host.plugin_lists 的 config_list JSON，
#  写值/提交走 lists.* 事件），插件启停来自 shot.host.plugin_registry；
#  设置页不直接调用其它插件。
#
#  窗口/上下文/事件循环：glfw3（跨平台；Windows / Linux / macOS 同一套代码）。
#  渲染：vcpkg 的 imgui 静态库 —— glfw-binding + opengl3-binding 已经把
#  imgui_impl_glfw.cpp / imgui_impl_opengl3.cpp 编进 imgui.lib，所以这里只需要
#  find_package(imgui) 再链接 imgui::imgui，不用把后端源码抄进本工程。
#  PATH 的"浏览…"：nativefiledialog-extended（vcpkg 包名是 nativefiledialog-extended，
#  但它导出的 CMake 包名/target 是 nfd / nfd::nfd）。
#  config_list / lists.updated / plugins.changed 的负载都是 JSON，用 vcpkg 里现成的
#  nlohmann-json 解析（header-only）。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
#
#  @note POSIX（Linux / macOS）按同一套代码可编译，但当前开发机只有 Windows，
#        尚未在 POSIX 上验证（代码里对应位置有说明）。
# ============================================================================

set(SHOT_PLUGIN_NAME settings_ui)

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

# imgui 是静态库（vcpkg 清单里已启用 glfw-binding + opengl3-binding）；
# nlohmann-json 用于解析 config_list / lists.updated / plugins.changed 的 JSON 负载，
# header-only、不引入运行库；
# glfw3 包导出的 target 叫 glfw；nativefiledialog-extended 导出的包名是 nfd（target nfd::nfd）。
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)

# OpenGL 按平台链接：Windows 直接链系统 opengl32；Linux/macOS 走 OpenGL::GL。
if(WIN32)
    set(SHOT_SETTINGS_UI_OPENGL opengl32)
else()
    find_package(OpenGL REQUIRED)
    set(SHOT_SETTINGS_UI_OPENGL OpenGL::GL)
endif()

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/settings_window.cpp
                 src/settings_graphics.cpp
                 src/settings_geometry.cpp
                 src/settings_dialog.cpp
                 src/settings_panel.cpp
                 src/settings_model.cpp
                 src/settings_form.cpp
                 src/settings_hotkey.cpp
                 src/settings_plugin_table.cpp
                 src/settings_apply.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 imgui::imgui
                 nlohmann_json::nlohmann_json
                 glfw
                 nfd::nfd
                 ${SHOT_SETTINGS_UI_OPENGL}
)
