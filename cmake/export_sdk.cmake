# ============================================================================
#  cmake/export_sdk.cmake — 导出离线插件 SDK（由 export_sdk 目标调用）
#
#  用法（脚本模式，不经过 configure）：
#    cmake -DVCPKG_BIN_DIR=<vcpkg bin>            \
#          -DVCPKG_LIB_DIR=<vcpkg lib>            \
#          -DVCPKG_INC_DIR=<vcpkg include>        \
#          -DCONTRACT_INCLUDE_DIR=<源码 include/> \
#          -DSDK_OUTPUT_DIR=<仓库 sdk/>           \
#          -P cmake/export_sdk.cmake
#
#  交付规则（硬约束）：
#    sdk/include  契约头(plugin_api*.h + plugin_api/ 子目录) + opencv4 头
#    sdk/lib      仅 *.lib
#    sdk/bin      仅 *.dll
#    全树无 *.pdb，无 pkgconfig 噪音，附 README.md
#
#  历史坑：旧写法用 cmake -E copy_directory 整目录拷贝 + cmake -E remove -f
#  "DIR/*.pdb"，通配符无人展开，pdb 与 pkgconfig 全漏进 SDK。
#  本脚本改为按扩展名 GLOB 后逐文件拷贝，并在最后做全树清洗。
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# 逐文件拷贝内容，不修改目标文件权限。
# 不用 file(COPY)：它会把源文件权限 apply 到目标上（chmod），
# 在权限受限的环境里会直接报 "file COPY cannot set permissions" 失败。
function(_copy_files_flat _dest)
    file(MAKE_DIRECTORY "${_dest}")
    foreach(_src_file IN LISTS ARGN)
        get_filename_component(_src_name "${_src_file}" NAME)
        configure_file("${_src_file}" "${_dest}/${_src_name}" COPYONLY)
    endforeach()
endfunction()

# 保留相对目录结构地拷贝（用于 opencv4 头文件树）
function(_copy_files_tree _base _dest)
    file(MAKE_DIRECTORY "${_dest}")
    foreach(_src_file IN LISTS ARGN)
        file(RELATIVE_PATH _src_rel "${_base}" "${_src_file}")
        get_filename_component(_src_rel_dir "${_src_rel}" DIRECTORY)
        get_filename_component(_src_name "${_src_file}" NAME)
        if(_src_rel_dir)
            file(MAKE_DIRECTORY "${_dest}/${_src_rel_dir}")
            configure_file("${_src_file}" "${_dest}/${_src_rel_dir}/${_src_name}" COPYONLY)
        else()
            configure_file("${_src_file}" "${_dest}/${_src_name}" COPYONLY)
        endif()
    endforeach()
endfunction()

foreach(_required VCPKG_BIN_DIR VCPKG_LIB_DIR VCPKG_INC_DIR CONTRACT_INCLUDE_DIR SDK_OUTPUT_DIR SHOT_CMAKE_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "export_sdk: 缺少必需参数 -D${_required}=<path>")
    endif()
endforeach()

