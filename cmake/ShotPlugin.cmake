# ============================================================================
#  cmake/ShotPlugin.cmake — 插件工程的公共构建规则（宿主与离线 SDK 共用）
#
#  用法（插件工程的 CMakeLists.txt）：
#    list(APPEND CMAKE_MODULE_PATH "<宿主仓库>/cmake" 或 "<SDK>/cmake")
#    include(ShotPlugin)
#    shot_add_plugin(example_processor
#        METADATA     metadata.json
#        SOURCES      src/a.cpp src/b.cpp
#        INCLUDE_DIRS include                    # 可选
#        LINK_LIBRARIES opencv_core4)            # 可选
#
#  本函数统一负责（这些规则以前散落在根 CMakeLists 里，容易与文档漂移）：
#    · PREFIX "" —— 产物名就是目标名
#    · CXX_VISIBILITY_PRESET hidden —— 只导出 SCREENSHOT_API 标注的符号
#    · MSVC 运行库 /MD —— 与宿主一致，否则跨 DLL 释放会崩
#    · 动态库落插件输出目录（Windows .dll / macOS .dylib / Linux .so），
#      导入库落 build/lib，pdb 落 build/pdb/plugins
#    · 构建后先校验 metadata.json（id 唯一 / interfaces 已注册 / abi 与头一致），
#      校验不过就不贴尾、直接构建失败
#    · 构建后把 metadata.json 贴到 dll 尾部（纯 CMake，不需要 Python）
#    · 构建后剪枝插件目录（不留 pdb/ilk/exp/lib）
#
#  输入变量（宿主构建会预先设置；独立构建时用下面的默认值）：
#    SHOT_CONTRACT_INCLUDE_DIR  契约头目录              默认 <本文件>/../include
#    SHOT_PLUGINS_OUTPUT_DIR    插件产物目录            默认 <build>/bin/plugins
#    SHOT_BUILD_LIB_DIR         插件导入库目录          默认 <build>/lib
#    SHOT_PDB_DIR               插件 pdb 目录           默认 <build>/pdb/plugins
#    SHOT_APP_OUTPUT_DIR        宿主产物目录（剪枝用）   未定义则跳过
#    SHOT_DYN_EXT               动态库扩展名            默认按平台（.dll/.dylib/.so）
#    SHOT_VCPKG_LIB_DIRS        vcpkg 运行库目录        非 Windows 作为 RPATH
# ============================================================================

