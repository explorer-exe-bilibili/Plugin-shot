# ============================================================================
#  cmake/package_sdk.cmake — 把导出的 sdk/ 打成可发布的 zip + sha256
#
#  用法（脚本模式，由根 CMakeLists.txt 的 package_sdk 目标调用）：
#    cmake -DSDK_OUTPUT_DIR=<仓库 sdk/>          \
#          [-DOUTPUT_DIR=<仓库 dist/>]           \
#          [-DSDK_VERSION=0.1.0]                 \
#          [-DHOST_PLATFORM=win64]               \
#          [-DWITH_OPENCV=ON]                    \
#          [-DCMAKE_COMMAND=<cmake 路径>]        \
#          -P cmake/package_sdk.cmake
#
#  输入：
#    SDK_OUTPUT_DIR   必填，export_sdk 生成的 SDK 根（含 manifest.json）
#    OUTPUT_DIR       产物根，缺省 <仓库>/dist
#    SDK_VERSION      缺省从 manifest.json 读
#    HOST_PLATFORM    缺省从 manifest.json 的 platform 读，再缺省 win64
#    WITH_OPENCV      缺省从 manifest.json 的 opencv_overlay 读（true/false）
#    CMAKE_COMMAND    缺省用当前 cmake（脚本模式下 ${CMAKE_COMMAND} 已可用）
#
#  产物（OUTPUT_DIR 下）：
#    sdk/plugin-shot-sdk-<ver>-<platform>/            核心包 staging 目录
#    sdk/plugin-shot-sdk-<ver>-<platform>-opencv/     OpenCV 叠加包 staging 目录
#    plugin-shot-sdk-<ver>-<platform>.zip             核心包（单根目录）
#    plugin-shot-sdk-<ver>-<platform>.zip.sha256      sha256sum 格式（<hash>  <文件名>）
#    plugin-shot-sdk-<ver>-<platform>-opencv.zip      OpenCV 叠加包（单根目录里是 opencv/）
#    plugin-shot-sdk-<ver>-<platform>-opencv.zip.sha256
#
#  OpenCV 叠加包的用法（文档里也要写）：zip 解压后的单根目录是
#  plugin-shot-sdk-<ver>-<platform>-opencv/，里面只有一个 opencv/ 目录；
#  把那个 opencv/ 整个放进核心 SDK 根即可（<SDK>/opencv/manifest.json 存在时
#  cmake/ShotPlugin.cmake 的 SDK 模式才会补 OpenCV 头/库目录）。
#
#  为什么用 cmake -E tar 而不是 Compress-Archive：
#    PowerShell 的 Compress-Archive 有 2 GB 限制、老版本中文路径问题、且只 Windows；
#    cmake -E tar --format=zip 在 3.20 起可用，输出正斜杠路径、单个根目录，
#    与 CI（Windows/Linux 打包机）一致。
#
#  为什么 zip 不可复现：
#    tar/zip 会写入时间戳，同一棵树两次打包的 sha256 不同（设计稿实测过）。
#    所以发布以"实际上传的 zip"为准校验 .sha256，内容完整性靠 manifest.files[]。
#    TODO: imports 闭包门禁（dumpbin /dependents 校验每个被依赖 DLL 都在包里）；
#          需要 MSVC 工具链，当前先用"白名单 + 非空"作为弱门禁，见 §3。
#    TODO: docs 页眉哈希（把每份文档的 sha256 写进 manifest）——等文档线冻结再开。
# ============================================================================
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_COMMAND OR "${CMAKE_COMMAND}" STREQUAL "")
    message(FATAL_ERROR "package_sdk: 缺少 CMAKE_COMMAND")
endif()

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# ---------------------------------------------------------------------------
#  1. 参数与缺省值
# ---------------------------------------------------------------------------
if(NOT DEFINED SDK_OUTPUT_DIR OR "${SDK_OUTPUT_DIR}" STREQUAL "")
    message(FATAL_ERROR "package_sdk: 缺少必需参数 -DSDK_OUTPUT_DIR=<path>")
