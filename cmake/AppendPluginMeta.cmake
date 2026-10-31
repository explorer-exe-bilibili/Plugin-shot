# ============================================================================
#  cmake/AppendPluginMeta.cmake — 把 metadata.json 贴到插件二进制尾部
#
#  用法（脚本模式，挂在目标 POST_BUILD）：
#    cmake -DTARGET_FILE=<刚链接出来的 dll/so> \\
#          -DMETADATA_FILE=<metadata.json> \\
#          -DCONTRACT_HEADER=<include/plugin_api.h> \\
#          -P cmake/AppendPluginMeta.cmake
#
#  为什么是纯文本标记而不是二进制魔数+长度：
#    CMake 能可靠地 file(APPEND) 文本，但没有可靠的二进制写入原语。
#    用换行包住 START/END 标记，宿主 rfind 一下就能取出 JSON，
#    插件开发者不需要装 Python，也不需要额外的贴尾小工具。
#
#  为什么不做"读回尾部自检"：
#    CMake 的 string 操作在二进制文件里遇到 NUL 会截断（实测：贴尾成功但
#    string(FIND) 找不到标记）。所以这里改用文件大小增量校验。
#    也因此不处理"重复贴尾"：宿主读取用 rfind 取【最后一对】标记，
#    多贴一份只会被后一份覆盖，不会解析错。
#
#  标记字面量不在这里硬编码：从契约头 plugin_api.h 的
#  PLUGIN_META_START_MARKER / PLUGIN_META_END_MARKER 宏读出来，单一事实来源。
# ============================================================================
cmake_minimum_required(VERSION 3.20)

foreach(_required TARGET_FILE METADATA_FILE CONTRACT_HEADER)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "AppendPluginMeta: 缺少必需参数 -D${_required}=<path>")
    endif()
    if(NOT EXISTS "${${_required}}")
        message(FATAL_ERROR "AppendPluginMeta: 文件不存在 ${_required} = ${${_required}}")
    endif()
endforeach()

file(READ "${CONTRACT_HEADER}" _contract)

string(REGEX MATCH "#define[ \t]+PLUGIN_META_START_MARKER[ \t]+\"([^\"]+)\"" _match "${_contract}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "AppendPluginMeta: ${CONTRACT_HEADER} 里找不到 PLUGIN_META_START_MARKER")
endif()
set(_start "${CMAKE_MATCH_1}")

string(REGEX MATCH "#define[ \t]+PLUGIN_META_END_MARKER[ \t]+\"([^\"]+)\"" _match "${_contract}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "AppendPluginMeta: ${CONTRACT_HEADER} 里找不到 PLUGIN_META_END_MARKER")
endif()
set(_end "${CMAKE_MATCH_1}")

file(READ "${METADATA_FILE}" _json)
if(_json STREQUAL "")
    message(FATAL_ERROR "AppendPluginMeta: ${METADATA_FILE} 是空文件")
endif()

# 一次追加，避免多次 file(APPEND) 之间出现半截状态。
set(_payload "\n${_start}\n${_json}\n${_end}\n")
string(LENGTH "${_payload}" _payload_len)

# file(APPEND) 在 Windows 上以文本模式写盘，每个 \n 会变成 \r\n（实测差 23 字节）。
# 标记是纯文本，CRLF 不影响宿主按字符串查找；这里只是让大小校验算得准。
if(WIN32)
    string(REGEX MATCHALL "\n" _newlines "${_payload}")
    list(LENGTH _newlines _newline_count)
    math(EXPR _payload_len "${_payload_len} + ${_newline_count}")
endif()

file(SIZE "${TARGET_FILE}" _size_before)
file(APPEND "${TARGET_FILE}" "${_payload}")
file(SIZE "${TARGET_FILE}" _size_after)

math(EXPR _expected "${_size_before} + ${_payload_len}")
if(NOT _size_after EQUAL _expected)
    message(FATAL_ERROR
        "AppendPluginMeta: 追加失败 —— 期望 ${_expected} 字节，实际 ${_size_after} -> ${TARGET_FILE}")
endif()

message(STATUS "AppendPluginMeta: 已贴元数据 +${_payload_len} 字节 -> ${TARGET_FILE}")
