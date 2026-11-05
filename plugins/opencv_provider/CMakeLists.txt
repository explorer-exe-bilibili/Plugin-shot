# ============================================================================
#  opencv_provider —— "环境提供者"插件工程
#
#  它不提供任何宿主可调用的能力（模块表里 query_interface 直接是 nullptr），
#  作用是演示"合法但零能力的插件包"，以及插件工程怎么链接 OpenCV。
#
#  构建方式一（宿主仓库内）：根 CMakeLists 自动 add_subdirectory，无需额外操作。
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
# ============================================================================

set(SHOT_PLUGIN_NAME opencv_provider)

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

# 插件工程自己声明依赖，不去依赖宿主有没有 find_package 过 OpenCV。
# 找不到也要能编：那时本插件照样是合法的 lib_provider，只是报不出库版本。
find_package(OpenCV QUIET)
set(_opencv_include_dirs "")
set(_opencv_libs "")
if(OpenCV_FOUND)
    list(APPEND _opencv_include_dirs ${OpenCV_INCLUDE_DIRS})
    list(APPEND _opencv_libs ${OpenCV_LIBS})
endif()

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA       metadata.json
    SOURCES        src/plugin_entry.cpp
    LINK_LIBRARIES ${_opencv_libs}
)

if(_opencv_include_dirs)
    target_include_directories(${SHOT_PLUGIN_NAME} PRIVATE ${_opencv_include_dirs})
    target_compile_definitions(${SHOT_PLUGIN_NAME} PRIVATE OPENCV_PROVIDER_HAS_OPENCV=1)
endif()
