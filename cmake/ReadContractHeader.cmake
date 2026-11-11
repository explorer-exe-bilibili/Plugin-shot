# ============================================================================
#  cmake/ReadContractHeader.cmake — 读取（已拆分的）契约头全文
#
#  用法（在 cmake -P 脚本里 include 后调用一次）：
#    include("${CMAKE_CURRENT_LIST_DIR}/ReadContractHeader.cmake")
#    shot_read_contract_header(_contract "${CONTRACT_HEADER}")
#
#  为什么要单独抽出来：契约头已经从"一个单文件"拆成
#    include/plugin_api.h        汇总头，只做 include
#    include/plugin_api/*.h      按主题的子头（版本 / interface id / 各能力 …）
#  而 ValidatePluginMeta / AppendPluginMeta 都是【按文本】读契约头，找
#  SHOT_ABI_*、SHOT_IFACE_* 与 PLUGIN_META_* 标记。只 READ 汇总头会一个字都
#  找不到：校验会静默失效（metadata 里拼错的 interface id 不再被拦下），
#  贴尾脚本也会报"契约头里找不到标记"。所以这里必须把汇总头 + 全部子头
#  拼成一份整文再交给调用方。
#
#  只扫单层目录、不用 GLOB_RECURSE：plugin_api/ 下约定不再嵌套子目录，
#  构建脚本、文档与导出布局都按这一层来；递归会把将来可能出现的备份/样例
#  文件也读进来，那反而不再是"契约正文"。
# ============================================================================

function(shot_read_contract_header _out_var _contract_header)
    if(NOT EXISTS "${_contract_header}")
        message(FATAL_ERROR "ReadContractHeader: 契约头不存在 ${_contract_header}")
    endif()

    file(READ "${_contract_header}" _contract_text)

    get_filename_component(_contract_dir "${_contract_header}" DIRECTORY)
    file(GLOB _contract_parts "${_contract_dir}/plugin_api/*.h")
    # 排序只为让日志/调试可复现；拼出来的整文只用于正则查找，顺序不影响结果。
    list(SORT _contract_parts)

    foreach(_part IN LISTS _contract_parts)
        if(IS_DIRECTORY "${_part}")
            continue()
        endif()
        file(READ "${_part}" _part_text)
        # 先用换行隔开再拼：子头若结尾没有换行，直接首尾相接会把上一行注释和
        # 下一个 #define 粘成一行，正则就会漏掉那个宏。
        string(APPEND _contract_text "\n${_part_text}")
    endforeach()

    if(NOT _contract_parts)
        # 汇总头只 include 子头，子头目录空 = 契约正文整个丢了。这里给明确警告，
        # 免得调用方只看到"找不到 SHOT_IFACE_*"这种误导性的下游报错。
        message(WARNING "ReadContractHeader: ${_contract_dir}/plugin_api 下没有子头，契约可能不完整")
    endif()

    set(${_out_var} "${_contract_text}" PARENT_SCOPE)
endfunction()
