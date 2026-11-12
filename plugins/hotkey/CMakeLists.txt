# ============================================================================
#  hotkey —— 列表驱动的全局热键 provider（"shot.ui_provider"）
#
#  无主窗口架构下，"按一个键就能触发动作"的入口：插件自己开线程、自己建隐藏
#  消息窗口并注册全局热键，按下后只往事件总线发条目声明的事件，不直接调用任何
#  其它插件。热键条目来自通用的 hotkey_list（任何插件都能用 lists.add 贡献，
#  hotkey.main 自己也贡献 capture / region / settings 三条内置项），实际组合键
#  仍存在宿主配置 hotkey.main/<id> 里，设置页用 options="hotkey" 控件录入。
#
#  平台：Windows 用 RegisterHotKey + 隐藏消息窗口；Linux 用 X11 的 XGrabKey
#  （hotkey_window_x11*.cpp），运行期没有可用 Display（纯 Wayland）时自动降级成
#  "只提供列表与配置项、不注册热键"，启动时 WARN 指向 session_control 的 D-Bus
#  入口；macOS 等其它平台编 hotkey_window_stub.cpp（同样是降级实现）。
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

# 平台无关部分：插件入口、热键语法、列表目录、动态配置项。
set(HOTKEY_SOURCES
    src/plugin_entry.cpp
    src/hotkey_binding.cpp
    src/hotkey_catalog.cpp
    src/hotkey_config.cpp
)
set(HOTKEY_INCLUDE_DIRS include)

# nlohmann-json：列表快照与 i18n 语言表解析（header-only，根 vcpkg 清单里已有）。
set(HOTKEY_LINK_LIBRARIES nlohmann_json::nlohmann_json)

if(WIN32)
    # user32：RegisterHotKey / UnregisterHotKey、隐藏消息窗口与消息循环。
    list(APPEND HOTKEY_SOURCES
        src/hotkey_window.cpp
        src/hotkey_window_message.cpp
        src/hotkey_window_register.cpp
    )
    list(APPEND HOTKEY_LINK_LIBRARIES user32)
elseif(UNIX AND NOT APPLE)
    # Linux/X11：XGrabKey / XUngrabKey / XNextEvent 与按键事件循环；头文件在
    # <X11/Xlib.h>，链接 libX11（Ubuntu 24.04 包 libx11-dev）。
    # Wayland 没有进程可用的全局热键 API，但这里不加编译开关：X11 实现在运行期
    # 检测 Display，拿不到（纯 Wayland、无 DISPLAY）就自动降级成 stub 行为，只
    # WARN 不失败——同一份二进制在 X11 与 Wayland 会话里都能用。
    find_package(PkgConfig REQUIRED)
    # 前缀用 HOTKEY_X11 而不是 X11：同一个超级构建里 clipboard 用 FindX11（X11_*），
    # 两边同名变量会互相覆盖；pkg_check_modules 的结果只服务本插件。
    pkg_check_modules(HOTKEY_X11 QUIET x11)
    if(NOT HOTKEY_X11_FOUND)
        message(FATAL_ERROR
            "hotkey：缺少 X11 开发文件（XGrabKey 全局热键依赖 libX11）。\n"
            "  Ubuntu/Debian 安装：sudo apt install libx11-dev\n"
            "  缺这个包时 configure 直接失败，而不是编出一个没有热键的插件。")
    endif()
    list(APPEND HOTKEY_SOURCES
        src/hotkey_window_x11.cpp
        src/hotkey_window_x11_loop.cpp
        src/hotkey_window_x11_register.cpp
    )
    # pkg_check_modules 只给变量（没有 imported target）：库用绝对路径，
    # X11 头文件目录单独加（发行版通常也在默认 include 路径里，这里保持通用）。
    list(APPEND HOTKEY_LINK_LIBRARIES ${HOTKEY_X11_LINK_LIBRARIES})
    list(APPEND HOTKEY_INCLUDE_DIRS ${HOTKEY_X11_INCLUDE_DIRS})
    message(STATUS "hotkey：Linux 使用 X11 实现（XGrabKey；无 Display 时运行期降级为 Wayland 提示）")
else()
    # macOS 等未实现全局热键的平台：保留 provider 的列表/配置能力，只把"注册"
    # 换成会 WARN 的降级实现，避免插件整个消失。Linux 不走这个分支（见上）。
    list(APPEND HOTKEY_SOURCES src/hotkey_window_stub.cpp)
    message(STATUS "hotkey：当前平台使用降级实现（发布 hotkey_list 与配置项，不注册全局热键）")
endif()

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      ${HOTKEY_SOURCES}
    INCLUDE_DIRS ${HOTKEY_INCLUDE_DIRS}
    LINK_LIBRARIES
                 ${HOTKEY_LINK_LIBRARIES}
)
