# ============================================================================
#  tools/plugin_sdk/new_plugin.cmake — 从模板生成插件工程（纯 CMake 脚本）
#
#  为什么是 cmake -P 而不是 Python / PowerShell：
#    离线 SDK 的承诺是"只需要 CMake + Ninja + MSVC"。Python 不保证存在；
#    PowerShell 还受 ExecutionPolicy 限制（AGENTS §7-15）。占位符替换用
#    string(REPLACE) 就够，不需要第三方运行时。
#
#  用法（导出后的路径是 <SDK>/tools/new_plugin.cmake）：
#    cmake -DTEMPLATE=generic                      \
#          -DDEST=D:/work/acme_foo                 \
#          -DPLUGIN_ID=acme.foo                    \
#          -DPLUGIN_NAME="ACME Foo"                \
#          -DPLUGIN_DESCRIPTION="逐像素滤镜示例"    \
#          -DVERSION=1.0.0                         \
#          -DOWNER="ACME"                          \
#          -P <SDK>/tools/new_plugin.cmake
#
#  参数：
#    TEMPLATE            模板名（<templates>/ 下的子目录）或模板的绝对路径；必填
#    DEST                生成目录；必填；已存在且非空时报错（-DFORCE=ON 才覆盖）
#    PLUGIN_ID           插件 id（全局唯一，建议 domain.name 形式）；必填
#    PLUGIN_NAME         显示名；缺省 = PLUGIN_ID
#    PLUGIN_DESCRIPTION  描述；缺省空串
#    VERSION             插件版本；缺省 1.0.0
#    OWNER               作者 / 组织；缺省空串
#    TEMPLATES_DIR       模板根目录；缺省自动探测（仓库布局 <script>/templates，
#                        SDK 布局 <script>/../templates）
#    FORCE               ON 时允许写入非空 DEST
#
#  占位符（模板作者在【文件内容】与【文件名】里都可以写）：
#    {{TARGET}} {{PLUGIN_ID}} {{PLUGIN_NAME}} {{PLUGIN_DESCRIPTION}} {{VERSION}} {{OWNER}}
#  另外把任何剩下的 {{NAME}} 通配替换成空串：模板多写了本脚本不认识的占位符
#  也不会导致生成失败（约定见 document/README_SDK.md 的模板一节）。
#
#  故意不在这里校验 metadata.json 的 abi：模板作者负责写正确值，导出脚本与文档
#  会说明规则；真写错了，shot_add_plugin() 的 POST_BUILD 校验会立刻报错，
#  不需要脚手架重复实现一遍。
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# 1. 参数与缺省值
# ---------------------------------------------------------------------------
foreach(_required TEMPLATE DEST PLUGIN_ID)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "new_plugin: 缺少必需参数 -D${_required}=<value>")
    endif()
endforeach()

if(NOT DEFINED PLUGIN_NAME OR "${PLUGIN_NAME}" STREQUAL "")
    set(PLUGIN_NAME "${PLUGIN_ID}")
endif()
if(NOT DEFINED PLUGIN_DESCRIPTION)
    set(PLUGIN_DESCRIPTION "")
endif()
if(NOT DEFINED VERSION OR "${VERSION}" STREQUAL "")
    set(VERSION "1.0.0")
endif()
if(NOT DEFINED OWNER)
    set(OWNER "")
endif()

# CMake 目标名 / 目录名里 '.' 与 '-' 不合法（或者会生成别扭的文件名），统一换成
# 下划线：acme.foo -> acme_foo。PLUGIN_ID 本身保持原样写进 metadata 与代码。
string(REPLACE "." "_" PLUGIN_TARGET "${PLUGIN_ID}")
string(REPLACE "-" "_" PLUGIN_TARGET "${PLUGIN_TARGET}")
string(TOLOWER "${PLUGIN_TARGET}" PLUGIN_TARGET)

# 不做硬校验：宿主只要求 id 全局唯一，历史插件也有不带点的 id。这里只提示，
# 免得把合法的老式 id 拦在门外（真正的 id 规则由宿主/文档保证）。
if(NOT PLUGIN_ID MATCHES "^[A-Za-z0-9_.-]+$")
    message(WARNING "new_plugin: PLUGIN_ID=${PLUGIN_ID} 含非常规字符（建议只保留字母、数字、_ . -）")
