# ============================================================================
#  cmake/prune_output_dir.cmake — 产物目录剪枝（挂成目标 POST_BUILD）
#
#  用法（脚本模式）：
#    cmake -DAPP_OUTPUT_DIR=<宿主输出目录> \
#          -DPLUGINS_OUTPUT_DIR=<插件输出目录> \
#          -DDYN_EXT=.dll \                          # 可选：.dll/.dylib/.so，默认 .dll
#          -DDEP_SCAN_DIRS="<vcpkg 运行库目录>[...]" \  # 可选：基础依赖泄漏对照
#          -P cmake/prune_output_dir.cmake
#
#  为什么需要这个脚本（缺陷1 的根治）：
#    copy_dlls 是 ALL 目标，Ninja 可以把它排在链接之前（实测 [1/7]），
#    那时宿主 Plugin-shot.pdb 还没被链接器生成，清理永远清不到。
#    POST_BUILD 依附在目标自身的链接边上，链接完成后才执行，所以本脚本
#    一定能看到链接器刚产出的 pdb/ilk。
#
#  规则（按平台选择，DYN_EXT 决定平台语义）：
#    APP_OUTPUT_DIR     : Windows 上不得出现 *.pdb / *.ilk
#    PLUGINS_OUTPUT_DIR : 顶层只允许插件自身的动态库（扩展名 == DYN_EXT；
#                         Windows 为 .dll / macOS .dylib / Linux .so）；
#                         Windows 额外清掉 *.pdb / *.ilk / *.exp / *.lib；
#                         与 vcpkg 基础依赖同名的动态库也清掉（见第 2.5 节）
#
#  禁止 cmake -E remove 带通配符（不展开）；一律 file(GLOB)+file(REMOVE)。
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# APP_OUTPUT_DIR 允许为空：插件工程用 SDK 独立构建时没有宿主产物目录。
# 空值/不存在的目录在下面按"跳过"处理，不报错。
foreach(_required PLUGINS_OUTPUT_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "prune_output_dir: 缺少必需参数 -D${_required}=<path>")
    endif()
endforeach()

# 动态库扩展名：默认 .dll（兼容旧的 Windows/SDK 调用）；macOS 传 .dylib，Linux 传 .so。
if(NOT DEFINED DYN_EXT OR DYN_EXT STREQUAL "")
    set(DYN_EXT ".dll")
endif()
string(TOLOWER "${DYN_EXT}" _dyn_ext)

# pdb / ilk / exp / lib 是 MSVC 产物；其它平台不做这些清理。
set(_is_msvc_platform FALSE)
if(_dyn_ext STREQUAL ".dll")
    set(_is_msvc_platform TRUE)
endif()

set(_pruned_total 0)

# ---------------------------------------------------------------------------
# 1. Windows：两个产物目录递归清 *.pdb / *.ilk
#    （通配符必须写进模式里，由 file(GLOB) 展开）
# ---------------------------------------------------------------------------
if(_is_msvc_platform)
    foreach(_dir "${APP_OUTPUT_DIR}" "${PLUGINS_OUTPUT_DIR}")
        if(NOT IS_DIRECTORY "${_dir}")
            message(STATUS "prune_output_dir: 目录不存在，跳过 ${_dir}")
            continue()
        endif()

        file(GLOB_RECURSE _junk "${_dir}/*.pdb" "${_dir}/*.ilk")
        if(_junk)
            list(LENGTH _junk _junk_count)
            file(REMOVE ${_junk})
            math(EXPR _pruned_total "${_pruned_total} + ${_junk_count}")
            message(STATUS "prune_output_dir: 清掉 ${_junk_count} 个 *.pdb/*.ilk <- ${_dir}")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 2. 插件目录白名单：顶层只允许插件自身的动态库（扩展名 == DYN_EXT）
