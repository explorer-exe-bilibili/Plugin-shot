# ============================================================================
#  cmake/export_sdk.cmake — 导出离线插件 SDK（核心包 + 可选 OpenCV 叠加包）
#
#  用法（脚本模式，由根 CMakeLists.txt 的 export_sdk 目标调用）：
#    cmake -DVCPKG_BIN_DIR=<vcpkg bin>            \
#          -DVCPKG_LIB_DIR=<vcpkg lib>            \
#          -DVCPKG_INC_DIR=<vcpkg include>        \
#          -DCONTRACT_INCLUDE_DIR=<源码 include/> \
#          -DSHOT_CMAKE_DIR=<源码 cmake/>         \
#          -DSDK_OUTPUT_DIR=<仓库 sdk/>           \
#          -DSDK_VERSION=0.1.0                    \
#          -DPLATFORM=win64 -DARCH=x64            \
#          [-DWITH_OPENCV=ON]                     \
#          [-DHOST_MIN_VERSION=0.1.0]             \
#          [-DVCPKG_DEBUG_BIN_DIR=...] [-DVCPKG_DEBUG_LIB_DIR=...] \
#          [-DPLUGIN_TEMPLATES_DIR=...] [-DPLUGIN_DEMOS_DIR=...]
#          [-DSKILL_DIR=...]
#          [-DLICENSES_DIR=...] [-DNEW_PLUGIN_SCRIPT=...]         \
#          [-DREPO_ROOT=...]                      \
#          -P cmake/export_sdk.cmake
#
#  产物布局（sdk/ 每次【整树重建】，杜绝上一次导出的残留）：
#    sdk/manifest.json                 机器可读清单：版本 / ABI / 契约哈希 / 文件哈希
#    sdk/README.md                     由 document/README_SDK.md 拷贝（单一事实来源）
#    sdk/LICENSE  sdk/NOTICE           仓库根的同名文件（NOTICE 缺失只 WARN）
#    sdk/THIRD_PARTY_LICENSES/         nlohmann-json.MIT.txt / stb.LICENSE
#    sdk/include/                      契约头 + 插件侧工具头（【不含】opencv4）
#    sdk/third_party/include/          nlohmann/ 整树 + stb_*.h（扁平）
#    sdk/cmake/                        插件构建模块（ShotPlugin 等 5 个）
#    sdk/docs/                         从 document/ 抽取的插件作者文档
#    sdk/templates/                    工程模板（由文档/模板线维护，缺失只 WARN）
#    sdk/demos/                        完整示例工程 watermark / archive（缺失只 WARN）
#    sdk/skill/plugin-shot-dev/        DSH skill（缺失只 WARN）
#    sdk/tools/new_plugin.cmake        脚手架脚本
#  可选叠加包（-DWITH_OPENCV=ON 时才导出）：
#    sdk/opencv/manifest.json          {kind:"opencv", version, abi}
#    sdk/opencv/include/opencv4/**     OpenCV 头（保持子目录结构）
#    sdk/opencv/lib{/debug}/*.lib      白名单内的导入库
#    sdk/opencv/bin{/debug}/*.dll      白名单内的运行时 DLL
#
#  为什么拆"核心包 / OpenCV 叠加包"（用户已拍板）：
#    大多数插件（Generic / 配置 / 订阅者）根本不需要 OpenCV，却被迫下载 58 MB。
#    核心包只含契约头 + 第三方头 + 构建模块（几 MB）；OpenCV 头/库/DLL 单独一个
#    zip，需要的人把 opencv/ 目录叠加到核心 SDK 根即可（见 document/README_SDK.md）。
#
#  几条硬约束（都是实测踩过的坑，不要"优化"掉）：
#    · nlohmann 必须【整树】拷贝：vcpkg 的 json.hpp 是多文件入口，只拷它一个会
#      报 C1083: 无法打开 "nlohmann/adl_serializer.hpp"（实测 E1b）。
#    · stb 必须是【扁平文件名】：插件源码写 #include "stb_image_write.h"，放进
#      stb/ 子目录会找不到（vcpkg 的 include/ 就是扁平布局）。
#    · 宿主私有依赖（fmt / spdlog / glfw3dll / imgui / nfd）不进入 SDK：整目录
#      dump 等于把它们变成对外契约（见下方「SDK 白名单」）。
#    · debug/release 同名文件必须分目录（lib/debug、bin/debug），绝不能平铺。
#    · 不能用 file(COPY)：它会把源权限 chmod 到目标，权限受限环境直接失败
#      （历史坑，见 configure_file 的注释）。
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
#  0. 通用小工具
# ---------------------------------------------------------------------------

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

# 保留相对目录结构地拷贝（用于 opencv4 / nlohmann / plugin_api 这类头文件树）
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

# 整目录拷贝（用 cmake -E copy_directory：逐字节、不改权限、保留目录结构）。
function(_copy_tree_dir _src _dest)
    if(NOT IS_DIRECTORY "${_src}")
        return()
    endif()
    file(MAKE_DIRECTORY "${_dest}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_src}" "${_dest}"
        RESULT_VARIABLE _copy_rc
        OUTPUT_QUIET)
    if(NOT _copy_rc EQUAL 0)
        message(FATAL_ERROR "export_sdk: 目录拷贝失败 ${_src} -> ${_dest}（exit ${_copy_rc}）")
    endif()
endfunction()

# 统计目录下文件数与总字节数（用于最终汇报，不参与门禁）。
function(_tree_stats _dir _out_count _out_bytes)
    set(_count 0)
    set(_bytes 0)
    if(IS_DIRECTORY "${_dir}")
        file(GLOB_RECURSE _files LIST_DIRECTORIES false "${_dir}/*")
        foreach(_file IN LISTS _files)
            file(SIZE "${_file}" _size)
            math(EXPR _count "${_count} + 1")
            math(EXPR _bytes "${_bytes} + ${_size}")
        endforeach()
    endif()
    set(${_out_count} "${_count}" PARENT_SCOPE)
    set(${_out_bytes} "${_bytes}" PARENT_SCOPE)