endif()

# ---------------------------------------------------------------------------
# 2. 定位模板（支持仓库布局与 SDK 布局，也支持直接给绝对路径）
# ---------------------------------------------------------------------------
set(_script_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_template_roots "")
if(DEFINED TEMPLATES_DIR AND NOT "${TEMPLATES_DIR}" STREQUAL "")
    list(APPEND _template_roots "${TEMPLATES_DIR}")
endif()
list(APPEND _template_roots
    "${_script_dir}/templates"        # 仓库布局：tools/plugin_sdk/templates
    "${_script_dir}/../templates"     # SDK 布局：<SDK>/templates
    "${_script_dir}/../../templates")

set(_tpl "")
if(IS_ABSOLUTE "${TEMPLATE}")
    if(IS_DIRECTORY "${TEMPLATE}")
        set(_tpl "${TEMPLATE}")
    endif()
else()
    foreach(_root IN LISTS _template_roots)
        if(IS_DIRECTORY "${_root}/${TEMPLATE}")
            set(_tpl "${_root}/${TEMPLATE}")
            break()
        endif()
    endforeach()
endif()

if(NOT _tpl)
    # 把可用的模板名列出来，用户不用去翻目录。
    set(_available "")
    foreach(_root IN LISTS _template_roots)
        if(IS_DIRECTORY "${_root}")
            file(GLOB _candidate_dirs LIST_DIRECTORIES true "${_root}/*")
            foreach(_candidate IN LISTS _candidate_dirs)
                if(IS_DIRECTORY "${_candidate}")
                    get_filename_component(_candidate_name "${_candidate}" NAME)
                    list(APPEND _available "${_candidate_name}")
                endif()
            endforeach()
        endif()
    endforeach()
    if(_available)
        list(REMOVE_DUPLICATES _available)
        list(SORT _available)
        set(_available_hint "；可用模板：${_available}")
    else()
        set(_available_hint "；没有找到任何模板目录（检查 -DTEMPLATES_DIR=）")
    endif()
    message(FATAL_ERROR "new_plugin: 找不到模板 ${TEMPLATE}${_available_hint}")
endif()

# 相对路径统一转绝对路径。原因：第 4 节的
#   file(GLOB_RECURSE ... LIST_DIRECTORIES false RELATIVE "${DEST}" "${DEST}/*")
# 在 DEST 是相对路径时（GLOB_RECURSE + 相对 pattern + RELATIVE 组合）会返回空列表，
# 占位符替换循环一次都不跑 —— 表现是"报告已生成 N 个文件，但 {{TARGET}} 没被替换"。
# 相对路径按"执行 cmake 的当前工作目录"解析（-P 脚本模式下 CMAKE_CURRENT_BINARY_DIR
# 就是当前工作目录），而不是按脚本所在目录。
if(NOT IS_ABSOLUTE "${DEST}")
    get_filename_component(DEST "${DEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()

# ---------------------------------------------------------------------------
# 3. 目标目录检查与拷贝
# ---------------------------------------------------------------------------
if(EXISTS "${DEST}")
    if(NOT IS_DIRECTORY "${DEST}")
        message(FATAL_ERROR "new_plugin: DEST=${DEST} 已存在但不是目录")
    endif()
    file(GLOB _dest_entries "${DEST}/*")
    list(LENGTH _dest_entries _dest_entry_count)
    if(_dest_entry_count GREATER 0 AND NOT FORCE)
        message(FATAL_ERROR
            "new_plugin: DEST=${DEST} 已存在且非空；确认要覆盖请加 -DFORCE=ON")
    endif()
endif()
file(MAKE_DIRECTORY "${DEST}")

# 用 cmake -E copy_directory（逐字节复制，不会像 file(COPY) 那样尝试改权限；
# 后者在权限受限的目录里会直接失败，导出脚本注释里记录过这个坑）。
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_tpl}" "${DEST}"
    RESULT_VARIABLE _copy_rc
    OUTPUT_QUIET)