if(NOT DEFINED SHOT_CONTRACT_INCLUDE_DIR)
    get_filename_component(_shot_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(SHOT_CONTRACT_INCLUDE_DIR "${_shot_repo_root}/include")
endif()
if(NOT DEFINED SHOT_PLUGINS_OUTPUT_DIR)
    set(SHOT_PLUGINS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/plugins")
endif()
if(NOT DEFINED SHOT_BUILD_LIB_DIR)
    set(SHOT_BUILD_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
endif()
if(NOT DEFINED SHOT_PDB_DIR)
    set(SHOT_PDB_DIR "${CMAKE_BINARY_DIR}/pdb/plugins")
endif()
if(NOT DEFINED SHOT_DYN_EXT)
    if(WIN32)
        set(SHOT_DYN_EXT ".dll")
    elseif(APPLE)
        set(SHOT_DYN_EXT ".dylib")
    else()
        set(SHOT_DYN_EXT ".so")
    endif()
endif()
# vcpkg 运行时 DLL 目录：用来把 applocal 拷进插件目录的基础依赖清掉。
# 独立构建（没有 vcpkg 工具链）时保持为空，剪枝脚本会自动跳过这一步。
if(NOT DEFINED SHOT_DEP_SCAN_DIRS AND VCPKG_TARGET_TRIPLET)
    # 尊重 VCPKG_INSTALLED_DIR（见根 CMakeLists.txt §5.1）：CI / 复用已有依赖时
    # 依赖目录不在构建目录下，仍按 CMAKE_BINARY_DIR 拼会扫错地方。
    if(NOT DEFINED SHOT_VCPKG_INSTALLED_DIR OR SHOT_VCPKG_INSTALLED_DIR STREQUAL "")
        set(SHOT_VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed")
    endif()
    if(WIN32)
        set(SHOT_DEP_SCAN_DIRS
            "${SHOT_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
            "${SHOT_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
    else()
        set(SHOT_DEP_SCAN_DIRS
            "${SHOT_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
            "${SHOT_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib")
    endif()
endif()
set(SHOT_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# ---------------------------------------------------------------------------
# SDK 模式判定（同一份模块同时服务宿主仓库与离线 SDK）
#
# 判据 = <本文件目录>/../manifest.json 是否存在：export_sdk 导出的 SDK 根有
# manifest.json，宿主仓库根没有（仓库根也没有 opencv/、third_party/、lib/），
# 所以这段对宿主构建完全惰性，13 个插件在仓库内构建的行为不变。
#
# 必须用文件作用域的 SHOT_CMAKE_DIR：在 shot_add_plugin() 函数体里
# CMAKE_CURRENT_LIST_DIR 指向【调用方】的 CMakeLists 目录，判定会永远为假、
# 静默失效（实测踩过，见 build/sdk-design/design.md 风险 3）。
#
# 用普通变量而不是缓存变量：插件工程可以用 if(SHOT_SDK_ROOT) 判断自己是不是
# 在 SDK 里构建；模板也用 SHOT_SDK_MODE 决定要不要提示"设置 SDK 根"。
# ---------------------------------------------------------------------------
set(SHOT_SDK_MODE FALSE)
set(SHOT_SDK_ROOT "")
get_filename_component(_shot_sdk_root_candidate "${SHOT_CMAKE_DIR}/.." ABSOLUTE)
if(EXISTS "${_shot_sdk_root_candidate}/manifest.json")
    set(SHOT_SDK_MODE TRUE)
    set(SHOT_SDK_ROOT "${_shot_sdk_root_candidate}")
    message(STATUS "ShotPlugin: SDK 模式（根 ${SHOT_SDK_ROOT}）")
endif()

function(shot_add_plugin _target)
    cmake_parse_arguments(PARSE_ARGV 1 _arg "" "METADATA" "SOURCES;INCLUDE_DIRS;LINK_LIBRARIES")

    if(NOT _arg_METADATA)
        message(FATAL_ERROR "shot_add_plugin(${_target}): 缺少 METADATA <metadata.json>")
    endif()
    if(NOT _arg_SOURCES)
        message(FATAL_ERROR "shot_add_plugin(${_target}): 缺少 SOURCES <源文件...>")
    endif()

    get_filename_component(_meta "${_arg_METADATA}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${_meta}")
        message(FATAL_ERROR "shot_add_plugin(${_target}): 元数据文件不存在 ${_meta}")
    endif()

    add_library(${_target} SHARED ${_arg_SOURCES})

    # Ninja：把插件的链接（连同挂在链接上的 POST_BUILD 剪枝）放进单并发池。
    # 链接器会把 *.ilk 临时写到 dll 旁边，多个插件并行链接时，一个插件的剪枝会
    # 扫到另一个插件"刚生成、还没被它自己的剪枝删掉"的 ilk，误报"清理后仍存在违规文件"。
    # 池在根 CMakeLists.txt 的「12.1 Ninja 链接池」里定义（shot_link=1）。独立 SDK 工程
    # 没有宿主根 CMakeLists，这里兜底定义，否则 Ninja 报 "unknown pool name 'shot_link'"，
    # 离线 SDK 的插件构建会直接失败（ValidatePluginMeta/AppendPluginMeta 都跑不到）。
    if(CMAKE_GENERATOR MATCHES "Ninja")
        if(NOT CMAKE_JOB_POOLS MATCHES "(^|;)shot_link=")
            if(DEFINED CMAKE_JOB_POOLS AND NOT CMAKE_JOB_POOLS STREQUAL "")
                set(CMAKE_JOB_POOLS "${CMAKE_JOB_POOLS};shot_link=1" PARENT_SCOPE)
            else()
                set(CMAKE_JOB_POOLS "shot_link=1" PARENT_SCOPE)
            endif()
        endif()
        set_property(TARGET ${_target} PROPERTY JOB_POOL_LINK shot_link)
    endif()

    # 改 metadata.json 必须触发重新链接，否则 dll 里留的还是旧清单。
    # 把 json 塞进源文件列表没用（Ninja 不认非编译文件），要显式声明链接依赖。
    set_property(TARGET ${_target} APPEND PROPERTY LINK_DEPENDS "${_meta}")

    set_target_properties(${_target} PROPERTIES
        OUTPUT_NAME                   "${_target}"
        PREFIX                        ""
        CXX_VISIBILITY_PRESET         hidden
        VISIBILITY_INLINES_HIDDEN     ON
        RUNTIME_OUTPUT_DIRECTORY      "${SHOT_PLUGINS_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY      "${SHOT_PLUGINS_OUTPUT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY      "${SHOT_BUILD_LIB_DIR}"
        PDB_OUTPUT_DIRECTORY          "${SHOT_PDB_DIR}"
        COMPILE_PDB_OUTPUT_DIRECTORY  "${SHOT_PDB_DIR}"
    )

    target_include_directories(${_target} PRIVATE "${SHOT_CONTRACT_INCLUDE_DIR}")

    # -----------------------------------------------------------------------
    # SDK 模式：补上 SDK 自带的第三方头与可选 OpenCV 叠加包路径。
    # 宿主仓库构建时 SHOT_SDK_MODE 恒为 FALSE，这段完全惰性（13 个仓库内插件不变）。
    #   third_party/include <- nlohmann/ 整树 + stb_*.h。shot_plugin_helpers.h 无条件
    #                          包含 <nlohmann/json.hpp>，不补就是 C1083（实测 E1）；
    #                          stb 是扁平文件名，源码写 "stb_image_write.h"（实测 E2b）。
    #   opencv/manifest.json <- 可选叠加包，存在才补 OpenCV 头/库目录（实测 E3a/E3b）。
    #   链接目录必须 per-target + 按配置选（不用全局 link_directories）：
    #     · 单配置生成器（Ninja）：$<CONFIG:Debug> 由 CMAKE_BUILD_TYPE 决定；
    #     · 多配置生成器（VS）：每个配置各自解析一次，结果同样正确。
    #   混用 Release/Debug 变体会在链接期报 LNK2019 cv::debug_build_guard::...（实测 E3e），
    #   所以这里必须按配置给目录，不能只给 lib。
    # -----------------------------------------------------------------------
    if(SHOT_SDK_MODE)
        if(EXISTS "${SHOT_SDK_ROOT}/third_party/include")
            target_include_directories(${_target} PRIVATE "${SHOT_SDK_ROOT}/third_party/include")
        endif()
        if(EXISTS "${SHOT_SDK_ROOT}/opencv/manifest.json")
            if(EXISTS "${SHOT_SDK_ROOT}/opencv/include/opencv4")
                target_include_directories(${_target} PRIVATE
                    "${SHOT_SDK_ROOT}/opencv/include/opencv4")
            endif()
            if(IS_DIRECTORY "${SHOT_SDK_ROOT}/opencv/lib")
                target_link_directories(${_target} PRIVATE
                    "$<IF:$<CONFIG:Debug>,${SHOT_SDK_ROOT}/opencv/lib/debug,${SHOT_SDK_ROOT}/opencv/lib>")
            endif()
        endif()
    endif()

    # 非 Windows 的共享库不在 exe 旁边：插件被宿主 dlopen 时也要能找到 vcpkg 运行库。
    if(UNIX AND SHOT_VCPKG_LIB_DIRS)
        set_target_properties(${_target} PROPERTIES BUILD_RPATH "${SHOT_VCPKG_LIB_DIRS}")
    endif()
    target_compile_definitions(${_target} PRIVATE NOMINMAX)

    if(_arg_INCLUDE_DIRS)
        target_include_directories(${_target} PRIVATE ${_arg_INCLUDE_DIRS})
    endif()
    if(_arg_LINK_LIBRARIES)
        target_link_libraries(${_target} PRIVATE ${_arg_LINK_LIBRARIES})
    endif()

    if(MSVC)
        set_property(TARGET ${_target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        target_compile_options(${_target} PRIVATE /utf-8)
    endif()

    # 贴元数据与剪枝都必须挂在【链接之后】：链接器每次都会重写 dll，
    # 只有 POST_BUILD 能保证贴的是刚产出的那一份。
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DMETADATA_FILE=${_meta}"
            "-DCONTRACT_HEADER=${SHOT_CONTRACT_INCLUDE_DIR}/plugin_api.h"
            "-DTARGET_NAME=${_target}"
            -P "${SHOT_CMAKE_DIR}/ValidatePluginMeta.cmake"
        COMMAND ${CMAKE_COMMAND}
            "-DTARGET_FILE=$<TARGET_FILE:${_target}>"
            "-DMETADATA_FILE=${_meta}"
            "-DCONTRACT_HEADER=${SHOT_CONTRACT_INCLUDE_DIR}/plugin_api.h"
            -P "${SHOT_CMAKE_DIR}/AppendPluginMeta.cmake"
        COMMAND ${CMAKE_COMMAND}
            "-DAPP_OUTPUT_DIR=${SHOT_APP_OUTPUT_DIR}"
            "-DPLUGINS_OUTPUT_DIR=${SHOT_PLUGINS_OUTPUT_DIR}"
            "-DDEP_SCAN_DIRS=${SHOT_DEP_SCAN_DIRS}"
            "-DDYN_EXT=${SHOT_DYN_EXT}"
            -P "${SHOT_CMAKE_DIR}/prune_output_dir.cmake"
        COMMENT "插件 ${_target}：贴元数据 + 剪枝产物目录"
        VERBATIM
    )
endfunction()
