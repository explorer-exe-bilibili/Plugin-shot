# ============================================================================
#  screen_capture —— 屏幕采集插件（"shot.screen_capture" + "shot.configurable"）
#
#  两段式能力：describe 报告整个虚拟桌面的几何，capture 把 BGRA8 像素写进
#  宿主自己分配的缓冲——插件因此从不分配图像内存，也没有跨 DLL 的所有权问题。
#  另声明一个配置项 include_cursor（是否把鼠标光标合成进截图）。
#  实现全部是 Windows GDI（GetDC + BitBlt + GetDIBits + DrawIconEx），所以只在
#  Windows 上构建。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME screen_capture)

# 本插件是 Windows GDI 实现：非 Windows 平台直接跳过，不产出伪实现。
# 被宿主 add_subdirectory 时用 PARENT_SCOPE 告诉根工程"这个插件被跳过了"。
if(NOT WIN32)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "screen_capture：仅 Windows（GDI 抓屏），当前平台跳过")
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

# 只链接两个系统库；没有第三方依赖，插件不链接 spdlog（日志走宿主服务）。
#   gdi32  —— BitBlt / CreateCompatibleDC / GetDIBits / DeleteObject 等 GDI 对象
#   user32 —— GetDC / GetSystemMetrics / GetCursorInfo / GetIconInfo / DrawIconEx
shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/screen_capture.cpp
                 src/capture_settings.cpp
                 src/gdi_desktop_capture.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 gdi32
                 user32
)