endif()
get_filename_component(_sdk_dir "${SDK_OUTPUT_DIR}" ABSOLUTE)
if(NOT IS_DIRECTORY "${_sdk_dir}")
    message(FATAL_ERROR "package_sdk: SDK 目录不存在 ${_sdk_dir}（先跑 export_sdk）")
endif()

set(_manifest "${_sdk_dir}/manifest.json")
if(NOT EXISTS "${_manifest}")
    message(FATAL_ERROR "package_sdk: 缺少 ${_manifest}（manifest 是发布资产的一部分，先跑 export_sdk）")
endif()
file(READ "${_manifest}" _manifest_json)
if("${_manifest_json}" STREQUAL "")
    message(FATAL_ERROR "package_sdk: ${_manifest} 是空文件")
endif()

# 先用 string(JSON) 验证 manifest 可解析，再从里面取缺省版本/平台。
string(JSON _manifest_sdk_version ERROR_VARIABLE _manifest_error GET
    "${_manifest_json}" sdk_version)
if(_manifest_error)
    message(FATAL_ERROR "package_sdk: ${_manifest} 不是合法 JSON：${_manifest_error}")
endif()
if("${_manifest_sdk_version}" STREQUAL "")
    message(FATAL_ERROR "package_sdk: ${_manifest} 缺少 sdk_version")
endif()

string(JSON _manifest_platform ERROR_VARIABLE _platform_error GET
    "${_manifest_json}" platform)
if(_platform_error)
    set(_manifest_platform "win64")
endif()

string(JSON _manifest_opencv ERROR_VARIABLE _opencv_error GET
    "${_manifest_json}" opencv_overlay)
if(_opencv_error)
    set(_manifest_opencv FALSE)
endif()

if(NOT DEFINED SDK_VERSION OR "${SDK_VERSION}" STREQUAL "")
    set(SDK_VERSION "${_manifest_sdk_version}")
endif()
if(NOT DEFINED HOST_PLATFORM OR "${HOST_PLATFORM}" STREQUAL "")
    set(HOST_PLATFORM "${_manifest_platform}")
endif()
if(NOT DEFINED WITH_OPENCV)
    set(WITH_OPENCV "${_manifest_opencv}")
endif()
if(NOT DEFINED OUTPUT_DIR OR "${OUTPUT_DIR}" STREQUAL "")
    set(OUTPUT_DIR "${_repo_root}/dist")
endif()
get_filename_component(_output_dir "${OUTPUT_DIR}" ABSOLUTE)
if("${_output_dir}" STREQUAL "${_repo_root}" OR "${_output_dir}" STREQUAL "/")
    message(FATAL_ERROR "package_sdk: OUTPUT_DIR=${OUTPUT_DIR} 指向仓库根/盘符根，拒绝把产物瘫在仓库根")
endif()
file(MAKE_DIRECTORY "${_output_dir}")

# ---------------------------------------------------------------------------
#  2. 门禁：核心包必须有这些内容（少一个都别发布）
# ---------------------------------------------------------------------------
set(_required_files
    "manifest.json"
    "README.md"
    "LICENSE"
    "include/plugin_api.h"
    "cmake/ShotPlugin.cmake"
    "third_party/include/nlohmann/json.hpp"
    "third_party/include/stb_image_write.h")
foreach(_required IN LISTS _required_files)
    if(NOT EXISTS "${_sdk_dir}/${_required}")
        message(FATAL_ERROR "package_sdk: 核心 SDK 缺少必需文件 ${_required}（导出不完整，拒绝打包）")
    endif()
endforeach()