foreach(_required_dir VCPKG_BIN_DIR VCPKG_LIB_DIR VCPKG_INC_DIR CONTRACT_INCLUDE_DIR SHOT_CMAKE_DIR)
    if(NOT IS_DIRECTORY "${${_required_dir}}")
        message(FATAL_ERROR "export_sdk: 目录不存在 ${_required_dir} = ${${_required_dir}}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# 1. 三个受管子目录整体重建，杜绝历史残留
# ---------------------------------------------------------------------------
foreach(_sub include lib bin)
    if(EXISTS "${SDK_OUTPUT_DIR}/${_sub}")
        file(REMOVE_RECURSE "${SDK_OUTPUT_DIR}/${_sub}")
    endif()
    file(MAKE_DIRECTORY "${SDK_OUTPUT_DIR}/${_sub}")
endforeach()

# ---------------------------------------------------------------------------
# 2. include/：OpenCV 头（保留子目录结构）+ 契约头
# ---------------------------------------------------------------------------
if(IS_DIRECTORY "${VCPKG_INC_DIR}/opencv4")
    file(GLOB_RECURSE _opencv_headers
        "${VCPKG_INC_DIR}/opencv4/*.h"
        "${VCPKG_INC_DIR}/opencv4/*.hpp"
    )
    if(_opencv_headers)
        _copy_files_tree("${VCPKG_INC_DIR}/opencv4" "${SDK_OUTPUT_DIR}/include/opencv4" ${_opencv_headers})
        list(LENGTH _opencv_headers _hdr_count)
        message(STATUS "export_sdk: ${_hdr_count} 个 OpenCV 头 -> sdk/include/opencv4")
    endif()
else()
    message(WARNING "export_sdk: ${VCPKG_INC_DIR}/opencv4 不存在，SDK 将缺少 OpenCV 头")
endif()

# 契约头 + 插件侧工具头：plugin_api*.h / shot_*.h
# （plugin_api.h 是契约汇总头，只 include；正文在 plugin_api/ 子目录，见下一段。
#   shot_plugin_helpers.h 是工具汇总头，其余 shot_* 是它包含的每类一个头文件；
#   第三方必须一起拿到，所以按前缀整组复制）
file(GLOB _contract_headers
    "${CONTRACT_INCLUDE_DIR}/plugin_api*.h"
    "${CONTRACT_INCLUDE_DIR}/shot_*.h"
)
if(_contract_headers)
    _copy_files_flat("${SDK_OUTPUT_DIR}/include" ${_contract_headers})
else()
    message(WARNING "export_sdk: 未找到契约头 plugin_api*.h（源码 include/ 可能尚未就绪）")
endif()

# 契约正文已经拆到 plugin_api/ 子目录里：汇总头 plugin_api.h 只 include，
# 每个子头对应一个主题（版本 / interface id / 各能力 / 宿主服务 / 布局锚点）。
# 必须保留相对目录结构复制 —— 汇总头里写的是 #include "plugin_api/xxx.h"，
# 平铺进 sdk/include 会让离线 SDK 编译直接报找不到头文件。
file(GLOB _contract_parts "${CONTRACT_INCLUDE_DIR}/plugin_api/*.h")
if(_contract_parts)
    _copy_files_tree("${CONTRACT_INCLUDE_DIR}" "${SDK_OUTPUT_DIR}/include" ${_contract_parts})
    list(LENGTH _contract_parts _contract_part_count)
    message(STATUS "export_sdk: ${_contract_part_count} 个契约子头 -> sdk/include/plugin_api")
else()
    message(WARNING "export_sdk: 未找到 plugin_api/*.h（契约子头，汇总头会 include 不到）")
endif()

# 插件侧工具的实现头在 shot_plugin_helpers/ 子目录里：保留相对目录结构复制，
# 汇总头 shot_plugin_helpers.h 里写的是 #include "shot_plugin_helpers/xxx.h"。
file(GLOB _helper_headers "${CONTRACT_INCLUDE_DIR}/shot_plugin_helpers/*.h")
if(_helper_headers)
    _copy_files_tree("${CONTRACT_INCLUDE_DIR}" "${SDK_OUTPUT_DIR}/include" ${_helper_headers})
    list(LENGTH _helper_headers _helper_count)
    message(STATUS "export_sdk: ${_helper_count} 个插件侧工具头 -> sdk/include/shot_plugin_helpers")
else()
    message(WARNING "export_sdk: 未找到 shot_plugin_helpers/*.h（插件侧工具头）")
endif()

# ---------------------------------------------------------------------------
# 3. lib/：仅 *.lib
# ---------------------------------------------------------------------------
file(GLOB _libs "${VCPKG_LIB_DIR}/*.lib")
if(_libs)
    _copy_files_flat("${SDK_OUTPUT_DIR}/lib" ${_libs})
    list(LENGTH _libs _lib_count)
    message(STATUS "export_sdk: ${_lib_count} 个导入库 -> sdk/lib")
else()
    message(WARNING "export_sdk: ${VCPKG_LIB_DIR} 下没有 *.lib")
endif()

# ---------------------------------------------------------------------------
# 4. bin/：仅 *.dll（绝不含 *.pdb）
# ---------------------------------------------------------------------------
file(GLOB _dlls "${VCPKG_BIN_DIR}/*.dll")
if(_dlls)
    _copy_files_flat("${SDK_OUTPUT_DIR}/bin" ${_dlls})
    list(LENGTH _dlls _dll_count)
    message(STATUS "export_sdk: ${_dll_count} 个运行时 DLL -> sdk/bin")
else()
    message(WARNING "export_sdk: ${VCPKG_BIN_DIR} 下没有 *.dll")
endif()

# ---------------------------------------------------------------------------
# 4.1 debug 变体：lib/debug 与 bin/debug
#
# 为什么必须分目录、绝不能平铺：vcpkg 的 debug 产物里有大量与 release 【同名】的
# 文件（jpeg62.dll / turbojpeg.dll / liblzma.dll / libwebp*.dll / jpeg.lib /
# lzma.lib ... 文件名里没有 d 后缀）。平铺进同一个 sdk/bin 或 sdk/lib 会互相
# 覆盖 —— SDK 会被悄悄替换成其中一种变体，而且不会有任何报错。
# ---------------------------------------------------------------------------
if(DEFINED VCPKG_DEBUG_LIB_DIR AND IS_DIRECTORY "${VCPKG_DEBUG_LIB_DIR}")
    file(GLOB _debug_libs "${VCPKG_DEBUG_LIB_DIR}/*.lib")
    if(_debug_libs)
        _copy_files_flat("${SDK_OUTPUT_DIR}/lib/debug" ${_debug_libs})
        list(LENGTH _debug_libs _debug_lib_count)
        message(STATUS "export_sdk: ${_debug_lib_count} 个 debug 导入库 -> sdk/lib/debug")
    endif()
else()
    message(WARNING "export_sdk: 未找到 debug 导入库目录 ${VCPKG_DEBUG_LIB_DIR}，SDK 仅含 release")
endif()

if(DEFINED VCPKG_DEBUG_BIN_DIR AND IS_DIRECTORY "${VCPKG_DEBUG_BIN_DIR}")
    file(GLOB _debug_dlls "${VCPKG_DEBUG_BIN_DIR}/*.dll")
    if(_debug_dlls)
        _copy_files_flat("${SDK_OUTPUT_DIR}/bin/debug" ${_debug_dlls})
        list(LENGTH _debug_dlls _debug_dll_count)
        message(STATUS "export_sdk: ${_debug_dll_count} 个 debug 运行时 DLL -> sdk/bin/debug")
    endif()
else()
    message(WARNING "export_sdk: 未找到 debug 运行时目录 ${VCPKG_DEBUG_BIN_DIR}，SDK 仅含 release")
endif()

# ---------------------------------------------------------------------------
# 5. 全树清洗：*.pdb / *.pc / pkgconfig 噪音
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _sdk_pdb "${SDK_OUTPUT_DIR}/*.pdb")
if(_sdk_pdb)
    list(LENGTH _sdk_pdb _pdb_count)
    file(REMOVE ${_sdk_pdb})
    message(STATUS "export_sdk: 剔除 PDB ${_pdb_count} 个")
