# ============================================================================
#  cmake/ValidatePluginMeta.cmake — 构建期校验 metadata.json
#
#  用法（脚本模式，挂在目标 POST_BUILD，排在 AppendPluginMeta 之前）：
#    cmake -DMETADATA_FILE=<metadata.json>
#          -DCONTRACT_HEADER=<include/plugin_api.h>
#          -DTARGET_NAME=<插件目标名>
#          -P cmake/ValidatePluginMeta.cmake
#
#  校验什么（都是"写错了要到宿主运行期才发现"的那类问题，提前到构建期）：
#    1. plugins[] 非空
#    2. 每个 "id" 唯一
#    3. "interfaces" 里的每个 id 都必须在契约头里真实存在（拼错一个字母，
#       宿主预筛就永远认不出这个插件）
#    3b. "requires" 每条都是 name[op version]（op ∈ >= > <= < ==，version 是数字段），
#        "provides" 每条只是名字，且不允许声明 core（core 是宿主自己）
#    4. "abi.major" 与契约头 SHOT_ABI_MAJOR 相等；"abi.minor" 不大于 SHOT_ABI_MINOR
#       （允许用旧 minor 构建的插件继续加载，追加能力不需要全量改 metadata）
#
#  注意：契约头已经拆成 include/plugin_api.h（汇总）+ include/plugin_api/*.h（子头），
#  所以这里的"契约头"指的是 ReadContractHeader.cmake 拼出来的整份文本，不是单个文件。
#
#  刻意不校验 "package"：这个字段已经从 metadata 里删掉了 —— 它和目标 DLL 名重复，
#  而宿主做配置键用的是全局唯一的 plugins[].id（改名、换目录都不受影响）。
#
#  刻意不做：JSON 语法校验。CMake 没有 JSON 解析器，硬写正则解析器既脆又长；
#  语法错误由宿主读取时报告，这里只抓"语义漂移"。
# ============================================================================
cmake_minimum_required(VERSION 3.20)

foreach(_required METADATA_FILE CONTRACT_HEADER TARGET_NAME)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "ValidatePluginMeta: 缺少必需参数 -D${_required}=<value>")
    endif()
endforeach()
if(NOT EXISTS "${METADATA_FILE}")
    message(FATAL_ERROR "ValidatePluginMeta: 元数据文件不存在 ${METADATA_FILE}")
endif()
if(NOT EXISTS "${CONTRACT_HEADER}")
    message(FATAL_ERROR "ValidatePluginMeta: 契约头不存在 ${CONTRACT_HEADER}")
endif()

# 契约头已拆分：汇总头 plugin_api.h 只 include，正文在 include/plugin_api/*.h。
# 必须读【整份】契约文本，只 READ 汇总头会让下面的 SHOT_ABI_* / SHOT_IFACE_* 查找
# 全部落空（第 3 步会把每个 interface 都报成"契约里没有"，等于校验坏了）。
include("${CMAKE_CURRENT_LIST_DIR}/ReadContractHeader.cmake")
shot_read_contract_header(_contract "${CONTRACT_HEADER}")
file(READ "${METADATA_FILE}" _json)

set(_problems "")

# ---------------------------------------------------------------------------
# 契约头里的事实：ABI 版本 + 已注册的 interface id
# ---------------------------------------------------------------------------
foreach(_var SHOT_ABI_MAJOR SHOT_ABI_MINOR)
    string(REGEX MATCH "#define[ \t]+${_var}[ \t]+([0-9]+)" _m "${_contract}")
    # 不能用 if(NOT CMAKE_MATCH_1) 判断"没匹配到"：CMake 把字符串 "0" 当 FALSE，
    # SHOT_ABI_MINOR 为 0 时会误报"契约头里找不到"。以 MATCH 的整段结果判断匹配与否。
    if(NOT _m)
        message(FATAL_ERROR "ValidatePluginMeta: 契约头里找不到 ${_var}")
    endif()
    set(_contract_${_var} "${CMAKE_MATCH_1}")
endforeach()

string(REGEX MATCHALL "#define[ \t]+SHOT_IFACE_[A-Za-z0-9_]+[ \t]+\"([^\"]+)\"" _iface_defs "${_contract}")
set(_known_ifaces "")
foreach(_def IN LISTS _iface_defs)
    string(REGEX REPLACE "^.*\"([^\"]+)\".*$" "\\1" _value "${_def}")
    list(APPEND _known_ifaces "${_value}")
endforeach()
list(LENGTH _known_ifaces _known_count)
if(_known_count EQUAL 0)
    message(FATAL_ERROR "ValidatePluginMeta: 契约头里找不到任何 SHOT_IFACE_* 定义")
endif()

# ---------------------------------------------------------------------------
# 1. abi.major 相等且 abi.minor 不超过契约头
# ---------------------------------------------------------------------------
string(REGEX MATCH "\"abi\"[ \t]*:[ \t]*{[^}]*}" _abi_block "${_json}")
if(NOT _abi_block)
    list(APPEND _problems "缺少 abi 对象")