# docs 至少 3 份（README_SDK/METADATA/ABI_POLICY 是底线）
file(GLOB _doc_files "${_sdk_dir}/docs/*.md")
list(LENGTH _doc_files _doc_count)
if(_doc_count LESS 3)
    message(FATAL_ERROR "package_sdk: docs/ 只有 ${_doc_count} 份文档（要求 >= 3），拒绝打包")
endif()

# templates 至少 1 个模板目录（模板内容由模板线维护；空模板 = 脚手架不可用）
set(_template_dir_count 0)
file(GLOB _template_entries LIST_DIRECTORIES true "${_sdk_dir}/templates/*")
foreach(_template_entry IN LISTS _template_entries)
    if(IS_DIRECTORY "${_template_entry}")
        math(EXPR _template_dir_count "${_template_dir_count} + 1")
    endif()
endforeach()
if(_template_dir_count LESS 1)
    message(FATAL_ERROR "package_sdk: templates/ 里没有模板目录（要求 >= 1），拒绝打包")
endif()
message(STATUS "package_sdk: 门禁通过（docs ${_doc_count} 份，模板 ${_template_dir_count} 个）")

# ---------------------------------------------------------------------------
#  3. OpenCV 叠加包门禁（启用时才要求）：include/opencv4、lib、bin 均非空
# ---------------------------------------------------------------------------
function(_require_nonempty_dir _dir _desc)
    if(NOT IS_DIRECTORY "${_dir}")
        message(FATAL_ERROR "package_sdk: 缺少${_desc} ${_dir}，拒绝打包")
    endif()
    file(GLOB_RECURSE _entries LIST_DIRECTORIES false "${_dir}/*")
    list(LENGTH _entries _entry_count)
    if(_entry_count EQUAL 0)
        message(FATAL_ERROR "package_sdk: ${_desc} ${_dir} 是空目录，拒绝打包")
    endif()
endfunction()

set(_cv_dir "${_sdk_dir}/opencv")
if(WITH_OPENCV)
    if(NOT EXISTS "${_cv_dir}/manifest.json")
        message(FATAL_ERROR
            "package_sdk: WITH_OPENCV 打开但 ${_cv_dir}/manifest.json 不存在（export 时忘了开 SHOT_SDK_WITH_OPENCV？）")
    endif()
    _require_nonempty_dir("${_cv_dir}/include/opencv4" " OpenCV 头目录")
    _require_nonempty_dir("${_cv_dir}/lib" " OpenCV 导入库目录")
    _require_nonempty_dir("${_cv_dir}/bin" " OpenCV 运行时 DLL 目录")
    # debug 变体可能缺（例如只用 release 的裁剪构建），只提示不失败。
    if(NOT IS_DIRECTORY "${_cv_dir}/lib/debug" OR NOT IS_DIRECTORY "${_cv_dir}/bin/debug")
        message(WARNING "package_sdk: OpenCV 叠加包缺少 debug 变体（lib/debug 或 bin/debug），Debug 用户会受影响")
    endif()
else()
    message(STATUS "package_sdk: WITH_OPENCV=OFF，只打核心包")
endif()

# ---------------------------------------------------------------------------
#  4. staging：<OUTPUT_DIR>/sdk/<单根目录>/（每次重建，杜绝历史残留）
# ---------------------------------------------------------------------------
set(_stage_root "${_output_dir}/sdk")
if(EXISTS "${_stage_root}")
    file(REMOVE_RECURSE "${_stage_root}")
endif()
file(MAKE_DIRECTORY "${_stage_root}")

set(_core_name "plugin-shot-sdk-${SDK_VERSION}-${HOST_PLATFORM}")
set(_core_stage "${_stage_root}/${_core_name}")
file(MAKE_DIRECTORY "${_core_stage}")

