# ============================================================================
#  {{TARGET}} —— 运行库提供者模板（shot.lib_provider）
#
#  离线 SDK 独立构建（不需要 vcpkg、不需要宿主源码）：
#    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#          -DSHOT_SDK_ROOT=<SDK 根目录>
#    cmake --build build
#  产物：build/bin/plugins/{{TARGET}}.dll（Linux 为 .so）。
#
#  这个模板是"真正带库的插件"（opencv_provider 那类）的最小骨架：插件本身不
#  提供宿主可调用的算法，只负责把某个运行库带进进程，并用 shot.lib_provider
#  报告库名与版本；别的插件在 metadata.requires 里写 provides 的名字即可依赖它。
#
#  SDK 根目录的定位顺序：-DSHOT_SDK_ROOT > 环境变量 SHOT_SDK_ROOT > 模板留在
#  SDK 内时的 ../../manifest.json（sdk/templates/<模板>/ 的上一级就是 sdk 根）。
#  显式传参最稳，脚本里建议一直传。
#
#  占位符 {{TARGET}} {{PLUGIN_ID}} 等由 SDK 的 tools/new_plugin.cmake 替换；
#  手工拷模板时改成实际名字即可（README.md 的「改成你自己的插件」列了要改的地方）。
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project({{TARGET}} LANGUAGES CXX)

# 占位符没替换时 CMake 目标名会带 {{ }}，报错会很难懂；这里提前给出可行动的提示。
if(PROJECT_NAME MATCHES "\\{\\{")
    message(FATAL_ERROR
        "这个工程还是模板：请用 <SDK>/tools/new_plugin.cmake 生成，或手工把 "
        "CMakeLists.txt 里的 {{TARGET}} 改成目标名（见 README.md）。")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 单配置生成器（Ninja / Makefiles）没给构建类型时默认 Release；SDK 只承诺
# Release / Debug 两种配置，不要用 RelWithDebInfo 混搭（OpenCV 叠加包按配置选库）。
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
# SDK 模式由 ShotPlugin.cmake 按 <SDK>/manifest.json 自动判定：它负责补第三方头
# （nlohmann / stb）与可选 OpenCV 叠加包的路径，所以本文件不需要写任何第三方依赖。
if(NOT SHOT_SDK_MODE)
    message(FATAL_ERROR "ShotPlugin 没有进入 SDK 模式（manifest.json 判定失败），请检查 SHOT_SDK_ROOT")
endif()

# 真正带库时在这里加 LINK_LIBRARIES：SDK 的 OpenCV 叠加包已按配置给出库目录，
# Release / Debug 的库名不同，用生成表达式避免用户手写 d 后缀：
#   LINK_LIBRARIES
#       $<$<CONFIG:Debug>:opencv_core4d>
#       $<$<NOT:$<CONFIG:Debug>>:opencv_core4>
shot_add_plugin({{TARGET}}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
    INCLUDE_DIRS include
)

# 新插件按仓库习惯开 /W4（不加 /WX：第三方头在不同 MSVC 小版本上偶有警告）。
if(MSVC)
    target_compile_options({{TARGET}} PRIVATE /W4)
endif()
