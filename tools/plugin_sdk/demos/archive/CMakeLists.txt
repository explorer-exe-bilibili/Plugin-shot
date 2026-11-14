# ============================================================================
#  archive —— 一句话 demo：截图保存后自动按日期归档到「年-月-日」文件夹。
#
#  实现为纯订阅者 plugin（shot.configurable + 订阅 capture.done）：
#    · create 里订阅、destroy 里退订；
#    · 事件回调只把路径入队 + 唤醒 worker，真正的文件操作在自己的线程里做
#      （绝不在宿主分发线程上做文件 IO）；
#    · 目标根 / 年月日 / 文件名重名处理 / move-copy 回退见 README。
#
#  离线 SDK 独立构建（不需要 vcpkg、不需要宿主源码）：
#    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#          -DSHOT_SDK_ROOT=<SDK 根目录>
#    cmake --build build
#  产物：build/bin/plugins/archive.dll（Linux 为 .so）。
#
#  SDK 根目录的定位顺序：-DSHOT_SDK_ROOT > 环境变量 SHOT_SDK_ROOT > 模板留在
#  SDK 内时的 ../../manifest.json。显式传参最稳，脚本里建议一直传。
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(archive LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型（Release / Debug）" FORCE)
endif()

# ---- 定位 SDK 根 ----------------------------------------------------------
set(SHOT_SDK_ROOT "${SHOT_SDK_ROOT}" CACHE PATH "Plugin-shot 离线 SDK 根目录")
if(NOT SHOT_SDK_ROOT AND DEFINED ENV{SHOT_SDK_ROOT})
    set(SHOT_SDK_ROOT "$ENV{SHOT_SDK_ROOT}" CACHE PATH "Plugin-shot 离线 SDK 根目录" FORCE)
endif()
if(NOT SHOT_SDK_ROOT)
    get_filename_component(_shot_candidate "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    if(EXISTS "${_shot_candidate}/manifest.json")
        set(SHOT_SDK_ROOT "${_shot_candidate}" CACHE PATH "Plugin-shot 离线 SDK 根目录" FORCE)
    endif()
endif()
if(NOT SHOT_SDK_ROOT)
    message(FATAL_ERROR
        "找不到 ShotPlugin SDK。请加 -DSHOT_SDK_ROOT=<解压后的 SDK 目录>，"
        "或设置环境变量 SHOT_SDK_ROOT。SDK 根目录里应有 manifest.json 和 cmake/ShotPlugin.cmake。")
endif()
get_filename_component(SHOT_SDK_ROOT "${SHOT_SDK_ROOT}" ABSOLUTE)
if(NOT EXISTS "${SHOT_SDK_ROOT}/manifest.json" OR
   NOT EXISTS "${SHOT_SDK_ROOT}/cmake/ShotPlugin.cmake")
    message(FATAL_ERROR "SHOT_SDK_ROOT=${SHOT_SDK_ROOT} 不像 SDK 根目录（缺 manifest.json 或 cmake/ShotPlugin.cmake）")
endif()

list(APPEND CMAKE_MODULE_PATH "${SHOT_SDK_ROOT}/cmake")
include(ShotPlugin)
if(NOT SHOT_SDK_MODE)
    message(FATAL_ERROR "ShotPlugin 没有进入 SDK 模式（manifest.json 判定失败），请检查 SHOT_SDK_ROOT")
endif()

shot_add_plugin(archive
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/archive_plugin.cpp
    INCLUDE_DIRS include
)

if(MSVC)
    target_compile_options(archive PRIVATE /W4)
endif()
