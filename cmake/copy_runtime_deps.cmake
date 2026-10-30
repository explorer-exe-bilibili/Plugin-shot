# ============================================================================
#  cmake/copy_runtime_deps.cmake — 构建期运行时依赖布局（由 copy_dlls 目标调用）
#
#  用法（脚本模式，不经过 configure）：
#    cmake -DRUNTIME_BIN_DIR=<本配置要拷的 vcpkg bin> \
#          -DVCPKG_BIN_DIR=<release bin，泄漏检测用> \
#          -DVCPKG_DEBUG_BIN_DIR=<debug bin，泄漏检测用> \
#          -DAPP_OUTPUT_DIR=<宿主输出目录> \
#          -DPLUGINS_OUTPUT_DIR=<插件输出目录> \
#          -P cmake/copy_runtime_deps.cmake
#
#  产物规则（硬约束）：
#    1. bin/          只允许运行依赖，且【绝不允许 *.pdb】
#    2. bin/plugins/  只允许插件自身产物，【绝不允许】OpenCV 等基础依赖 DLL/PDB
#
#  缺陷3（已修）：x64-windows 三胞胎把 release DLL 放 <triplet>/bin、
#  debug DLL 放 <triplet>/debug/bin。宿主 Debug 构建必须拷 debug 那份，
#  否则运行时找不到 opencv_core4d.dll。拷贝源由 RUNTIME_BIN_DIR 按配置传入。
#
#  缺陷3-b（已修）：插件目录的“基础依赖泄漏”检测必须同时对照 release 与
#  debug 两个 bin 目录，否则 opencv_*4d.dll 这类 debug 泄漏检测不到。
#
#  历史坑：旧写法 cmake -E remove -f "DIR/*.pdb" 的通配符无人展开
#  （cmake -E 不做 glob 展开），导致 pdb 从未被删掉。
#  本脚本一律用 file(GLOB) + file(REMOVE) 在 CMake 内部展开通配符。
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

foreach(_required RUNTIME_BIN_DIR APP_OUTPUT_DIR PLUGINS_OUTPUT_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "copy_runtime_deps: 缺少必需参数 -D${_required}=<path>")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${RUNTIME_BIN_DIR}")
    message(FATAL_ERROR "copy_runtime_deps: 运行时依赖目录不存在: ${RUNTIME_BIN_DIR}")
endif()

message(STATUS "copy_runtime_deps: 拷贝源 = ${RUNTIME_BIN_DIR}")

# ---------------------------------------------------------------------------
# 1. 目录创建放到构建期（configure 期做会把 $<CONFIG> 建成字面量目录）
# ---------------------------------------------------------------------------
file(MAKE_DIRECTORY "${APP_OUTPUT_DIR}" "${PLUGINS_OUTPUT_DIR}")

# ---------------------------------------------------------------------------
# 2. 本配置的 vcpkg 运行时 DLL -> 宿主输出目录；只取 *.dll，绝不拷 *.pdb
# ---------------------------------------------------------------------------
file(GLOB _runtime_dlls "${RUNTIME_BIN_DIR}/*.dll")
if(_runtime_dlls)
    _copy_files_flat("${APP_OUTPUT_DIR}" ${_runtime_dlls})
    list(LENGTH _runtime_dlls _runtime_dll_count)
    message(STATUS "copy_runtime_deps: ${_runtime_dll_count} 个运行时 DLL -> ${APP_OUTPUT_DIR}")
else()
    message(WARNING "copy_runtime_deps: ${RUNTIME_BIN_DIR} 下没有找到 *.dll")
endif()

# ---------------------------------------------------------------------------
# 3. 自愈：宿主输出目录里历史遗留的 *.pdb 全部清掉
# ---------------------------------------------------------------------------
file(GLOB _app_stale_pdb "${APP_OUTPUT_DIR}/*.pdb")
if(_app_stale_pdb)
    list(LENGTH _app_stale_pdb _app_stale_pdb_count)
    file(REMOVE ${_app_stale_pdb})
    message(STATUS "copy_runtime_deps: 清理宿主目录遗留 PDB ${_app_stale_pdb_count} 个")
