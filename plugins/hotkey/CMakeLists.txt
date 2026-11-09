# ============================================================================
#  hotkey —— Windows 全局热键插件（"shot.ui_provider"）
#
#  无主窗口架构下，"按一个键就能触发动作"的入口：插件自己开线程、自己建隐藏
#  消息窗口，用 RegisterHotKey 注册全局热键，收到 WM_HOTKEY 后只往事件总线发
#  一条事件（截屏 / 区域截屏 / 打开设置），不直接调用任何其它插件。宿主只调
#  init / deinit / post_to_plugin，既不知道 HWND，也不知道插件有几个线程。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME hotkey)

# 本插件是 Win32 全局热键实现（RegisterHotKey + 隐藏消息窗口）：非 Windows 平台
# 直接跳过，而不是留一份跑不起来的 stub。被宿主 add_subdirectory 时用
# PARENT_SCOPE 告诉根工程"这个插件被跳过了"（根 CMakeLists 据此打印提示）。
#
#  TODO: macOS 用 RegisterEventHotKey（Carbon/HIToolbox）注册全局热键，
#        Linux 用 X11 的 XGrabKey（或 Wayland 下交给桌面环境的 portal）实现；
#        两者都需要各自的事件循环适配，在补齐之前非 Windows 平台一直走下面的
#        return() 跳过整个插件。
if(NOT WIN32)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "hotkey：仅 Windows（RegisterHotKey 全局热键），当前平台跳过")
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

# user32：RegisterHotKey / UnregisterHotKey、隐藏消息窗口与消息循环。
# 插件不链接 nlohmann-json / spdlog：热键事件负载是两个固定 JSON 字面量，
# 日志走宿主的 log 服务（见 STYLE.md §6.4）。
shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/hotkey_binding.cpp
                 src/hotkey_config.cpp
                 src/hotkey_window.cpp
                 src/hotkey_window_message.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 user32
)
