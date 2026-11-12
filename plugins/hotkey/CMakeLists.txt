# ============================================================================
#  hotkey —— 列表驱动的全局热键 provider（"shot.ui_provider"）
#
#  无主窗口架构下，"按一个键就能触发动作"的入口：插件自己开线程、自己建隐藏
#  消息窗口并注册全局热键，按下后只往事件总线发条目声明的事件，不直接调用任何
#  其它插件。热键条目来自通用的 hotkey_list（任何插件都能用 lists.add 贡献，
#  hotkey.main 自己也贡献 capture / region / settings 三条内置项），实际组合键
#  仍存在宿主配置 hotkey.main/<id> 里，设置页用 options="hotkey" 控件录入。
#
#  平台：Windows 用 RegisterHotKey + 隐藏消息窗口；其它平台（默认目标是 Wayland）
#  编一个降级实现 hotkey_window_stub.cpp：列表与配置项照常提供，但不注册全局
#  热键，启动时 WARN 指向 session_control 的 D-Bus 入口。
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
else()
    # Wayland（以及未实现热键的其它平台）：保留 provider 的列表/配置能力，
    # 只把"注册"换成会 WARN 的降级实现，避免插件整个消失。
    list(APPEND HOTKEY_SOURCES src/hotkey_window_stub.cpp)
    message(STATUS "hotkey：非 Windows 平台使用降级实现（发布 hotkey_list 与配置项，不注册全局热键）")
endif()

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      ${HOTKEY_SOURCES}
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 ${HOTKEY_LINK_LIBRARIES}
)
