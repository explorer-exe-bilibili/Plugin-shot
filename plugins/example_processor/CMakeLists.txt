# ============================================================================
#  示例插件工程 —— 一个 DLL 里装两个插件（invert / grayscale）
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 add_subdirectory(plugins/example_processor) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME example_processor)

# SHOT_ENABLE_OPENCV=OFF 时跳过依赖 OpenCV 的插件（CI/无 OpenCV 机器裁剪用；默认 ON）。
# 本插件虽然只用标准库处理像素，但 metadata.json 声明了 "opencv>=4.0.0" 运行时依赖，
# 没有 OpenCV 时它没有完整可运行的意义，所以和 opencv_provider 一起跳过。
if(DEFINED SHOT_ENABLE_OPENCV AND NOT SHOT_ENABLE_OPENCV)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "example_processor：SHOT_ENABLE_OPENCV=OFF，当前构建跳过")
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

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/invert.cpp
                 src/grayscale.cpp
    INCLUDE_DIRS include
)