else()
    string(REGEX MATCH "\"major\"[ \t]*:[ \t]*([0-9]+)" _m "${_abi_block}")
    set(_abi_major "${CMAKE_MATCH_1}")
    string(REGEX MATCH "\"minor\"[ \t]*:[ \t]*([0-9]+)" _m "${_abi_block}")
    set(_abi_minor "${CMAKE_MATCH_1}")
    if(NOT "${_abi_major}" STREQUAL "${_contract_SHOT_ABI_MAJOR}")
        list(APPEND _problems "abi.major(${_abi_major}) 与契约头(${_contract_SHOT_ABI_MAJOR}) 不一致")
    endif()
    if("${_abi_minor}" GREATER "${_contract_SHOT_ABI_MINOR}")
        list(APPEND _problems "abi.minor(${_abi_minor}) 比契约头(${_contract_SHOT_ABI_MINOR}) 新，需要重新构建插件")
    endif()
endif()

# ---------------------------------------------------------------------------
# 2. plugins[] 非空，且每个 id 唯一
# ---------------------------------------------------------------------------
string(REGEX MATCHALL "\"id\"[ \t]*:[ \t]*\"([^\"]+)\"" _id_matches "${_json}")
list(LENGTH _id_matches _id_count)
if(_id_count EQUAL 0)
    list(APPEND _problems "plugins[] 里一个 id 都没有")
endif()

set(_ids "")
foreach(_match IN LISTS _id_matches)
    string(REGEX REPLACE "^.*\"([^\"]+)\".*$" "\\1" _id "${_match}")
    list(FIND _ids "${_id}" _dup)
    if(NOT _dup EQUAL -1)
        list(APPEND _problems "id(${_id}) 重复")
    endif()
    list(APPEND _ids "${_id}")
endforeach()

# ---------------------------------------------------------------------------
# 3. interfaces 里的每个 id 都必须在契约头里定义过
# ---------------------------------------------------------------------------
string(REGEX MATCHALL "\"interfaces\"[ \t]*:[ \t]*[[]([^]]*)[]]" _iface_blocks "${_json}")
foreach(_block IN LISTS _iface_blocks)
    string(REGEX MATCHALL "\"[^\"]+\"" _quoted "${_block}")
    foreach(_q IN LISTS _quoted)
        string(REGEX REPLACE "^\"([^\"]+)\".*$" "\\1" _name "${_q}")
        if(_name STREQUAL "interfaces")
            continue()  # 匹配到的是键名本身，不是能力 id
        endif()
        list(FIND _known_ifaces "${_name}" _hit)
        if(_hit EQUAL -1)
            list(APPEND _problems "interface(${_name}) 在契约头里没有定义")
        endif()
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# 3b. requires / provides 语法
# ---------------------------------------------------------------------------
string(REGEX MATCHALL "\"requires\"[ \t]*:[ \t]*[[]([^]]*)[]]" _req_blocks "${_json}")
foreach(_block IN LISTS _req_blocks)
    string(REGEX MATCHALL "\"[^\"]+\"" _quoted "${_block}")
    foreach(_q IN LISTS _quoted)
        string(REGEX REPLACE "^\"([^\"]+)\".*$" "\\1" _text "${_q}")
        if(_text STREQUAL "requires")
            continue()
        endif()
        if(NOT _text MATCHES "^[A-Za-z0-9_.-]+$" AND
           NOT _text MATCHES "^[A-Za-z0-9_.-]+(>=|<=|==|>|<)[0-9]+(\\.[0-9]+)*$")
            list(APPEND _problems "requires(\"\"${_text}\"\") 语法不对，应为 name 或 name>=x.y.z")
        endif()
    endforeach()
endforeach()

string(REGEX MATCHALL "\"provides\"[ \t]*:[ \t]*[[]([^]]*)[]]" _prov_blocks "${_json}")
foreach(_block IN LISTS _prov_blocks)
    string(REGEX MATCHALL "\"[^\"]+\"" _quoted "${_block}")
    foreach(_q IN LISTS _quoted)
        string(REGEX REPLACE "^\"([^\"]+)\".*$" "\\1" _text "${_q}")
        if(_text STREQUAL "provides")
            continue()
        endif()
        if(NOT _text MATCHES "^[A-Za-z0-9_.-]+$")
            list(APPEND _problems "provides(\"\"${_text}\"\") 只能是名字，不带版本")
        endif()
        if(_text STREQUAL "core")
            list(APPEND _problems "provides 里不能写 core（core 由宿主提供）")
        endif()
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# 结论
# ---------------------------------------------------------------------------
if(_problems)
    list(REMOVE_DUPLICATES _problems)
    string(REPLACE ";" "\n  - " _pretty "${_problems}")
    message(FATAL_ERROR "ValidatePluginMeta: ${METADATA_FILE} 校验失败（${TARGET_NAME}）：\n  - ${_pretty}\n已知能力：${_known_ifaces}")
endif()

message(STATUS "ValidatePluginMeta: ${TARGET_NAME} 元数据通过（abi ${_contract_SHOT_ABI_MAJOR}.${_contract_SHOT_ABI_MINOR}，${_id_count} 个插件，已知能力 ${_known_count} 个）")
