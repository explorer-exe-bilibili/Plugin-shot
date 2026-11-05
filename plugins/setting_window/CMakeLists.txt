# ============================================================================
#  setting_window —— 插件自带窗口的示例（"shot.ui_provider"）
#
#  窗口归插件自己：本插件开自己的线程、跑自己的 Win32 消息循环，
#  宿主只调 init / deinit / post_to_plugin，从不接触 HWND。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 add_subdirectory(plugins/example_processor) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME setting_window)

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

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/setting_window.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES user32
)
