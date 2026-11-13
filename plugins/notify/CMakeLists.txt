# ============================================================================
#  notify —— 截屏结果消息提示插件（"shot.configurable"）
#
#  插件不采集、不保存、不弹自己的窗口：它订阅 capture.done / capture.failed，
#  按平台显示系统通知：
#    · Windows：Shell_NotifyIconW 的 NIF_INFO 气泡（Windows 10/11 上表现为系统通知）；
#    · Linux：libnotify（org.freedesktop.Notifications 的 D-Bus 客户端）。
#  配置项（enabled / show_on_success / show_on_error / sound / title / click_action）
#  由宿主渲染并回灌，插件只负责 get/set。Linux 的 sound 只能作为 freedesktop 的 hint
#  建议，不保证有声；点击通知打开图片/文件夹在 Windows 用 ShellExecuteW，Linux 用
#  libnotify action + xdg-open。
#
#  为什么 Windows 需要一个隐藏的占位托盘图标：实测（Windows 11 26H2）不做 NIM_ADD
#  直接 NIM_MODIFY(NIF_INFO) 会返回失败（E_FAIL），系统不会显示任何通知；而
#  NIM_ADD 带 NIF_STATE + NIS_HIDDEN 的图标在通知区域不可见，却仍能挂 NIF_INFO
#  气泡。所以本插件在通知区域加一个隐藏图标，气泡都发在它上面。Linux 不需要
#  窗口或图标，通知由桌面通知守护进程显示。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
#    Linux 独立构建时系统需要 libnotify-dev 与 pkg-config。
# ============================================================================

set(SHOT_PLUGIN_NAME notify)

# macOS 用 UserNotifications 框架（UNUserNotificationCenter）发本地通知，尚未补齐；
# 在补齐之前跳过整个插件，而不是留一份跑不起来的 stub。被宿主 add_subdirectory
# 时用 PARENT_SCOPE 告诉根工程"这个插件被跳过了"（根 CMakeLists 据此打印提示）。
#  TODO: macOS 用 UNUserNotificationCenter 实现，并在 CMake 里加 APPLE 源。
if(APPLE)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "notify：macOS（UserNotifications）待补，当前平台跳过")
    return()
endif()

# 本插件只有 Windows Shell 与 Linux libnotify 两个后端；其它 Unix（如 FreeBSD）
# 没有验证过桌面通知依赖，同样明确跳过，避免编出一个没有 notify_toast 实现的 .so。
if(NOT WIN32 AND NOT UNIX)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "notify：仅 Windows / Linux（系统通知），当前平台跳过")
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

# 平台相关源文件与系统库：Windows 的隐藏窗口/托盘实现保持原样；Linux 走 libnotify。
set(_notify_sources
    src/plugin_entry.cpp
    src/notify_config.cpp
    src/notify_title.cpp
)
set(_notify_libraries nlohmann_json::nlohmann_json)
set(_notify_include_dirs)

if(WIN32)
    # user32：隐藏消息窗口与 Shell 通知；shell32：Shell_NotifyIconW + ShellExecuteW。
    # 气泡文本与发送、通知点击行为各拆一个 .cpp（STYLE §6.8：一个文件一个关注点）。
    list(APPEND _notify_sources
         src/notify_toast.cpp
         src/notify_toast_window.cpp
         src/notify_toast_balloon.cpp
         src/notify_toast_click.cpp)
    list(APPEND _notify_libraries user32 shell32)
elseif(UNIX AND NOT APPLE)
    # libnotify 只提供 pkg-config 描述（没有 CMake config 包）；用系统包而不是 vcpkg，
    # 保证链接的是当前桌面会话使用的同一份 glib / libnotify。
    # 缺依赖时 pkg_check_modules(REQUIRED) 在 configure 阶段直接 FATAL_ERROR；
    # Ubuntu 24.04 安装包：sudo apt install libnotify-dev（会带 libglib2.0-dev）。
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBNOTIFY REQUIRED libnotify)
    list(APPEND _notify_sources src/notify_toast_linux.cpp)
    # 用 LINK_LIBRARIES（绝对路径）而不是 LIBRARIES：pkg-config 的 LIBRARIES 里是
    # "-lnotify" 对应的裸名字 "notify"，而本插件的 CMake target 也叫 notify，
    # 直接放进 target_link_libraries 会被当成"链接到自己"（CMake 报
    # "Target notify links to itself"）。
    list(APPEND _notify_libraries ${LIBNOTIFY_LINK_LIBRARIES})
    list(APPEND _notify_include_dirs ${LIBNOTIFY_INCLUDE_DIRS})
endif()

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      ${_notify_sources}
    INCLUDE_DIRS include ${_notify_include_dirs}
    LINK_LIBRARIES
                 ${_notify_libraries}
)

if(UNIX AND NOT APPLE)
    # pkg-config 的 -I / -L / 额外编译参数（glib 的 include 路径、线程宏等）不在
    # shot_add_plugin() 的两种参数里，单独加在目标上。
    if(LIBNOTIFY_CFLAGS_OTHER)
        target_compile_options(${SHOT_PLUGIN_NAME} PRIVATE ${LIBNOTIFY_CFLAGS_OTHER})
    endif()
    if(LIBNOTIFY_LIBRARY_DIRS)
        target_link_directories(${SHOT_PLUGIN_NAME} PRIVATE ${LIBNOTIFY_LIBRARY_DIRS})
    endif()
endif()

# 新插件要求 /W4 干净；不加 /WX，避免不同 MSVC 小版本的第三方头差异卡死构建。
if(MSVC)
    target_compile_options(${SHOT_PLUGIN_NAME} PRIVATE /W4)
endif()
