# ============================================================================
#  tray —— 托盘图标插件（"shot.ui_provider"）
#
#  无主窗口架构下，托盘是"没有任何窗口时也能触发动作"的入口：图标默认用宿主应用
#  图标（可用 icon 配置换成 .ico），左键执行默认命令，右键菜单完全由宿主列表服务的
#  tray_command_list 数据驱动（core 预置三项 + 插件 lists.add 贡献的命令）。它自己
#  开线程、自己建隐藏的消息窗口、自己调 Shell_NotifyIconW；宿主只调 init / deinit /
#  post_to_plugin，既不接触 HWND，也不知道插件有几个线程。所有跨插件通信只走事件
#  总线（点击条目发布条目声明的 event；只订阅 lists.changed / config.applied），
#  托盘不直接调用其它插件。成功/失败提示气泡全部由独立的消息提示插件
#  plugins/notify 负责，托盘只管图标 + 菜单。
#
#  平台实现（同一个公共头 include/tray_icon.h，两套 .cpp 互斥编译）：
#    · Windows：Win32 托盘（RegisterClassExW + Shell_NotifyIconW + HMENU），
#      见 src/tray_icon*.cpp；
#    · Linux  ：StatusNotifierItem（org.kde.StatusNotifierItem，走 GIO/GDBus）
#      + com.canonical.dbusmenu，见 src/tray_*_linux.cpp。不引入新库：GIO 是
#      Linux 桌面会话/ Wayland 门户链路本来就有的系统依赖（gio-2.0）；
#    · macOS  ：没有 NSStatusBar/NSStatusItem 实现，整个工程跳过。
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

# 先做平台分支：不支持的平台在声明工程 / 找依赖之前返回，避免把 GIO 之类
# 只有 Linux 才有的系统依赖带给 Windows 构建。被宿主 add_subdirectory 时用
# PARENT_SCOPE 告诉根工程"这个插件被跳过了"（根 CMakeLists 据此打印提示）。
if(WIN32)
    set(SHOT_TRAY_PLATFORM "windows")
elseif(UNIX AND NOT APPLE)
    set(SHOT_TRAY_PLATFORM "linux")
else()
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "tray：仅 Windows（Win32 托盘）与 Linux（StatusNotifierItem），当前平台跳过")
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

# nlohmann-json 用来解析 tray_command_list（菜单数据）与 lists.changed 的 JSON 负载
# （header-only，根 vcpkg 清单里已有，不会给插件目录带进额外运行库）。两个平台共用。
find_package(nlohmann_json CONFIG REQUIRED)

if(SHOT_TRAY_PLATFORM STREQUAL "windows")
    # user32：窗口类/消息/菜单；shell32：Shell_NotifyIconW。
    shot_add_plugin(${SHOT_PLUGIN_NAME}
        METADATA     metadata.json
        SOURCES      src/plugin_entry.cpp
                     src/tray_config.cpp
                     src/tray_icon.cpp
                     src/tray_icon_window.cpp
                     src/tray_icon_icon.cpp
                     src/tray_icon_menu.cpp
                     src/tray_icon_events.cpp
                     src/tray_menu.cpp
        INCLUDE_DIRS include
        LINK_LIBRARIES
                     nlohmann_json::nlohmann_json
                     user32
                     shell32
    )
    return()
endif()

# ----------------------------------------------------------------------------
#  Linux：StatusNotifierItem + com.canonical.dbusmenu（GIO/GDBus，无新依赖）
# ----------------------------------------------------------------------------

# GIO/GDBus 用系统包而不是 vcpkg：会话总线与桌面主题由发行版提供，版本要跟系统
# 一致。Ubuntu 24.04 上是 libglib2.0-dev；pkg-config 找不到时 configure 直接失败，
# 比编到一半才发现 <gio/gio.h> 不存在清楚（提示里点名要装哪个开发包）。
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO QUIET gio-2.0)
if(NOT GIO_FOUND)
    message(FATAL_ERROR
        "tray 的 Linux 后端需要 GIO/GDBus 开发包（pkg-config 找不到 gio-2.0）："
        "Ubuntu/Debian 请安装 libglib2.0-dev（sudo apt install libglib2.0-dev）")
endif()
find_package(Threads REQUIRED)

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/tray_config.cpp
                 src/tray_menu.cpp
                 src/tray_icon_linux.cpp
                 src/tray_sni_linux.cpp
                 src/tray_dbusmenu_linux.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 nlohmann_json::nlohmann_json
                 Threads::Threads
                 ${GIO_LIBRARIES}
)

# shot_add_plugin() 只认识 INCLUDE_DIRS / LINK_LIBRARIES，glib 的 include 路径
# （/usr/include/glib-2.0 等）与 pkg-config 的编译参数在目标上单独加。
target_include_directories(${SHOT_PLUGIN_NAME} PRIVATE ${GIO_INCLUDE_DIRS})
target_compile_options(${SHOT_PLUGIN_NAME} PRIVATE ${GIO_CFLAGS_OTHER})
if(GIO_LIBRARY_DIRS)
    target_link_directories(${SHOT_PLUGIN_NAME} PRIVATE ${GIO_LIBRARY_DIRS})
endif()