endif()

# ---------------------------------------------------------------------------
# 4. 自愈：插件目录只许放插件自身产物
#    - *.pdb 一律删除
#    - 任何与 vcpkg release 或 debug 运行时 DLL 同名的 DLL 都是基础依赖，
#      必须清走（插件复用宿主进程已加载的 OpenCV 实例，不得自带副本）
# ---------------------------------------------------------------------------
set(_dep_scan_dirs "")
foreach(_candidate "${VCPKG_BIN_DIR}" "${VCPKG_DEBUG_BIN_DIR}")
    if(_candidate AND IS_DIRECTORY "${_candidate}")
        list(APPEND _dep_scan_dirs "${_candidate}")
    endif()
endforeach()
list(LENGTH _dep_scan_dirs _dep_scan_count)
message(STATUS "copy_runtime_deps: 泄漏检测对照 ${_dep_scan_count} 个依赖目录")

set(_leaked "")
file(GLOB _plugin_pdb "${PLUGINS_OUTPUT_DIR}/*.pdb")
list(APPEND _leaked ${_plugin_pdb})

file(GLOB _plugin_dlls "${PLUGINS_OUTPUT_DIR}/*.dll")
foreach(_plugin_dll IN LISTS _plugin_dlls)
    get_filename_component(_plugin_dll_name "${_plugin_dll}" NAME)
    foreach(_scan_dir IN LISTS _dep_scan_dirs)
        if(EXISTS "${_scan_dir}/${_plugin_dll_name}")
            list(APPEND _leaked "${_plugin_dll}")
            message(STATUS "copy_runtime_deps: 插件目录发现基础依赖 ${_plugin_dll_name}（对照 ${_scan_dir}）")
        endif()
    endforeach()
endforeach()

if(_leaked)
    list(REMOVE_DUPLICATES _leaked)
    list(LENGTH _leaked _leaked_count)
    file(REMOVE ${_leaked})
    message(STATUS "copy_runtime_deps: 从插件目录清走基础依赖/PDB ${_leaked_count} 个")
endif()

# ---------------------------------------------------------------------------
# 5. 变体卫生：清掉宿主输出目录里「属于另一个变体」的依赖 DLL
#    Debug 包里遗留的 release 版 opencv_core4.dll 正是缺陷3 的表象
#    （bin/ 里 20 个 DLL 全是 release 版，而构建是 Debug）。
#    判定很保守：只删「名字出现在某个 vcpkg bin 目录、但不在本配置拷贝源里」
#    的 DLL —— 宿主自己的 exe 不在 vcpkg 里，永远不会被误删。
# ---------------------------------------------------------------------------
set(_stale_variant "")
file(GLOB _app_dlls "${APP_OUTPUT_DIR}/*.dll")
foreach(_app_dll IN LISTS _app_dlls)
    get_filename_component(_app_dll_name "${_app_dll}" NAME)
    if(EXISTS "${RUNTIME_BIN_DIR}/${_app_dll_name}")
        continue()
    endif()
    foreach(_scan_dir IN LISTS _dep_scan_dirs)
        if(EXISTS "${_scan_dir}/${_app_dll_name}")
            list(APPEND _stale_variant "${_app_dll}")
            message(STATUS "copy_runtime_deps: 宿主目录发现另一变体的依赖 ${_app_dll_name}（来自 ${_scan_dir}）")
            break()
        endif()
    endforeach()
endforeach()

if(_stale_variant)
    list(REMOVE_DUPLICATES _stale_variant)
    list(LENGTH _stale_variant _stale_count)
    file(REMOVE ${_stale_variant})
    message(STATUS "copy_runtime_deps: 清掉宿主目录里另一变体的依赖 DLL ${_stale_count} 个")
endif()

message(STATUS "copy_runtime_deps: 完成 —— bin/ 无 PDB 且只含本配置变体，plugins/ 仅插件自身产物")
