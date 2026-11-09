# ============================================================================
#  image_saver —— 图像保存插件（"shot.image_saver" + "shot.configurable"）
#
#  把宿主流水线处理完的 ShotImage 落盘成 PNG/JPEG：格式、质量、目录、文件名模板
#  都是本插件自己的配置项，宿主不硬编码任何保存策略。编码用 vcpkg 的 stb
#  （header-only），跨平台纯标准库 + stb，不引入平台 API。
#
#  构建方式一（宿主仓库内）：根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt)
#  自动纳入本工程，shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME image_saver)

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

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/image_saver.cpp
                 src/saver_config.cpp
                 src/image_encoder.cpp
                 src/output_path.cpp
    INCLUDE_DIRS include
)

# shot_add_plugin() 只认识 INCLUDE_DIRS / LINK_LIBRARIES 两种参数，所以 stb 的
# 头目录在目标上单独加（header-only，没有链接项）。
target_include_directories(${SHOT_PLUGIN_NAME} PRIVATE ${Stb_INCLUDE_DIR})

# 新插件要求 /W4 干净；不加 /WX，避免不同 MSVC 小版本的第三方头差异卡死构建。
if(MSVC)
    target_compile_options(${SHOT_PLUGIN_NAME} PRIVATE /W4)
endif()