# 核心包 = sdk/ 下除 opencv/ 之外的所有条目（opencv 只进叠加包）。
file(GLOB _sdk_entries LIST_DIRECTORIES true "${_sdk_dir}/*")
foreach(_entry IN LISTS _sdk_entries)
    get_filename_component(_entry_name "${_entry}" NAME)
    if(_entry_name STREQUAL "opencv")
        continue()
    endif()
    if(IS_DIRECTORY "${_entry}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_entry}" "${_core_stage}/${_entry_name}"
            RESULT_VARIABLE _copy_rc
            OUTPUT_QUIET)
    else()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy "${_entry}" "${_core_stage}/${_entry_name}"
            RESULT_VARIABLE _copy_rc
            OUTPUT_QUIET)
    endif()
    if(NOT _copy_rc EQUAL 0)
        message(FATAL_ERROR "package_sdk: 拷贝 ${_entry} 到 staging 失败（exit ${_copy_rc}）")
    endif()
endforeach()

file(GLOB_RECURSE _core_stage_files LIST_DIRECTORIES false "${_core_stage}/*")
list(LENGTH _core_stage_files _core_stage_count)
message(STATUS "package_sdk: 核心包 staging ${_core_stage_count} 个文件 -> ${_core_stage}")

# OpenCV 叠加包 staging：单根目录里只放 opencv/，外加一份安装说明。
# 这样用户解压后把 opencv/ 拖进核心 SDK 根即可（与 ShotPlugin 的检测路径对齐）。
set(_cv_name "${_core_name}-opencv")
set(_cv_stage "${_stage_root}/${_cv_name}")
if(WITH_OPENCV)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_cv_dir}" "${_cv_stage}/opencv"
        RESULT_VARIABLE _cv_copy_rc
        OUTPUT_QUIET)
    if(NOT _cv_copy_rc EQUAL 0)
        message(FATAL_ERROR "package_sdk: 拷贝 OpenCV 叠加包失败（exit ${_cv_copy_rc}）")
    endif()
    file(WRITE "${_cv_stage}/README-OPENCV.txt"
        "Plugin-shot 离线插件 SDK —— 可选 OpenCV 叠加包\n"
        "\n"
        "用法：把本文件旁边的 opencv/ 整个目录放进核心 SDK 根目录，最终布局为\n"
        "  <核心 SDK 根>/opencv/manifest.json\n"
        "  <核心 SDK 根>/opencv/include/opencv4/...\n"
        "  <核心 SDK 根>/opencv/lib/...  <核心 SDK 根>/opencv/lib/debug/...\n"
        "  <核心 SDK 根>/opencv/bin/...  <核心 SDK 根>/opencv/bin/debug/...\n"
        "\n"
        "之后用 cmake/ShotPlugin.cmake 的 shot_add_plugin() 构建插件时，SDK 模式会\n"
        "自动补 OpenCV 头目录，并按 $<CONFIG:Debug> 选择 lib/debug 或 lib。\n"
        "核心包（plugin-shot-sdk-${SDK_VERSION}-${HOST_PLATFORM}.zip）与本包的 sha256\n"
        "见同目录 .zip.sha256 文件；详细说明见核心包 README.md。\n")
    file(GLOB_RECURSE _cv_stage_files LIST_DIRECTORIES false "${_cv_stage}/*")
    list(LENGTH _cv_stage_files _cv_stage_count)
    message(STATUS "package_sdk: OpenCV 叠加包 staging ${_cv_stage_count} 个文件 -> ${_cv_stage}")
endif()

# ---------------------------------------------------------------------------
#  5. 打 zip（单根目录；正斜杠路径；跨平台）
# ---------------------------------------------------------------------------
function(_package_zip _stage_parent _root_name _zip_path)
    if(EXISTS "${_zip_path}")
        file(REMOVE "${_zip_path}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E chdir "${_stage_parent}"
                "${CMAKE_COMMAND}" -E tar cf "${_zip_path}" --format=zip -- "${_root_name}"
        RESULT_VARIABLE _zip_rc
        OUTPUT_QUIET
        ERROR_VARIABLE _zip_err)
    if(NOT _zip_rc EQUAL 0 OR NOT EXISTS "${_zip_path}")
        message(FATAL_ERROR "package_sdk: 打包失败 ${_zip_path}（exit ${_zip_rc}）：${_zip_err}")
    endif()
