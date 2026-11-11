# ============================================================================
#  translator —— 外部语言包插件（"shot.configurable"，Generic 插件）
#
#  扫描 <exe>/files/lang/*.json（官方包，priority 200）与
#  %LOCALAPPDATA%\screen-shot\langs\*.json（用户包，priority 300），把每个
#  texts[owner][key] 贡献成 i18n_source_list 的条目（target=owner、priority=来源），
#  并为每个包贡献一条 "@language/<tag>" 语言目录声明。合并与投影都由宿主完成，
#  本插件不碰 ABI、不改宿主任何代码——这就是"语言包"能做到零 ABI 改动的原因。
#
#  构建方式一（宿主仓库内）：根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt)
#  自动纳入本工程，shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME translator)

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

# nlohmann-json：解析语言包文件与拼 lists.add 负载（header-only，根 vcpkg 清单里已有）。
# 除路径解析外只用标准库：文件扫描走 std::filesystem，其它平台也能编译。
find_package(nlohmann_json CONFIG REQUIRED)

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp
                 src/translator_plugin.cpp
                 src/translator_config.cpp
                 src/pack_dirs.cpp
                 src/pack_scanner.cpp
                 src/pack_loader.cpp
                 src/source_publisher.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES
                 nlohmann_json::nlohmann_json
)

if(WIN32)
    # SHGetKnownFolderPath（用户语言包目录取 %LOCALAPPDATA%）在 shell32，
    # CoTaskMemFree（释放它返回的 PWSTR）在 ole32。
    target_link_libraries(${SHOT_PLUGIN_NAME} PRIVATE shell32 ole32)
endif()

# 新插件要求 /W4 干净；不加 /WX，避免不同 MSVC 小版本的第三方头差异卡死构建。
if(MSVC)
    target_compile_options(${SHOT_PLUGIN_NAME} PRIVATE /W4)
endif()
