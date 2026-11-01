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
#    · dll 落插件输出目录，导入库落 build/lib，pdb 落 build/pdb/plugins
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
# vcpkg 运行时 DLL 目录：用来把 applocal 拷进插件目录的基础依赖清掉。
# 独立构建（没有 vcpkg 工具链）时保持为空，剪枝脚本会自动跳过这一步。
if(NOT DEFINED SHOT_DEP_SCAN_DIRS AND VCPKG_TARGET_TRIPLET)
    set(SHOT_DEP_SCAN_DIRS
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/bin")
endif()
set(SHOT_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

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
            -P "${SHOT_CMAKE_DIR}/prune_output_dir.cmake"
        COMMENT "插件 ${_target}：贴元数据 + 剪枝产物目录"
        VERBATIM
    )
endfunction()
