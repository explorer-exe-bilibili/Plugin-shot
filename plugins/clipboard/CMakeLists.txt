# ============================================================================
#  clipboard —— 截屏落盘后把图片写进系统剪贴板（"shot.configurable"）
#
#  插件不自己采集、不自己保存：它订阅 capture.done，拿到保存插件写好的文件路径，
#  用 stb 把文件读回内存，再转成 Win32 剪贴板格式（CF_DIB，可选 CF_HDROP）。
#  配置项 enabled / also_file_drop 由宿主渲染并回灌，插件只负责 get/set。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME clipboard)

# 剪贴板写入是 Win32 实现（OpenClipboard / CF_DIB / CF_HDROP）：非 Windows 平台
# 直接跳过整个插件，而不是留一份跑不起来的 stub。被宿主 add_subdirectory 时用
# PARENT_SCOPE 告诉根工程"这个插件被跳过了"（根 CMakeLists 据此打印提示）。
#  TODO: X11 用 XSetSelectionOwner + image/png target 实现；macOS 用 NSPasteboard
#  （writeObjects:@[NSImage]）实现；在补齐之前非 Windows 平台一直走下面的 return()。
if(NOT WIN32)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "clipboard：仅 Windows（Win32 剪贴板），当前平台跳过")
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
# 没有导入目标、也没有要链接的库。
find_package(Stb REQUIRED)

# nlohmann-json 解析 capture.done 的 JSON 负载（header-only，根 vcpkg 清单里已有，
# 不会给插件目录带进额外运行库）。
find_package(nlohmann_json CONFIG REQUIRED)

# user32：剪贴板 API（OpenClipboard / SetClipboardData）；shell32：DROPFILES /
# CF_HDROP 文件列表。
shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/clipboard_plugin.cpp
                 src/clipboard_config.cpp
                 src/clipboard_writer.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 nlohmann_json::nlohmann_json
                 user32
                 shell32
)

# shot_add_plugin() 只认识 INCLUDE_DIRS / LINK_LIBRARIES 两种参数，所以 stb 的
# 头目录在目标上单独加（header-only，没有链接项）。
target_include_directories(${SHOT_PLUGIN_NAME} PRIVATE ${Stb_INCLUDE_DIR})

# 新插件要求 /W4 干净；不加 /WX，避免不同 MSVC 小版本的第三方头差异卡死构建。
if(MSVC)
    target_compile_options(${SHOT_PLUGIN_NAME} PRIVATE /W4)
endif()
