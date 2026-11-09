# ============================================================================
#  notify —— 截屏结果消息提示插件（"shot.configurable"）
#
#  插件不采集、不保存、不弹自己的窗口：它订阅 capture.done / capture.failed，
#  用 Shell_NotifyIconW 的 NIF_INFO 气泡（Windows 10/11 上表现为系统通知）告诉
#  用户"已保存到哪"或"截图为什么失败"。配置项（enabled / show_on_success /
#  show_on_error / sound）由宿主渲染并回灌，插件只负责 get/set。
#
#  为什么需要一个隐藏的占位托盘图标：实测（Windows 11 26H2）不做 NIM_ADD 直接
#  NIM_MODIFY(NIF_INFO) 会返回失败（E_FAIL），系统不会显示任何通知；而 NIM_ADD
#  带 NIF_STATE + NIS_HIDDEN 的图标在通知区域不可见，却仍能挂 NIF_INFO 气泡。
#  所以本插件在通知区域加一个隐藏图标，气泡都发在它上面。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME notify)

# 消息气泡是 Win32 实现（Shell_NotifyIconW + 隐藏消息窗口）：非 Windows 平台直接
# 跳过整个插件，而不是留一份跑不起来的 stub。被宿主 add_subdirectory 时用
# PARENT_SCOPE 告诉根工程"这个插件被跳过了"（根 CMakeLists 据此打印提示）。
#  TODO: macOS 用 UserNotifications 框架（UNUserNotificationCenter）发本地通知；
#        Linux 走 libnotify / 桌面 portal 的 org.freedesktop.Notifications DBus 接口。
#        两者都不需要托盘占位图标，在补齐之前非 Windows 平台一直走下面的 return()。
if(NOT WIN32)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "notify：仅 Windows（Shell_NotifyIconW 通知气泡），当前平台跳过")
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

# nlohmann-json 解析 capture.done / capture.failed 的 JSON 负载（header-only，根
# vcpkg 清单里已有，不会给插件目录带进额外运行库）。
find_package(nlohmann_json CONFIG REQUIRED)

# user32：隐藏消息窗口与 Shell 通知；shell32：Shell_NotifyIconW。
shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/notify_config.cpp
                 src/notify_toast.cpp
                 src/notify_toast_window.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 nlohmann_json::nlohmann_json
                 user32
                 shell32
)

# 新插件要求 /W4 干净；不加 /WX，避免不同 MSVC 小版本的第三方头差异卡死构建。
if(MSVC)
    target_compile_options(${SHOT_PLUGIN_NAME} PRIVATE /W4)
endif()