endfunction()

# 收集 manifest.files[]：对给定文件/子目录（相对 _root）算相对路径 + sha256，
# 输出可直接拼进 JSON 的对象字符串列表。
# 只覆盖 include/cmake/third_party 等"小而关键"的子树；opencv 叠加包有自己的
# manifest，文件名通配与二进制哈希都放那边，核心 manifest 保持在几十 KB 量级。
function(_collect_file_hashes _out_var _root)
    set(_items "")
    foreach(_sub IN LISTS ARGN)
        if(NOT EXISTS "${_root}/${_sub}")
            continue()
        endif()
        if(IS_DIRECTORY "${_root}/${_sub}")
            file(GLOB_RECURSE _files "${_root}/${_sub}/*")
        else()
            set(_files "${_root}/${_sub}")
        endif()
        foreach(_file IN LISTS _files)
            if(IS_DIRECTORY "${_file}")
                continue()
            endif()
            file(RELATIVE_PATH _rel "${_root}" "${_file}")
            # 清单里的路径统一用 '/', 与 zip 内路径、跨平台阅读习惯一致。
            string(REPLACE "\\" "/" _rel "${_rel}")
            file(SHA256 "${_file}" _sha)
            list(APPEND _items "    {\"path\": \"${_rel}\", \"sha256\": \"${_sha}\"}")
        endforeach()
    endforeach()
    set(${_out_var} "${_items}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
#  白名单：SDK 只承诺 OpenCV 及其图像编解码 / 压缩传递依赖
#
#  为什么不能整目录拷贝：vcpkg_installed/<triplet>/lib|bin 里同时躺着宿主私有
#  依赖（fmt / spdlog / glfw3dll / imgui / nfd）。整目录 dump 等于把它们写进
#  SDK 的对外契约——第三方照着链接宿主的内部实现，宿主一升级就断；SDK 的
#  bin/ 也会被塞进一堆与插件无关的 DLL。白名单让"SDK 能力"只等于
#  shot.lib_provider 真正带来的那一套（OpenCV + opencv_imgcodecs 落地时
#  必需的编解码/压缩库），其余一概不进 SDK。
#
#  正则匹配"完整文件名"，release / debug 的命名差异（opencv_*4d、libpng16d、
#  tiffd、zd）都写在同一张表里，所以两个变体共用一份白名单。
#  白名单以当前 sdk/opencv/lib、sdk/opencv/bin 的实测清单为准（2026-11 复核）：
#    lib: jpeg / libpng16 / tiff / z / lzma / libwebp* / libsharpyuv / turbojpeg
#    dll: jpeg62 / libpng16 / tiff / z / liblzma / libwebp* / libsharpyuv / turbojpeg
# ---------------------------------------------------------------------------
set(_SDK_LIB_WHITELIST
    "^opencv_[A-Za-z0-9]+\\.lib$"   # OpenCV 模块：opencv_core4.lib / opencv_core4d.lib
    "^jpeg\\.lib$"                  # libjpeg-turbo 的 jpeg 兼容库
    "^turbojpeg\\.lib$"
    "^libpng16d?\\.lib$"            # PNG：release libpng16.lib / debug libpng16d.lib
    "^tiffd?\\.lib$"
    "^zd?\\.lib$"                   # zlib：release z.lib / debug zd.lib
    "^lzma\\.lib$"
    "^libwebp[a-z]*\\.lib$"         # libwebp / decoder / demux / mux
    "^libsharpyuv\\.lib$"
)

set(_SDK_DLL_WHITELIST
    "^opencv_[A-Za-z0-9]+\\.dll$"
    "^jpeg[0-9]*\\.dll$"            # 实际文件名是 jpeg62.dll
    "^turbojpeg\\.dll$"
    "^libpng16d?\\.dll$"
    "^tiffd?\\.dll$"
    "^zd?\\.dll$"
    "^liblzma\\.dll$"               # 导入库叫 lzma.lib，DLL 叫 liblzma.dll
    "^libwebp[a-z]*\\.dll$"
    "^libsharpyuv\\.dll$"
)

set(_SDK_LIB_WHITELIST_DESC
    "opencv_* / jpeg / turbojpeg / libpng16 / tiff / z / lzma / libwebp* / libsharpyuv")
set(_SDK_DLL_WHITELIST_DESC
    "opencv_* / jpeg* / turbojpeg / libpng16 / tiff / z / liblzma / libwebp* / libsharpyuv")

message(STATUS "export_sdk: SDK 导入库白名单 = ${_SDK_LIB_WHITELIST_DESC}")
message(STATUS "export_sdk: SDK 运行时白名单 = ${_SDK_DLL_WHITELIST_DESC}")
message(STATUS "export_sdk: 宿主私有依赖不入 SDK：fmt / spdlog / glfw3dll / imgui / nfd")

# 按白名单正则过滤文件列表：命中项与未命中项分别写入两个输出变量。
function(_filter_whitelist _out_kept _out_dropped _patterns)
    set(_kept "")
    set(_dropped "")
    foreach(_src_file IN LISTS ARGN)
        get_filename_component(_src_name "${_src_file}" NAME)
        set(_matched FALSE)
        foreach(_pattern IN LISTS _patterns)
            if(_src_name MATCHES "${_pattern}")
                set(_matched TRUE)
                break()
            endif()
        endforeach()
        if(_matched)
            list(APPEND _kept "${_src_file}")
        else()
            list(APPEND _dropped "${_src_file}")
        endif()
    endforeach()
    set(${_out_kept} "${_kept}" PARENT_SCOPE)
    set(${_out_dropped} "${_dropped}" PARENT_SCOPE)
endfunction()

# 白名单过滤 + 平铺拷贝（沿用 _copy_files_flat 的逐文件风格）。
# 输出：命中数量与被排除的文件列表；日志由调用方打印，便于发现 vcpkg 新增依赖。
function(_copy_whitelisted _src_dir _dest _ext _patterns _out_count _out_dropped)
    file(GLOB _candidates "${_src_dir}/*${_ext}")
    if(_candidates)
        _filter_whitelist(_matched _dropped "${_patterns}" ${_candidates})
    else()
        set(_matched "")
        set(_dropped "")
    endif()
    if(_matched)
        _copy_files_flat("${_dest}" ${_matched})
    endif()
    list(LENGTH _matched _count)
    set(${_out_count} "${_count}" PARENT_SCOPE)
    set(${_out_dropped} "${_dropped}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
#  1. 参数：必需项 + 新增可选项（缺目录只 WARN，不失败）
# ---------------------------------------------------------------------------
foreach(_required VCPKG_BIN_DIR VCPKG_LIB_DIR VCPKG_INC_DIR CONTRACT_INCLUDE_DIR
        SDK_OUTPUT_DIR SHOT_CMAKE_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "export_sdk: 缺少必需参数 -D${_required}=<path>")
    endif()
endforeach()

foreach(_required_dir VCPKG_BIN_DIR VCPKG_LIB_DIR VCPKG_INC_DIR CONTRACT_INCLUDE_DIR
        SHOT_CMAKE_DIR)
    if(NOT IS_DIRECTORY "${${_required_dir}}")
        message(FATAL_ERROR "export_sdk: 目录不存在 ${_required_dir} = ${${_required_dir}}")
    endif()
endforeach()

get_filename_component(_repo_root "${SHOT_CMAKE_DIR}/.." ABSOLUTE)
if(DEFINED REPO_ROOT AND NOT "${REPO_ROOT}" STREQUAL "")
    get_filename_component(_repo_root "${REPO_ROOT}" ABSOLUTE)
endif()

# debug 变体目录可以从 vcpkg 目录结构推出来（vcpkg_installed/<triplet>/debug/lib），
# 传了就尊重传入值；没传时推导，避免脚本在独立使用时因为少一个 -D 直接失败。
if(NOT DEFINED VCPKG_DEBUG_LIB_DIR OR "${VCPKG_DEBUG_LIB_DIR}" STREQUAL "")
    get_filename_component(VCPKG_DEBUG_LIB_DIR "${VCPKG_LIB_DIR}/../debug/lib" ABSOLUTE)
endif()
if(NOT DEFINED VCPKG_DEBUG_BIN_DIR OR "${VCPKG_DEBUG_BIN_DIR}" STREQUAL "")
    get_filename_component(VCPKG_DEBUG_BIN_DIR "${VCPKG_BIN_DIR}/../debug/bin" ABSOLUTE)
endif()

if(NOT DEFINED SDK_VERSION OR "${SDK_VERSION}" STREQUAL "")
    set(SDK_VERSION "0.0.0")
endif()
if(NOT DEFINED HOST_MIN_VERSION OR "${HOST_MIN_VERSION}" STREQUAL "")
    set(HOST_MIN_VERSION "${SDK_VERSION}")
endif()
if(NOT DEFINED PLATFORM OR "${PLATFORM}" STREQUAL "")
    set(PLATFORM "win64")
endif()
if(NOT DEFINED ARCH OR "${ARCH}" STREQUAL "")
    set(ARCH "x64")
endif()
if(NOT DEFINED WITH_OPENCV)
    set(WITH_OPENCV OFF)
endif()
if(NOT DEFINED PLUGIN_TEMPLATES_DIR OR "${PLUGIN_TEMPLATES_DIR}" STREQUAL "")
    set(PLUGIN_TEMPLATES_DIR "${_repo_root}/tools/plugin_sdk/templates")
endif()
if(NOT DEFINED SKILL_DIR OR "${SKILL_DIR}" STREQUAL "")
    set(SKILL_DIR "${_repo_root}/skills/plugin-shot-dev")
endif()
if(NOT DEFINED LICENSES_DIR OR "${LICENSES_DIR}" STREQUAL "")
    set(LICENSES_DIR "${_repo_root}/tools/plugin_sdk/licenses")
endif()
if(NOT DEFINED NEW_PLUGIN_SCRIPT OR "${NEW_PLUGIN_SCRIPT}" STREQUAL "")
    set(NEW_PLUGIN_SCRIPT "${_repo_root}/tools/plugin_sdk/new_plugin.cmake")
endif()

# 删除目标目录前先做防呆：SDK_OUTPUT_DIR 必须是明确的子目录，不能是仓库根、
# 盘符根之类；这个脚本会 file(REMOVE_RECURSE)，路径传错就是灾难。
get_filename_component(_sdk_out_abs "${SDK_OUTPUT_DIR}" ABSOLUTE)
get_filename_component(_repo_root_abs "${_repo_root}" ABSOLUTE)
if("${_sdk_out_abs}" STREQUAL "${_repo_root_abs}" OR
   "${_sdk_out_abs}" STREQUAL "/" OR "${_sdk_out_abs}" MATCHES "^[A-Za-z]:/$")
    message(FATAL_ERROR "export_sdk: SDK_OUTPUT_DIR=${SDK_OUTPUT_DIR} 指向仓库根/盘符根，拒绝删除")
endif()

# ---------------------------------------------------------------------------
#  2. 契约事实：ABI 版本 + 契约头整份文本的 sha256
#
#  contract_sha256 = ReadContractHeader 拼出来的"汇总头 + 全部子头"整份文本的
#  SHA256。自检工具（将来）据此判断"SDK 头是不是被改过/新旧混用"。
#  TODO: 把同一算法接进 SDK 自检工具 tools/sdk_check.cmake（本脚本只负责生成）。
# ---------------------------------------------------------------------------
include("${SHOT_CMAKE_DIR}/ReadContractHeader.cmake")
shot_read_contract_header(_contract_text "${CONTRACT_INCLUDE_DIR}/plugin_api.h")

set(_abi_vars "")
foreach(_abi_var SHOT_ABI_MAJOR SHOT_ABI_MINOR)
    string(REGEX MATCH "#define[ \t]+${_abi_var}[ \t]+([0-9]+)" _abi_match "${_contract_text}")
    # 不能用 if(NOT CMAKE_MATCH_1) 判断"没匹配到"：CMake 把字符串 "0" 当假，
    # SHOT_ABI_MINOR 为 0 时会误报（AGENTS §7-9）。以 MATCH 的整段结果为准。
    if(NOT _abi_match)
        message(FATAL_ERROR "export_sdk: 契约头里找不到 ${_abi_var}")
    endif()
    set(_${_abi_var} "${CMAKE_MATCH_1}")
endforeach()
if(_SHOT_ABI_MAJOR STREQUAL "0")
    message(FATAL_ERROR "export_sdk: 读出的 SHOT_ABI_MAJOR 为 0，契约头解析异常")
endif()
string(SHA256 _contract_sha256 "${_contract_text}")
message(STATUS "export_sdk: 契约 ABI ${_SHOT_ABI_MAJOR}.${_SHOT_ABI_MINOR}，sha256=${_contract_sha256}")

# ---------------------------------------------------------------------------
#  3. 整树重建：先删再建，SDK 里绝不允许出现上一次导出的残留文件
#     （旧版只删 include/lib/bin，docs/templates/third_party 会留下历史版本。）
# ---------------------------------------------------------------------------
if(EXISTS "${SDK_OUTPUT_DIR}")
    file(REMOVE_RECURSE "${SDK_OUTPUT_DIR}")
endif()
file(MAKE_DIRECTORY "${SDK_OUTPUT_DIR}")

# ---------------------------------------------------------------------------
#  4. include/：契约头 + 插件侧工具头（核心包【不含】opencv4）
#
#  include/ 的语义是"导出即契约"：只放来自宿主 include/ 的头，禁止手工塞第三方
#  头；第三方头进 third_party/include（ShotPlugin 的 SDK 模式会加进 include 路径）。
# ---------------------------------------------------------------------------
# 契约头：plugin_api.h 是汇总头（只 include），plugin_api_export.h 是导出宏，
# 正文在 plugin_api/ 子目录（下一段）。shot_plugin_helpers.h 是插件侧工具汇总头，
# shot_semver.h 是版本比较工具；其余 shot_* 是 helpers 的子头（保留在子目录）。
file(GLOB _contract_headers
    "${CONTRACT_INCLUDE_DIR}/plugin_api*.h"
    "${CONTRACT_INCLUDE_DIR}/shot_*.h"
)
if(_contract_headers)
    _copy_files_flat("${SDK_OUTPUT_DIR}/include" ${_contract_headers})
else()
    message(FATAL_ERROR "export_sdk: ${CONTRACT_INCLUDE_DIR} 下没有 plugin_api*.h/shot_*.h，"
        "SDK 没有契约头就没有意义，拒绝导出")
endif()

# plugin_api/ 子目录：汇总头里写的是 #include "plugin_api/xxx.h"，必须保留层级。
file(GLOB _contract_parts "${CONTRACT_INCLUDE_DIR}/plugin_api/*.h")
if(_contract_parts)
    _copy_files_tree("${CONTRACT_INCLUDE_DIR}" "${SDK_OUTPUT_DIR}/include" ${_contract_parts})
    list(LENGTH _contract_parts _contract_part_count)
    message(STATUS "export_sdk: ${_contract_part_count} 个契约子头 -> sdk/include/plugin_api")
else()
    message(WARNING "export_sdk: 未找到 plugin_api/*.h（契约子头，汇总头会 include 不到）")
endif()

# shot_plugin_helpers/ 子目录：汇总头写的是 #include "shot_plugin_helpers/xxx.h"。
file(GLOB _helper_headers "${CONTRACT_INCLUDE_DIR}/shot_plugin_helpers/*.h")
if(_helper_headers)
    _copy_files_tree("${CONTRACT_INCLUDE_DIR}" "${SDK_OUTPUT_DIR}/include" ${_helper_headers})
    list(LENGTH _helper_headers _helper_count)
    message(STATUS "export_sdk: ${_helper_count} 个插件侧工具头 -> sdk/include/shot_plugin_helpers")
else()
    message(WARNING "export_sdk: 未找到 shot_plugin_helpers/*.h（插件侧工具头）")
endif()

# 核心包不得带 opencv4：opencv 头只允许出现在可选叠加包 sdk/opencv/include 下。
if(EXISTS "${SDK_OUTPUT_DIR}/include/opencv4")
    message(FATAL_ERROR "export_sdk: 核心包 sdk/include 下出现了 opencv4，OpenCV 头应进 sdk/opencv/")
endif()

# ---------------------------------------------------------------------------
#  5. third_party/include：SDK 内置的 header-only 依赖（nlohmann-json / stb）
#
#  这两个是 shot_plugin_helpers.h / image_saver 级插件的直接依赖，不内置的话
#  "下载 SDK 即可编译"就是假的（实测 E1/E2）。它们的许可证单独放在
#  THIRD_PARTY_LICENSES/（vcpkg 安装树里 nlohmann 连 copyright 都没有）。
# ---------------------------------------------------------------------------
set(_third_party_inc "${SDK_OUTPUT_DIR}/third_party/include")
file(MAKE_DIRECTORY "${_third_party_inc}")

if(IS_DIRECTORY "${VCPKG_INC_DIR}/nlohmann")
    file(GLOB_RECURSE _nlohmann_headers
        "${VCPKG_INC_DIR}/nlohmann/*.hpp"
        "${VCPKG_INC_DIR}/nlohmann/*.h")
    if(_nlohmann_headers)
        _copy_files_tree("${VCPKG_INC_DIR}" "${_third_party_inc}" ${_nlohmann_headers})
        list(LENGTH _nlohmann_headers _nlohmann_count)
        message(STATUS "export_sdk: ${_nlohmann_count} 个 nlohmann 头 -> sdk/third_party/include/nlohmann（整树）")
    else()
        message(WARNING "export_sdk: ${VCPKG_INC_DIR}/nlohmann 下没有头文件，SDK 将无法编译 shot_plugin_helpers.h")
    endif()
else()
    message(WARNING "export_sdk: ${VCPKG_INC_DIR}/nlohmann 不存在，SDK 将缺少 nlohmann-json（检查 vcpkg 清单）")
endif()

file(GLOB _stb_headers "${VCPKG_INC_DIR}/stb_*.h")
if(_stb_headers)
    _copy_files_flat("${_third_party_inc}" ${_stb_headers})
    list(LENGTH _stb_headers _stb_count)
    message(STATUS "export_sdk: ${_stb_count} 个 stb 头 -> sdk/third_party/include（扁平文件名）")
else()
    message(WARNING "export_sdk: ${VCPKG_INC_DIR}/stb_*.h 不存在，SDK 将缺少 stb")
endif()

# ---------------------------------------------------------------------------
#  6. 许可证：仓库 LICENSE/NOTICE + 第三方的全文
# ---------------------------------------------------------------------------
file(MAKE_DIRECTORY "${SDK_OUTPUT_DIR}/THIRD_PARTY_LICENSES")
foreach(_license_file nlohmann-json.MIT.txt stb.LICENSE)
    if(EXISTS "${LICENSES_DIR}/${_license_file}")
        configure_file("${LICENSES_DIR}/${_license_file}"
            "${SDK_OUTPUT_DIR}/THIRD_PARTY_LICENSES/${_license_file}" COPYONLY)
    else()
        message(WARNING "export_sdk: 缺少第三方许可证 ${LICENSES_DIR}/${_license_file}")
    endif()
endforeach()

if(EXISTS "${_repo_root}/LICENSE")
    configure_file("${_repo_root}/LICENSE" "${SDK_OUTPUT_DIR}/LICENSE" COPYONLY)
else()
    message(WARNING "export_sdk: 未找到 ${_repo_root}/LICENSE，SDK 不附宿主许可证")
endif()
if(EXISTS "${_repo_root}/NOTICE")
    configure_file("${_repo_root}/NOTICE" "${SDK_OUTPUT_DIR}/NOTICE" COPYONLY)
else()
    # NOTICE 不存在不算错误（设计稿允许），但要在日志里留痕，便于发布前补齐。
    message(WARNING "export_sdk: 未找到 ${_repo_root}/NOTICE，SDK 不附第三方清单")
endif()

# ---------------------------------------------------------------------------
#  7. cmake/：插件工程构建模块，让 SDK 用户直接用 shot_add_plugin()
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
#  8. docs/：插件作者需要的设计文档（从 document/ 拷贝，单一事实来源）
#
#  BUILD_LINUX.md 是宿主构建文档，插件作者不需要，故意不导出。
#  AUTHORING.md 由文档线维护，还没写完时只 WARN（不能因为文档没写好卡构建）。
# ---------------------------------------------------------------------------
file(MAKE_DIRECTORY "${SDK_OUTPUT_DIR}/docs")
set(_sdk_docs
    README_SDK.md AUTHORING.md METADATA.md LISTS.md EVENT_BUS.md
    CAPTURE.md I18N.md SETTINGS.md ABI_POLICY.md PLATFORMS.md PLUGIN_TEST.md)
foreach(_doc IN LISTS _sdk_docs)
    if(EXISTS "${_repo_root}/document/${_doc}")
        configure_file("${_repo_root}/document/${_doc}"
            "${SDK_OUTPUT_DIR}/docs/${_doc}" COPYONLY)
    else()
        message(WARNING "export_sdk: 未找到 document/${_doc}，SDK docs 缺这一份")
        # 文档缺失只 WARN：导出不能被"另一条文档线还没写完"卡住；完整性由
        # package_sdk 的 docs 门禁（>= 3 份）兜底，发布前人工核对 docs 清单。
    endif()
endforeach()

# ---------------------------------------------------------------------------
#  9. templates/ + skill/：模板工程与 DSH skill（内容由模板/文档线维护）
#     缺失只 WARN：导出动作不能被"另一个 agent 还没写完模板"卡住。
# ---------------------------------------------------------------------------
if(IS_DIRECTORY "${PLUGIN_TEMPLATES_DIR}")
    _copy_tree_dir("${PLUGIN_TEMPLATES_DIR}" "${SDK_OUTPUT_DIR}/templates")
    _tree_stats("${SDK_OUTPUT_DIR}/templates" _tpl_count _tpl_bytes)
    message(STATUS "export_sdk: ${_tpl_count} 个模板文件 -> sdk/templates")
else()
    message(WARNING "export_sdk: 模板目录不存在 ${PLUGIN_TEMPLATES_DIR}，SDK 不附 templates/")
    # 模板缺失只 WARN（内容由模板线维护，导出不能被它卡住）；package_sdk 的门禁
    # 已经要求至少 1 个模板目录，发布时缺模板会直接失败。
endif()

if(IS_DIRECTORY "${SKILL_DIR}")
    _copy_tree_dir("${SKILL_DIR}" "${SDK_OUTPUT_DIR}/skill/plugin-shot-dev")
    _tree_stats("${SDK_OUTPUT_DIR}/skill/plugin-shot-dev" _skill_count _skill_bytes)
    message(STATUS "export_sdk: ${_skill_count} 个 skill 文件 -> sdk/skill/plugin-shot-dev")
else()
    message(WARNING "export_sdk: skill 目录不存在 ${SKILL_DIR}，SDK 不附 skill/")
endif()

# ---------------------------------------------------------------------------
#  9.2 demos/：完整示例工程（watermark / archive）
#      模板是骨架，demos 是"能直接跑"的完整插件；AUTHORING.md 与 skill 都引用它。
#      缺失只 WARN（示例由模板/文档线维护，导出不该被它卡住）。
# ---------------------------------------------------------------------------
if(NOT DEFINED PLUGIN_DEMOS_DIR OR "${PLUGIN_DEMOS_DIR}" STREQUAL "")
    set(PLUGIN_DEMOS_DIR "${PLUGIN_TEMPLATES_DIR}/../demos")
endif()
if(IS_DIRECTORY "${PLUGIN_DEMOS_DIR}")
    _copy_tree_dir("${PLUGIN_DEMOS_DIR}" "${SDK_OUTPUT_DIR}/demos")
    _tree_stats("${SDK_OUTPUT_DIR}/demos" _demo_count _demo_bytes)
    message(STATUS "export_sdk: ${_demo_count} 个示例文件 -> sdk/demos")
else()
    message(WARNING "export_sdk: 示例目录不存在 ${PLUGIN_DEMOS_DIR}，SDK 不附 demos/")
endif()

# ---------------------------------------------------------------------------
#  10. tools/new_plugin.cmake：脚手架（纯 cmake -P 脚本）
# ---------------------------------------------------------------------------
if(EXISTS "${NEW_PLUGIN_SCRIPT}")
    file(MAKE_DIRECTORY "${SDK_OUTPUT_DIR}/tools")
    configure_file("${NEW_PLUGIN_SCRIPT}" "${SDK_OUTPUT_DIR}/tools/new_plugin.cmake" COPYONLY)
    message(STATUS "export_sdk: 脚手架 -> sdk/tools/new_plugin.cmake")
else()
    message(WARNING "export_sdk: 未找到 ${NEW_PLUGIN_SCRIPT}，SDK 不附脚手架")
endif()

# ---------------------------------------------------------------------------
#  11. README.md —— 直接复制 document/README_SDK.md（单一事实来源，不要硬编码）
# ---------------------------------------------------------------------------
if(EXISTS "${_repo_root}/document/README_SDK.md")
    configure_file("${_repo_root}/document/README_SDK.md"
        "${SDK_OUTPUT_DIR}/README.md" COPYONLY)
    message(STATUS "export_sdk: README.md <- document/README_SDK.md")
else()
    message(WARNING "export_sdk: 未找到 document/README_SDK.md，SDK 不附 README.md")
endif()

# ---------------------------------------------------------------------------
#  12. 全树清洗：*.pdb / *.pc / pkgconfig 噪音
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _sdk_pdb "${SDK_OUTPUT_DIR}/*.pdb")
if(_sdk_pdb)
    list(LENGTH _sdk_pdb _pdb_count)
    file(REMOVE ${_sdk_pdb})
    message(STATUS "export_sdk: 剔除 PDB ${_pdb_count} 个")
endif()

file(GLOB_RECURSE _sdk_pc "${SDK_OUTPUT_DIR}/*.pc")
if(_sdk_pc)
    list(LENGTH _sdk_pc _pc_count)
    file(REMOVE ${_sdk_pc})
    message(STATUS "export_sdk: 剔除 pkgconfig *.pc ${_pc_count} 个")
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
#  13. 可选 OpenCV 叠加包：sdk/opencv/（-DWITH_OPENCV=ON 时）
#
#  目录结构与核心 SDK 根对齐（include/ lib/ bin/），所以第三方把它整个
#  opencv/ 目录放进核心 SDK 根即可；ShotPlugin.cmake 的 SDK 模式检测
#  <SDK>/opencv/manifest.json 是否存在，存在才补 OpenCV 路径。
# ---------------------------------------------------------------------------
if(WITH_OPENCV)
    set(_cv_dir "${SDK_OUTPUT_DIR}/opencv")
    file(MAKE_DIRECTORY "${_cv_dir}/include" "${_cv_dir}/lib" "${_cv_dir}/bin")

    # 13.1 头文件（保留 opencv4 子目录结构）
    if(IS_DIRECTORY "${VCPKG_INC_DIR}/opencv4")
        file(GLOB_RECURSE _opencv_headers
            "${VCPKG_INC_DIR}/opencv4/*.h"
            "${VCPKG_INC_DIR}/opencv4/*.hpp")
        if(_opencv_headers)
            _copy_files_tree("${VCPKG_INC_DIR}/opencv4" "${_cv_dir}/include/opencv4" ${_opencv_headers})
            list(LENGTH _opencv_headers _hdr_count)
            message(STATUS "export_sdk: ${_hdr_count} 个 OpenCV 头 -> sdk/opencv/include/opencv4")
        endif()
    else()
        message(FATAL_ERROR "export_sdk: WITH_OPENCV=ON 但 ${VCPKG_INC_DIR}/opencv4 不存在")
    endif()

    # 13.2 导入库 / 运行时 DLL（release 在 lib|bin，debug 在 lib/debug|bin/debug）
    _copy_whitelisted("${VCPKG_LIB_DIR}" "${_cv_dir}/lib" ".lib"
                      "${_SDK_LIB_WHITELIST}" _lib_count _lib_dropped)
    list(LENGTH _lib_dropped _lib_dropped_count)
    if(_lib_count GREATER 0)
        message(STATUS "export_sdk: ${_lib_count} 个导入库 -> sdk/opencv/lib（排除 ${_lib_dropped_count} 个非 SDK 依赖）")
    else()
        message(WARNING "export_sdk: ${VCPKG_LIB_DIR} 下没有白名单命中的 *.lib，opencv 包不完整")
    endif()

    _copy_whitelisted("${VCPKG_BIN_DIR}" "${_cv_dir}/bin" ".dll"
                      "${_SDK_DLL_WHITELIST}" _dll_count _dll_dropped)
    list(LENGTH _dll_dropped _dll_dropped_count)
    if(_dll_count GREATER 0)
        message(STATUS "export_sdk: ${_dll_count} 个运行时 DLL -> sdk/opencv/bin（排除 ${_dll_dropped_count} 个非 SDK 依赖）")
    else()
        message(WARNING "export_sdk: ${VCPKG_BIN_DIR} 下没有白名单命中的 *.dll，opencv 包不完整")
    endif()

    # debug 变体必须分目录：vcpkg debug 里大量文件与 release 同名（jpeg62.dll /
    # turbojpeg.dll / liblzma.dll / jpeg.lib ...），平铺会静默互相覆盖。
    if(IS_DIRECTORY "${VCPKG_DEBUG_LIB_DIR}")
        _copy_whitelisted("${VCPKG_DEBUG_LIB_DIR}" "${_cv_dir}/lib/debug" ".lib"
                          "${_SDK_LIB_WHITELIST}" _debug_lib_count _debug_lib_dropped)
        list(LENGTH _debug_lib_dropped _debug_lib_dropped_count)
        if(_debug_lib_count GREATER 0)
            message(STATUS "export_sdk: ${_debug_lib_count} 个 debug 导入库 -> sdk/opencv/lib/debug")
        else()
            message(WARNING "export_sdk: ${VCPKG_DEBUG_LIB_DIR} 下没有白名单命中的 *.lib，debug 变体不完整")
        endif()
    else()
        message(WARNING "export_sdk: 未找到 debug 导入库目录 ${VCPKG_DEBUG_LIB_DIR}，opencv 包仅含 release")
    endif()

    if(IS_DIRECTORY "${VCPKG_DEBUG_BIN_DIR}")
        _copy_whitelisted("${VCPKG_DEBUG_BIN_DIR}" "${_cv_dir}/bin/debug" ".dll"
                          "${_SDK_DLL_WHITELIST}" _debug_dll_count _debug_dll_dropped)
        list(LENGTH _debug_dll_dropped _debug_dll_dropped_count)
        if(_debug_dll_count GREATER 0)
            message(STATUS "export_sdk: ${_debug_dll_count} 个 debug DLL -> sdk/opencv/bin/debug")
        else()
            message(WARNING "export_sdk: ${VCPKG_DEBUG_BIN_DIR} 下没有白名单命中的 *.dll，debug 变体不完整")
        endif()
    else()
        message(WARNING "export_sdk: 未找到 debug 运行时目录 ${VCPKG_DEBUG_BIN_DIR}，opencv 包仅含 release")
    endif()

    # 13.3 OpenCV 版本（从 version.hpp 读，不依赖 find_package）
    set(_opencv_version "unknown")
    set(_opencv_version_hpp "${VCPKG_INC_DIR}/opencv4/opencv2/core/version.hpp")
    if(EXISTS "${_opencv_version_hpp}")
        file(READ "${_opencv_version_hpp}" _opencv_version_text)
        set(_opencv_parts "")
        foreach(_part MAJOR MINOR REVISION)
            string(REGEX MATCH "#define[ \t]+CV_VERSION_${_part}[ \t]+([0-9]+)"
                _opencv_match "${_opencv_version_text}")
            if(NOT _opencv_match)
                set(_opencv_parts "")
                break()
            endif()
            list(APPEND _opencv_parts "${CMAKE_MATCH_1}")
        endforeach()
        if(_opencv_parts)
            list(JOIN _opencv_parts "." _opencv_version)
        endif()
    endif()

    # 13.4 opencv 叠加包自己的 manifest（相对 sdk/opencv/ 根）
    _collect_file_hashes(_cv_file_items "${_cv_dir}" include lib bin)
    list(JOIN _cv_file_items ",\n" _cv_files_json)
    string(TIMESTAMP _cv_generated_at "%Y-%m-%dT%H:%M:%SZ" UTC)
    # 用 string(CONCAT) 而不是 set()：set(var "a" "b") 会生成列表（分号连接），
    # 写进文件就是非法 JSON。这里必须逐段拼接成一份连续文本。
    string(CONCAT _cv_manifest_text "{\n"
        "  \"schema\": 1,\n"
        "  \"kind\": \"opencv\",\n"
        "  \"version\": \"${_opencv_version}\",\n"
        "  \"sdk_version\": \"${SDK_VERSION}\",\n"
        "  \"abi\": { \"major\": ${_SHOT_ABI_MAJOR}, \"minor\": ${_SHOT_ABI_MINOR} },\n"
        "  \"platform\": \"${PLATFORM}\",\n"
        "  \"arch\": \"${ARCH}\",\n"
        "  \"generated_by\": \"cmake/export_sdk.cmake\",\n"
        "  \"generated_at\": \"${_cv_generated_at}\",\n"
        "  \"files\": [\n${_cv_files_json}\n  ]\n"
        "}\n")
    file(WRITE "${_cv_dir}/manifest.json" "${_cv_manifest_text}")

    _tree_stats("${_cv_dir}" _cv_count _cv_bytes)
    math(EXPR _cv_mib "${_cv_bytes} / 1048576")
    message(STATUS "export_sdk: OpenCV 叠加包 ${_opencv_version} -> sdk/opencv（${_cv_count} 个文件，约 ${_cv_mib} MiB）")
else()
    message(STATUS "export_sdk: WITH_OPENCV=OFF，不导出 OpenCV 叠加包（核心包只含契约头/第三方头/构建模块）")
endif()

# ---------------------------------------------------------------------------
#  14. manifest.json：机器可读清单（下载方/自检工具用）
#
#  字段约定（document/README_SDK.md「版本与 ABI」一节同步说明）：
#    sdk_version        SDK 包版本（与宿主版本同步）
#    host_min_version   首次携带该 ABI 的宿主版本，插件作者可据此声明要求
#    abi.major/minor    契约头里的 SHOT_ABI_MAJOR/MINOR
#    contract_sha256    契约头整份文本的 SHA256（防"新旧头混用"）
#    files[]            include/cmake/third_party 等关键文件的相对路径 + sha256
#  TODO: 加 files[] 全量校验的独立自检工具（tools/sdk_check.cmake），本脚本只生成。
# ---------------------------------------------------------------------------
_collect_file_hashes(_core_file_items "${SDK_OUTPUT_DIR}"
    README.md LICENSE NOTICE THIRD_PARTY_LICENSES
    include third_party cmake docs templates demos skill tools)
list(LENGTH _core_file_items _core_file_count)
list(JOIN _core_file_items ",\n" _core_files_json)
string(TIMESTAMP _generated_at "%Y-%m-%dT%H:%M:%SZ" UTC)

# 脚本模式（cmake -P）不会展开生成器表达式，这里老老实实算一个 JSON 布尔值。
if(WITH_OPENCV)
    set(_opencv_overlay_json "true")
else()
    set(_opencv_overlay_json "false")
endif()

# 用 string(CONCAT) 而不是 set()：set(var "a" "b") 会生成列表（分号连接），
# 写进文件就是非法 JSON。这里必须逐段拼接成一份连续文本。
string(CONCAT _manifest_text "{\n"
    "  \"schema\": 1,\n"
    "  \"kind\": \"core\",\n"
    "  \"sdk_version\": \"${SDK_VERSION}\",\n"
    "  \"host_min_version\": \"${HOST_MIN_VERSION}\",\n"
    "  \"abi\": { \"major\": ${_SHOT_ABI_MAJOR}, \"minor\": ${_SHOT_ABI_MINOR} },\n"
    "  \"contract_sha256\": \"${_contract_sha256}\",\n"
    "  \"platform\": \"${PLATFORM}\",\n"
    "  \"arch\": \"${ARCH}\",\n"
    "  \"opencv_overlay\": ${_opencv_overlay_json},\n"
    "  \"generated_by\": \"cmake/export_sdk.cmake\",\n"
    "  \"generated_at\": \"${_generated_at}\",\n"
    "  \"files\": [\n${_core_files_json}\n  ]\n"
    "}\n")
file(WRITE "${SDK_OUTPUT_DIR}/manifest.json" "${_manifest_text}")

# 回读校验：manifest 必须非空且能被 string(JSON) 解析出 sdk_version / abi.major。
# 这里故意再读一遍文件（而不是复用变量）：写盘失败 / 编码问题都要在这里炸出来。
file(READ "${SDK_OUTPUT_DIR}/manifest.json" _manifest_readback)
string(JSON _manifest_sdk_version ERROR_VARIABLE _json_error GET
    "${_manifest_readback}" sdk_version)
if(_json_error)
    message(FATAL_ERROR "export_sdk: 生成的 manifest.json 解析失败：${_json_error}")
endif()
string(JSON _manifest_abi_major GET "${_manifest_readback}" abi major)
if(NOT "${_manifest_sdk_version}" STREQUAL "${SDK_VERSION}")
    message(FATAL_ERROR "export_sdk: manifest.json sdk_version=${_manifest_sdk_version}，期望 ${SDK_VERSION}")
endif()
if(NOT "${_manifest_abi_major}" STREQUAL "${_SHOT_ABI_MAJOR}")
    message(FATAL_ERROR "export_sdk: manifest.json abi.major=${_manifest_abi_major}，期望 ${_SHOT_ABI_MAJOR}")
endif()

# ---------------------------------------------------------------------------
#  15. 结果汇总
# ---------------------------------------------------------------------------
_tree_stats("${SDK_OUTPUT_DIR}" _sdk_file_count _sdk_bytes)
math(EXPR _sdk_kib "${_sdk_bytes} / 1024")
message(STATUS "export_sdk: ============================================")
message(STATUS "export_sdk: SDK 核心包 -> ${SDK_OUTPUT_DIR}")
message(STATUS "export_sdk: 文件 ${_sdk_file_count} 个，约 ${_sdk_kib} KiB；manifest.files[] ${_core_file_count} 项")
message(STATUS "export_sdk: sdk_version=${SDK_VERSION} host_min_version=${HOST_MIN_VERSION} "
               "platform=${PLATFORM} arch=${ARCH} abi=${_SHOT_ABI_MAJOR}.${_SHOT_ABI_MINOR} opencv=${WITH_OPENCV}")
if(NOT WITH_OPENCV)
    message(STATUS "export_sdk: 需要 OpenCV 的用户请用 -DSHOT_SDK_WITH_OPENCV=ON 重新导出（或 package_sdk）")
endif()
message(STATUS "export_sdk: ============================================")