endif()

file(GLOB_RECURSE _sdk_pc "${SDK_OUTPUT_DIR}/*.pc")
if(_sdk_pc)
    file(REMOVE ${_sdk_pc})
    message(STATUS "export_sdk: 剔除 pkgconfig *.pc")
endif()

# 注意：不能用 file(GLOB_RECURSE X LIST_DIRECTORIES true "<dir>/pkgconfig")。
# 无通配符的 GLOB_RECURSE 会把 <dir> 整棵树递归出来并返回所有子目录，
# 直接 REMOVE_RECURSE 会把 include/lib/bin 一起删掉（实测过）。
# 必须用带通配符的模式取全量条目，再按名字筛。
file(GLOB_RECURSE _sdk_entries LIST_DIRECTORIES true "${SDK_OUTPUT_DIR}/*")
set(_pkgconfig_dirs "")
foreach(_entry IN LISTS _sdk_entries)
    if(IS_DIRECTORY "${_entry}" AND _entry MATCHES "/pkgconfig$")
        list(APPEND _pkgconfig_dirs "${_entry}")
    endif()
endforeach()
if(_pkgconfig_dirs)
    file(REMOVE_RECURSE ${_pkgconfig_dirs})
    message(STATUS "export_sdk: 剔除 pkgconfig 目录")
endif()

# ---------------------------------------------------------------------------
# 5.5 cmake/：插件工程构建模块，让 SDK 用户也能直接用 shot_add_plugin()
# ---------------------------------------------------------------------------
file(MAKE_DIRECTORY "${SDK_OUTPUT_DIR}/cmake")
foreach(_module ShotPlugin.cmake AppendPluginMeta.cmake ValidatePluginMeta.cmake
        ReadContractHeader.cmake prune_output_dir.cmake)
    if(EXISTS "${SHOT_CMAKE_DIR}/${_module}")
        configure_file("${SHOT_CMAKE_DIR}/${_module}" "${SDK_OUTPUT_DIR}/cmake/${_module}" COPYONLY)
    else()
        message(WARNING "export_sdk: 缺少构建模块 ${SHOT_CMAKE_DIR}/${_module}")
    endif()
endforeach()
message(STATUS "export_sdk: 插件构建模块 -> sdk/cmake")

# ---------------------------------------------------------------------------
# 6. README.md —— 直接复制 document/README_SDK.md（单一事实来源）
#
#    这里过去硬编码一整份 README 文本，与入库的 document/README_SDK.md 逐渐
#    分叉，还出现过 "宿主已 target_link_libraries(OpenCV_LIBS)" 这类与事实
#    相反的说法（宿主根本不链接 OpenCV，运行时 DLL 由 copy_runtime_deps
#    从 vcpkg 目录布置）。改为复制后，两处不会再各自演化。
# ---------------------------------------------------------------------------
set(_sdk_readme_src "${SHOT_CMAKE_DIR}/../document/README_SDK.md")
if(EXISTS "${_sdk_readme_src}")
    configure_file("${_sdk_readme_src}" "${SDK_OUTPUT_DIR}/README.md" COPYONLY)
    message(STATUS "export_sdk: README.md <- ${_sdk_readme_src}")
else()
    message(WARNING "export_sdk: 未找到 ${_sdk_readme_src}，SDK 不附 README.md")
endif()

message(STATUS "export_sdk: 完成 -> ${SDK_OUTPUT_DIR}（include/lib/bin 均无 PDB）")