if(NOT _copy_rc EQUAL 0)
    message(FATAL_ERROR "new_plugin: 拷贝模板失败（${_tpl} -> ${DEST}），exit ${_copy_rc}")
endif()

# ---------------------------------------------------------------------------
# 4. 占位符替换
# ---------------------------------------------------------------------------
set(_ph_keys
    "{{TARGET}}" "{{PLUGIN_ID}}" "{{PLUGIN_NAME}}"
    "{{PLUGIN_DESCRIPTION}}" "{{VERSION}}" "{{OWNER}}")
set(_ph_vals
    "${PLUGIN_TARGET}" "${PLUGIN_ID}" "${PLUGIN_NAME}"
    "${PLUGIN_DESCRIPTION}" "${VERSION}" "${OWNER}")

# 把一段文本里的已知占位符换掉，再清掉不认识的 {{XXX}}（通配，替换成空串）。
function(_new_plugin_subst _out_var _text)
    set(_result "${_text}")
    foreach(_key IN LISTS _ph_keys)
        list(FIND _ph_keys "${_key}" _index)
        list(GET _ph_vals ${_index} _value)
        string(REPLACE "${_key}" "${_value}" _result "${_result}")
    endforeach()
    string(REGEX REPLACE "\\{\\{[A-Za-z0-9_]+\\}\\}" "" _result "${_result}")
    set(${_out_var} "${_result}" PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE _gen_files LIST_DIRECTORIES false RELATIVE "${DEST}" "${DEST}/*")
foreach(_rel IN LISTS _gen_files)
    set(_abs "${DEST}/${_rel}")

    # 4.1 文件内容：只重写真正含占位符的文件。二进制资源（图片等）不会被改写；
    #     无占位符的文本也不会因为 file(WRITE) 在 Windows 上把 LF 变成 CRLF。
    file(READ "${_abs}" _content)
    _new_plugin_subst(_new_content "${_content}")
    if(NOT _new_content STREQUAL _content)
        file(WRITE "${_abs}" "${_new_content}")
    endif()

    # 4.2 文件名 / 目录名里的占位符（例：src/{{TARGET}}_plugin.cpp）。
    _new_plugin_subst(_new_rel "${_rel}")
    if(NOT _new_rel STREQUAL _rel)
        get_filename_component(_new_dir "${_new_rel}" DIRECTORY)
        if(_new_dir)
            file(MAKE_DIRECTORY "${DEST}/${_new_dir}")
        endif()
        file(RENAME "${_abs}" "${DEST}/${_new_rel}")
    endif()
endforeach()

# 4.3 目录名含占位符时，按文件重命名后原目录会留下空壳；从深到浅删掉空目录。
file(GLOB_RECURSE _gen_dirs LIST_DIRECTORIES true "${DEST}/*")
if(_gen_dirs)
    list(SORT _gen_dirs)
    list(REVERSE _gen_dirs)
    foreach(_dir IN LISTS _gen_dirs)
        if(NOT IS_DIRECTORY "${_dir}")
            continue()
        endif()
        file(GLOB _dir_entries "${_dir}/*")
        list(LENGTH _dir_entries _dir_entry_count)
        if(_dir_entry_count EQUAL 0)
            file(REMOVE_RECURSE "${_dir}")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 5. 结果
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _final_files LIST_DIRECTORIES false "${DEST}/*")
list(LENGTH _final_files _final_count)
message(STATUS "new_plugin: 模板 ${_tpl}")
message(STATUS "new_plugin: 已生成 ${_final_count} 个文件 -> ${DEST}")
message(STATUS "new_plugin: target=${PLUGIN_TARGET} id=${PLUGIN_ID} name=${PLUGIN_NAME} version=${VERSION}")
message(STATUS "new_plugin: 下一步：cmake -S \"${DEST}\" -B \"${DEST}/build\" -G Ninja -DCMAKE_BUILD_TYPE=Release")
message(STATUS "new_plugin: 构建脚本会自动 link/pack，产物在 <build>/bin/plugins/")
