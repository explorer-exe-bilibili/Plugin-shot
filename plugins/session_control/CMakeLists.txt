# ============================================================================
#  session_control —— Linux 会话控制插件（"shot.configurable"，Generic 插件）
#
#  Wayland 下第三方应用没有可移植的全局热键 API（GNOME 46 还没有
#  GlobalShortcuts portal），所以本插件不在进程里抢按键，而是把入口交给桌面：
#  在会话总线上暴露 org.pluginshot.Control，用户在 GNOME「自定义快捷键」/ KDE
#  「自定义快捷键」里绑定一条 gdbus call ... 命令，桌面负责按键、本插件负责把
#  D-Bus 方法翻译成事件总线主题（capture.request / settings.open /
#  long_capture.start）。这样 Wayland 下也有与 hotkey 插件等价的触发入口。
#
#  仅 Linux：GDBus/GIO 跟着发行版的桌面会话走；Windows/macOS 跳过本工程
#  （它们各自的全局热键由别的插件/平台代码负责）。
#
#  构建方式一（宿主仓库内）：根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt)
#  自动纳入本工程，shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
#    独立构建时系统仍需 gio-2.0 开发包（Ubuntu: libglib2.0-dev）。
# ============================================================================

set(SHOT_PLUGIN_NAME session_control)

# GDBus 是 Linux 桌面会话的现成 D-Bus 实现；非 Linux 直接跳过，而不是留一份
# 跑不起来的 stub。被宿主 add_subdirectory 时用 PARENT_SCOPE 告诉根工程
# "这个插件被跳过了"（根 CMakeLists 据此打印提示）。
if(NOT (UNIX AND NOT APPLE))
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "session_control：仅 Linux（D-Bus/GDBus 会话控制），当前平台跳过")
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

# GDBus / GLib 用系统包而不是 vcpkg：会话总线由发行版提供，版本要跟系统一致。
# Ubuntu 24.04 上是 libglib2.0-dev；pkg-config 找不到时 configure 直接报错，
# 比编到一半才发现 <gio/gio.h> 不存在清楚。
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED gio-2.0)
find_package(Threads REQUIRED)
# nlohmann-json：只有 i18n 语言表解析用到（header-only，根 vcpkg 清单里已有）。
find_package(nlohmann_json CONFIG REQUIRED)

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/session_control_plugin.cpp
                 src/session_control_config.cpp
                 src/control_service.cpp
                 src/control_service_dispatch.cpp
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