#    （子目录不动；这里顺带兜住 .exp/.lib/.ilk/调试文件之类）
# ---------------------------------------------------------------------------
if(IS_DIRECTORY "${PLUGINS_OUTPUT_DIR}")
    file(GLOB _plugin_entries "${PLUGINS_OUTPUT_DIR}/*")
    set(_non_dyn "")
    foreach(_entry IN LISTS _plugin_entries)
        if(IS_DIRECTORY "${_entry}")
            continue()
        endif()
        get_filename_component(_entry_ext "${_entry}" EXT)
        string(TOLOWER "${_entry_ext}" _entry_ext_lc)
        # Linux 共享库可能带版本后缀（libfoo.so.1）：前缀匹配也算合法。
        if(NOT _entry_ext_lc STREQUAL "${_dyn_ext}")
            list(APPEND _non_dyn "${_entry}")
        endif()
    endforeach()

    if(_non_dyn)
        list(LENGTH _non_dyn _non_dyn_count)
        file(REMOVE ${_non_dyn})
        math(EXPR _pruned_total "${_pruned_total} + ${_non_dyn_count}")
        message(STATUS "prune_output_dir: 插件目录清掉 ${_non_dyn_count} 个非 ${DYN_EXT} 文件")
    endif()
endif()

# ---------------------------------------------------------------------------
# 2.5 插件目录不许出现基础依赖（vcpkg 的 applocal 会把它们拷到插件旁边）
#
#    -DDEP_SCAN_DIRS="<vcpkg 运行库目录>..."：这些目录里同名的动态库都算基础依赖，
#    一律从插件目录清走。Windows 是 <triplet>/(debug/)bin，macOS/Linux 是
#    <triplet>/(debug/)lib。
#
#    为什么必须清：插件一旦链接 OpenCV，applocal 就会把 opencv 运行库拷到插件旁边。
#    那会在同一进程里造出第二个 OpenCV —— 类型信息、全局状态、内存分配器全部割裂，
#    表现为莫名其妙的崩溃。插件必须复用宿主已加载的那一份。
#    这里只作用于 PLUGINS_OUTPUT_DIR：宿主输出目录当然要留着这些库。
# ---------------------------------------------------------------------------
if(DEFINED DEP_SCAN_DIRS AND DEP_SCAN_DIRS AND IS_DIRECTORY "${PLUGINS_OUTPUT_DIR}")
    file(GLOB _plugin_dyns "${PLUGINS_OUTPUT_DIR}/*${DYN_EXT}")
    set(_leaked_deps "")
    foreach(_dyn IN LISTS _plugin_dyns)
        get_filename_component(_dyn_name "${_dyn}" NAME)
        foreach(_scan IN LISTS DEP_SCAN_DIRS)
            if(EXISTS "${_scan}/${_dyn_name}")
                list(APPEND _leaked_deps "${_dyn}")
                break()
            endif()
        endforeach()
    endforeach()
    if(_leaked_deps)
        list(LENGTH _leaked_deps _leaked_count)
        file(REMOVE ${_leaked_deps})
        math(EXPR _pruned_total "${_pruned_total} + ${_leaked_count}")
        message(STATUS "prune_output_dir: 插件目录清掉 ${_leaked_count} 个基础依赖动态库（applocal 拷进来的）")
    endif()
endif()

# ---------------------------------------------------------------------------
# 3. 自检：清理后仍然违约就直接报错，别让坏产物悄悄过关
# ---------------------------------------------------------------------------
set(_violations "")

if(_is_msvc_platform AND IS_DIRECTORY "${APP_OUTPUT_DIR}")
    file(GLOB_RECURSE _left_app_pdb "${APP_OUTPUT_DIR}/*.pdb")
    list(APPEND _violations ${_left_app_pdb})
endif()

if(IS_DIRECTORY "${PLUGINS_OUTPUT_DIR}")
    file(GLOB _left_plugin_all "${PLUGINS_OUTPUT_DIR}/*")
    foreach(_entry IN LISTS _left_plugin_all)
        if(IS_DIRECTORY "${_entry}")
            continue()
        endif()
        get_filename_component(_ext "${_entry}" EXT)
        string(TOLOWER "${_ext}" _ext_lc)
        if(NOT _ext_lc STREQUAL "${_dyn_ext}")
            list(APPEND _violations "${_entry}")
        endif()
    endforeach()
endif()

if(_violations)
    list(REMOVE_DUPLICATES _violations)
    message(FATAL_ERROR "prune_output_dir: 清理后仍存在违规文件：${_violations}")
endif()

message(STATUS "prune_output_dir: 完成（本轮清掉 ${_pruned_total} 个文件）")
