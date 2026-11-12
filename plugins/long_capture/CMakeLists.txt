# ============================================================================
#  long_capture —— 手动滚动长截图插件（"shot.configurable"）
#
#  用户先在 region_selector 里点选目标窗口/拖出区域（region.select.request →
#  region.selected 握手），插件再用自带的 GDI 采集层按目标区域连续抓帧，在内存里
#  按行指纹匹配做在线拼接，Enter / HUD 按钮保存成一张长 PNG 并发布**一条**
#  capture.done；Esc / HUD 按钮取消。
#
#  核心（画布 / 行指纹拼接 / PNG 编码）只用标准库 + stb，放在
#  stitch_canvas.cpp / scroll_stitcher.cpp / long_image_saver.cpp，不包含任何平台头；
#  自带采集层（capture_windows.cpp）、HUD、会话按键、托盘气泡是 Win32 实现
#  （hud_windows.cpp / global_keys_windows.cpp / tray_hint_windows.cpp），
#  非 Windows 平台直接跳过整个插件，不产出伪实现。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME long_capture)

# 本插件是 Windows 实现（GDI 采集 + GDI HUD + RegisterHotKey 会话按键）：
# 非 Windows 平台直接跳过，不留一份跑不起来的 stub。被宿主 add_subdirectory 时
# 用 PARENT_SCOPE 告诉根工程"这个插件被跳过了"（根 CMakeLists 据此打印提示）。
#
# TODO: 其它平台需要各自的采集/客户区查询、HUD 绘制与会话按键实现
#       （macOS：CGWindowListCreateImage + NSWindow + RegisterEventHotKey；
#       Linux：X11 的 XGetImage + XGrabKey，Wayland 下没有等价能力）。在补齐之前
#       非 Windows 平台一直走下面的 return() 跳过整个插件。
if(NOT WIN32)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "long_capture：仅 Windows（GDI 采集 + GDI HUD + 会话按键），当前平台跳过")
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

# stb 是 header-only：vcpkg 的 FindStb.cmake 只负责找到 Stb_INCLUDE_DIR，
# 没有导入目标、也没有要链接的库（与 image_saver / clipboard 同一写法）。
find_package(Stb REQUIRED)
# nlohmann-json：capture.done 与 config_list 的 JSON 解析、语言表解析（header-only）。
find_package(nlohmann_json CONFIG REQUIRED)

# 平台库：user32（会话热键/鼠标/HUD 窗口）、gdi32（自带采集与 HUD 绘制）、
# shell32（托盘气泡的 Shell_NotifyIconW）。
shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/long_capture.cpp
                 src/long_capture_session.cpp
                 src/long_capture_config.cpp
                 src/long_capture_i18n.cpp
                 src/long_capture_output.cpp
                 src/stitch_canvas.cpp
                 src/scroll_stitcher.cpp
                 src/long_image_saver.cpp
                 src/capture_windows.cpp
                 src/hud_windows.cpp
                 src/global_keys_windows.cpp
                 src/tray_hint_windows.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 nlohmann_json::nlohmann_json
                 user32
                 gdi32
                 shell32
)

# shot_add_plugin() 只认识 INCLUDE_DIRS / LINK_LIBRARIES 两种参数，所以 stb 的
# 头目录在目标上单独加（header-only，没有链接项）。
target_include_directories(${SHOT_PLUGIN_NAME} PRIVATE ${Stb_INCLUDE_DIR})

# 新插件要求 /W4 干净；不加 /WX，避免不同 MSVC 小版本的第三方头差异卡死构建。
if(MSVC)
    target_compile_options(${SHOT_PLUGIN_NAME} PRIVATE /W4)
endif()

