# ============================================================================
#  tray —— Windows 托盘图标插件（"shot.ui_provider"）
#
#  无主窗口架构下，托盘是"没有任何窗口时也能触发动作"的入口：左键执行默认命令，
#  右键菜单完全由宿主列表服务的 tray_command_list 数据驱动（core 预置三项 + 插件
#  lists.add 贡献的命令）。它自己开线程、自己建隐藏的消息窗口、自己调
#  Shell_NotifyIconW；宿主只调 init / deinit / post_to_plugin，既不接触 HWND，
#  也不知道插件有几个线程。所有跨插件通信只走事件总线（点击条目发布条目声明的
#  event，气泡订阅 capture.done / capture.failed），托盘不直接调用其它插件。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME tray)

# 本插件是 Win32 托盘实现（RegisterClassExW + Shell_NotifyIconW）：非 Windows 平台
# 直接跳过，而不是留一份跑不起来的 stub。被宿主 add_subdirectory 时用 PARENT_SCOPE
# 告诉根工程"这个插件被跳过了"（根 CMakeLists 据此打印提示）。
#  TODO: macOS 用 NSStatusBar、Linux 用 StatusNotifier/AppIndicator 实现托盘；
#  在补齐那两套实现之前，非 Windows 平台一直走下面的 return() 跳过整个插件。
if(NOT WIN32)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "tray：仅 Windows（Win32 托盘），当前平台跳过")
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

# nlohmann-json 用来解析 tray_command_list（菜单数据）与 capture.done /
# capture.failed 的 JSON 负载（header-only，根 vcpkg 清单里已有，不会给插件目录
# 带进额外运行库）。
find_package(nlohmann_json CONFIG REQUIRED)

# user32：窗口类/消息/菜单；shell32：Shell_NotifyIconW。
shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/tray_config.cpp
                 src/tray_icon.cpp
                 src/tray_icon_window.cpp
                 src/tray_icon_menu.cpp
                 src/tray_icon_notify.cpp
                 src/tray_menu.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 nlohmann_json::nlohmann_json
                 user32
                 shell32
)