endfunction()

# sha256：优先 cmake -E sha256sum（CMake >= 3.28，输出就是 sha256sum 格式）；
# 老版本 / 受限环境回退 certutil（Windows 自带，输出本地化，只按 64 位 hex 提取）；
# 最后一道是 file(SHA256)（任何平台都能用）。
function(_sha256_of_file _file _out_hash)
    set(_hash "")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E sha256sum "${_file}"
        OUTPUT_VARIABLE _sha_out
        RESULT_VARIABLE _sha_rc
        ERROR_QUIET)
    if(_sha_rc EQUAL 0)
        string(REGEX MATCH "^([0-9a-fA-F]{64})" _sha_match "${_sha_out}")
        if(_sha_match)
            set(_hash "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(NOT _hash AND WIN32)
        execute_process(
            COMMAND certutil -hashfile "${_file}" SHA256
            OUTPUT_VARIABLE _cert_out
            RESULT_VARIABLE _cert_rc
            ERROR_QUIET)
        if(_cert_rc EQUAL 0)
            string(REGEX MATCH "[0-9a-fA-F]{64}" _cert_match "${_cert_out}")
            if(_cert_match)
                set(_hash "${CMAKE_MATCH_1}")
            endif()
        endif()
    endif()
    if(NOT _hash)
        file(SHA256 "${_file}" _hash)
    endif()
    set(${_out_hash} "${_hash}" PARENT_SCOPE)
endfunction()

# 落一个"zip + .zip.sha256"资产并打印大小/hash。
function(_emit_package _stage_parent _root_name)
    set(_zip_path "${_output_dir}/${_root_name}.zip")
    _package_zip("${_stage_parent}" "${_root_name}" "${_zip_path}")

    _sha256_of_file("${_zip_path}" _sha256)
    if("${_sha256}" STREQUAL "")
        message(FATAL_ERROR "package_sdk: 计算 sha256 失败 ${_zip_path}")
    endif()
    # sha256sum 兼容格式（两空格分隔，校验命令：sha256sum -c <file>）。
    file(WRITE "${_zip_path}.sha256" "${_sha256}  ${_root_name}.zip\n")

    file(SIZE "${_zip_path}" _zip_size)
    if(_zip_size GREATER_EQUAL 1048576)
        math(EXPR _zip_mib "${_zip_size} / 1048576")
        set(_zip_size_text "约 ${_zip_mib} MiB")
    else()
        math(EXPR _zip_kib "${_zip_size} / 1024")
        set(_zip_size_text "约 ${_zip_kib} KiB")
    endif()
    message(STATUS "package_sdk: 产物  ${_zip_path}")
    message(STATUS "package_sdk: 大小  ${_zip_size} 字节（${_zip_size_text}）")
    message(STATUS "package_sdk: sha256 ${_sha256}")
endfunction()

_emit_package("${_stage_root}" "${_core_name}")
if(WITH_OPENCV)
    _emit_package("${_stage_root}" "${_cv_name}")
endif()

# ---------------------------------------------------------------------------
#  6. 结果汇总
# ---------------------------------------------------------------------------
message(STATUS "package_sdk: ============================================")
message(STATUS "package_sdk: 发布资产根 -> ${_output_dir}")
if(WITH_OPENCV)
    message(STATUS "package_sdk: 核心包 + OpenCV 叠加包（4 个文件：2 zip + 2 sha256）")
else()
    message(STATUS "package_sdk: 只有核心包（2 个文件：zip + sha256）；需要 OpenCV 加 -DWITH_OPENCV=ON")
endif()
message(STATUS "package_sdk: staging 保留在 ${_stage_root}（可直接检查 zip 内容，下一次运行会清空）")
message(STATUS "package_sdk: ============================================")
