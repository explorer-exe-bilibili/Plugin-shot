# ============================================================================
#  📋 CMakeLists.txt — 跨平台 C++ 构建模板
#  📌 使用说明：只需修改下方 >>> CONFIGURATION <<< 区域即可适配新项目
#  🚀 使用 vcpkg 管理依赖
#  🌐 支持 Windows / macOS / Linux 三平台 (含 WSL 优化)
#  🔧 自动检测 编译器 / 架构 / 构建类型 / Git 信息
# ============================================================================
#  如果你正在将本模板用于一个新项目，请依次：
#     1. 修改 CONFIGURATION 区的项目名称和版本号
#     2. 在 DEPENDENCIES 区域按需添加你的 find_package
#     3. 在 [15. 查找并链接依赖库] 中同步更新 include/link
#     4. 检查 src/ include/ 目录结构是否匹配
# ============================================================================

# ============================================================================
#  0. cmake 最小版本 —— 必须是第一条 CMake 命令（先于 project() 与所有依赖探测）
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# ============================================================================
#  >>> START OF CONFIGURATION — 从这里开始定制你的项目 <<<
# ============================================================================

# --- 项目标识 ------------------------------------------------
set(PROJECT_NAME                  "Plugin-shot")            # 项目名称 = 可执行文件名
set(PROJECT_DESCRIPTION           "A plugin-based screenshot tool with a stable C plugin ABI")
set(PROJECT_HOMEPAGE_URL          "https://alist.1class.top/")
set(PROJECT_COMPANY               "Plugin-shot contributors")           # 出现在 exe 属性的 CompanyName
set(PROJECT_COPYRIGHT             "Copyright (C) 2025")   # 出现在 LegalCopyright

# --- 版本号 --------------------------------------------------
set(PROJECT_VERSION_MAJOR         0)
set(PROJECT_VERSION_MINOR         1)
set(PROJECT_VERSION_PATCH         0)
set(PROJECT_VERSION_BUILD         0)      # 构建号，会被 build_number.txt 覆盖

# --- 目录结构 ------------------------------------------------
set(SRC_DIR                       "src")
set(INC_DIR                       "include")
set(RESOURCE_DIR                  "files")
set(VERSION_H_IN                  "${INC_DIR}/version.h.in")

# --- 可选特性 ------------------------------------------------
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Windows 图标与资源文件 -----------------------------------
set(APP_ICON                      "${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.ico")
set(APP_RC_IN                     "${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in")

# --- C++ 标准 ------------------------------------------------
set(CPP_STANDARD                  23)                     # 17 / 20 / 23

# ============================================================================
# 在15处添加你的 find_package 调用，并在 15. 查找并链接依赖库 中同步更新 include/link
# ===========================================================================


# ============================================================================
#  1. 依赖管理基础设施 — vcpkg 工具链检测（必须在 project() 之前）
# ============================================================================
if(DEFINED ENV{GITHUB_ACTIONS})
    message(STATUS "Running in GitHub Actions environment")
    set(IN_GITHUB_ACTIONS TRUE)
else()
    message(STATUS "Running in local development environment")
    set(IN_GITHUB_ACTIONS FALSE)
endif()

message(STATUS "Using vcpkg for dependency management")

if(NOT IN_GITHUB_ACTIONS)
    if(WIN32)
        set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "Vcpkg target triplet")
    elseif(APPLE)
        set(VCPKG_TARGET_TRIPLET "x64-osx"     CACHE STRING "Vcpkg target triplet")
    else()
        set(VCPKG_TARGET_TRIPLET "x64-linux"   CACHE STRING "Vcpkg target triplet")
    endif()
else()
    if(DEFINED ENV{VCPKG_TARGET_TRIPLET})
        set(VCPKG_TARGET_TRIPLET $ENV{VCPKG_TARGET_TRIPLET} CACHE STRING "Vcpkg target triplet")
    else()
        if(WIN32)
            set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "Vcpkg target triplet")
        elseif(APPLE)
            set(VCPKG_TARGET_TRIPLET "x64-osx"            CACHE STRING "Vcpkg target triplet")
        else()
            set(VCPKG_TARGET_TRIPLET "x64-linux"          CACHE STRING "Vcpkg target triplet")
        endif()
    endif()
endif()

set(USE_MIXED_LINKING FALSE)
if(WIN32 AND VCPKG_TARGET_TRIPLET MATCHES "static-dynamic-ffmpeg")
    message(STATUS "Using mixed linking: Static libraries + Dynamic FFMPEG")
    set(USE_MIXED_LINKING TRUE)
endif()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        message(STATUS "Using VCPKG_ROOT: $ENV{VCPKG_ROOT}")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    elseif(DEFINED ENV{VCPKG_INSTALLATION_ROOT})
        message(STATUS "Using VCPKG_INSTALLATION_ROOT: $ENV{VCPKG_INSTALLATION_ROOT}")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_INSTALLATION_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    else()
        message(STATUS "Using default vcpkg path: /opt/vcpkg")
        set(CMAKE_TOOLCHAIN_FILE "/opt/vcpkg/scripts/buildsystems/vcpkg.cmake")
    endif()
endif()
message(STATUS "Vcpkg target triplet: ${VCPKG_TARGET_TRIPLET}")
message(STATUS "Vcpkg toolchain file: ${CMAKE_TOOLCHAIN_FILE}")

set(VCPKG_MANIFEST_MODE ON CACHE BOOL "Use vcpkg manifest mode for auto-install" FORCE)
set(VCPKG_MANIFEST_DIR  "${CMAKE_SOURCE_DIR}" CACHE PATH "Directory containing vcpkg.json" FORCE)


# ============================================================================
#  2. 项目定义 — project() 必须在 vcpkg 工具链之后
# ============================================================================
project(${PROJECT_NAME}
    VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}
    DESCRIPTION ${PROJECT_DESCRIPTION}
    HOMEPAGE_URL ${PROJECT_HOMEPAGE_URL}
    LANGUAGES CXX
)


# ============================================================================
#  3. 版本相关变量
# ============================================================================
set(PROJECT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")


# ============================================================================
#  4. 构建信息 — 日期、时间、Git 提交
# ============================================================================
string(TIMESTAMP BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP BUILD_TIME "%H:%M:%S")

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(GIT_COMMIT_HASH "unknown")
    set(GIT_BRANCH       "unknown")
endif()


# ============================================================================
#  5. 平台检测
# ============================================================================
if(WIN32)
    set(PLATFORM_NAME "Windows")
    set(PLATFORM_WINDOWS TRUE)
    execute_process(
        COMMAND cmd /c "ver"
        OUTPUT_VARIABLE WINDOWS_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(WINDOWS_VERSION MATCHES "Version ([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(PLATFORM_VERSION "${CMAKE_MATCH_1}")
    else()
        set(PLATFORM_VERSION "Unknown")
    endif()

elseif(APPLE)
    set(PLATFORM_NAME "macOS")
    set(PLATFORM_MACOS TRUE)
    execute_process(
        COMMAND sw_vers -productVersion
        OUTPUT_VARIABLE PLATFORM_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT PLATFORM_VERSION)
        set(PLATFORM_VERSION "Unknown")
    endif()

elseif(UNIX)
    set(PLATFORM_NAME "Linux")
    set(PLATFORM_LINUX TRUE)
    if(EXISTS "/etc/os-release")
        execute_process(
            COMMAND grep "^PRETTY_NAME=" /etc/os-release
            COMMAND sed "s/PRETTY_NAME=\"\\(.*\\)\"/\\1/"
            OUTPUT_VARIABLE PLATFORM_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    if(NOT PLATFORM_VERSION)
        set(PLATFORM_VERSION "Unknown Linux")
    endif()
    # 提前检测 WSL，供 version.h 使用
    execute_process(
        COMMAND grep -q Microsoft /proc/version
        RESULT_VARIABLE IS_WSL
        OUTPUT_QUIET ERROR_QUIET
    )
    if(IS_WSL EQUAL 0)
        set(PLATFORM_WSL TRUE)
    endif()
else()
    set(PLATFORM_NAME    "Unknown")
    set(PLATFORM_VERSION "Unknown")
endif()


# ============================================================================
#  6. 编译器检测
# ============================================================================
if(MSVC)
    set(COMPILER_NAME "MSVC")
    set(COMPILER_MSVC TRUE)
    set(COMPILER_VERSION "${MSVC_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(COMPILER_NAME "GCC")
    set(COMPILER_GCC TRUE)
    set(COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(COMPILER_NAME "Clang")
    set(COMPILER_CLANG TRUE)
    set(COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
else()
    set(COMPILER_NAME    "Unknown")
    set(COMPILER_VERSION "Unknown")
endif()


# ============================================================================
#  7. 架构检测
# ============================================================================
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)")
        set(ARCHITECTURE "arm64")
        set(ARCH_ARM64 TRUE)
    else()
        set(ARCHITECTURE "x64")
        set(ARCH_X64 TRUE)
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
        set(ARCHITECTURE "arm")
        set(ARCH_ARM TRUE)
    else()
        set(ARCHITECTURE "x86")
        set(ARCH_X86 TRUE)
    endif()
endif()


# ============================================================================
#  8. 构建类型检测
# ============================================================================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BUILD_TYPE "Debug")
    set(BUILD_DEBUG TRUE)
else()
    set(BUILD_TYPE "Release")
    set(BUILD_RELEASE TRUE)
endif()

set(USE_VCPKG TRUE)


# ============================================================================
#  9. 自动递增构建号 —— 必须在 configure_file 之前完成
# ============================================================================
if(EXISTS "${CMAKE_SOURCE_DIR}/build_number.txt")
    file(READ "${CMAKE_SOURCE_DIR}/build_number.txt" BUILD_NUMBER)
    string(STRIP "${BUILD_NUMBER}" BUILD_NUMBER)
    if(BUILD_NUMBER MATCHES "^[0-9]+$")
        math(EXPR BUILD_NUMBER "${BUILD_NUMBER} + 1")
    else()
        set(BUILD_NUMBER 1)
    endif()
else()
    set(BUILD_NUMBER 1)
endif()
file(WRITE "${CMAKE_SOURCE_DIR}/build_number.txt" "${BUILD_NUMBER}")
set(PROJECT_VERSION_BUILD ${BUILD_NUMBER})


# ============================================================================
#  10. 配置版本头文件
# ============================================================================
configure_file(
    "${CMAKE_SOURCE_DIR}/${VERSION_H_IN}"
    "${CMAKE_BINARY_DIR}/generated/version.h"
    @ONLY
)


# ============================================================================
#  11. Windows 资源文件处理
# ============================================================================
if(WIN32)
    # 图标必须在 .rc 之前定稿：改名后 ${PROJECT_NAME}.ico 往往还没跟上，
    # 而 .rc 里写死 ICON 语句会让 rc.exe 直接 RC2135 失败。缺失就降级成注释。
    if(EXISTS "${APP_ICON}")
        configure_file(
            "${APP_ICON}"
            "${CMAKE_BINARY_DIR}/generated/${PROJECT_NAME}.ico"
            COPYONLY
        )
        set(APP_ICON_RESOURCE_LINE "IDI_ICON1 ICON \"${PROJECT_NAME}.ico\"")
    else()
        message(WARNING "App icon not found: ${APP_ICON}，生成的 .rc 不含图标资源")
        set(APP_ICON_RESOURCE_LINE "// 无图标资源：${APP_ICON} 不存在")
    endif()

    configure_file(
        "${APP_RC_IN}"
        "${CMAKE_BINARY_DIR}/generated/${PROJECT_NAME}.rc"
        @ONLY
    )
    set(RESOURCE_FILES "${CMAKE_BINARY_DIR}/generated/${PROJECT_NAME}.rc")
else()
    set(RESOURCE_FILES "")
endif()


# ============================================================================
#  12. C++ 标准与输出目录
# ============================================================================
set(CMAKE_CXX_STANDARD          ${CPP_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(CMAKE_CONFIGURATION_TYPES)
    set(APP_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
else()
    set(APP_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

set(PLUGINS_OUTPUT_DIR "${APP_OUTPUT_DIR}/plugins")
# 注意：这里【不能】写 file(MAKE_DIRECTORY "${PLUGINS_OUTPUT_DIR}")。
# 多配置生成器下该路径含 $<CONFIG> 生成器表达式，configure 期会真的建出
# 一个字面量名为 "$<CONFIG>" 的目录。目录创建统一推迟到构建期，
# 见第 20 节 cmake/copy_runtime_deps.cmake。

# ============================================================================
#  13. 收集源文件
# ============================================================================
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "${SRC_DIR}/*.cpp" "${SRC_DIR}/*.c")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "${INC_DIR}/*.hpp" "${INC_DIR}/*.h")


# ============================================================================
#  14. 创建可执行目标
# ============================================================================
if(WIN32)
    add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${RESOURCE_FILES})
else()
    add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})
endif()

if(MSVC)
    set_property(TARGET ${PROJECT_NAME} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# 缺陷1/缺陷2：链接器与编译器产出的 PDB 一律落到 build 树内的独立目录。
# 不重定向的话，链接器会把 .pdb 直接丢在目标文件旁边 —— 也就是 bin/ 与
# bin/plugins/，而这两个目录是禁止出现 pdb 的。从源头移走 + POST_BUILD 兜底。
set(PROJECT_PDB_DIR "${CMAKE_BINARY_DIR}/pdb")
set_target_properties(${PROJECT_NAME} PROPERTIES
    PDB_OUTPUT_DIRECTORY         "${PROJECT_PDB_DIR}"
    COMPILE_PDB_OUTPUT_DIRECTORY "${PROJECT_PDB_DIR}"
)


# ============================================================================
#  14.1 链接前杀掉正在运行的实例（防止 Windows 上 LNK1104 文件占用）
# ============================================================================
option(ENABLE_KILL_RUNNING "Kill running instances of the target before linking" ON)

if(ENABLE_KILL_RUNNING)
    if(WIN32)
        set(KILL_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kill_running.bat")
    else()
        set(KILL_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kill_running.sh")
        # file(CHMOD) 需要 CMake >= 3.19；低版本走 chmod 命令
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.19")
            file(CHMOD "${KILL_SCRIPT}"
                 FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                                  GROUP_READ GROUP_EXECUTE
                                  WORLD_READ WORLD_EXECUTE)
        else()
            execute_process(COMMAND chmod +x "${KILL_SCRIPT}")
        endif()
    endif()

    if(NOT EXISTS "${KILL_SCRIPT}")
        message(FATAL_ERROR "Kill script not found: ${KILL_SCRIPT}")
    endif()

    add_custom_command(TARGET ${PROJECT_NAME} PRE_LINK
        COMMAND "${KILL_SCRIPT}" "${PROJECT_NAME}"
        COMMENT "Ensuring no running instances of ${PROJECT_NAME}"
        VERBATIM
    )
endif()

# ============================================================================
#  15. 查找并链接依赖库 — 在这里添加你的 find_package 和链接
# ============================================================================
message(STATUS "Using vcpkg dependencies")

# ---- 在这里添加你的 find_package 调用 ----
# 示例：
#   find_package(glfw3 CONFIG REQUIRED)
#   find_package(nlohmann_json CONFIG REQUIRED)
#   find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# ---- 编译定义 ----
target_compile_definitions(${PROJECT_NAME} PRIVATE
    NOMINMAX
    $<$<CONFIG:Debug>:DEBUG_MODE>
)

# ---- 包含目录 ----
target_include_directories(${PROJECT_NAME} PRIVATE
    ${INC_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    # 在这里添加你的 xxx_INCLUDE_DIRS
)

# ---- 链接 ----
target_link_libraries(${PROJECT_NAME} PRIVATE
    # 在这里添加你的链接库
    # glfw
    nlohmann_json::nlohmann_json
)


# ============================================================================
#  15.1 插件工程 —— plugins/*/
#
#  每个插件是一个独立的小工程：自带 CMakeLists.txt + metadata.json + include/ + src/。
#  构建规则统一由 cmake/ShotPlugin.cmake 的 shot_add_plugin() 提供（/MD、隐藏符号、
#  输出目录、贴元数据、剪枝），宿主这里只负责发现并纳入，不再重复写目标属性。
#  关键：绝不为插件复制任何依赖 DLL。宿主本身不链接 OpenCV，`bin/` 里的
#  opencv_*4.dll 由 copy_runtime_deps 从 vcpkg 运行时目录整目录布置 —— 需要 OpenCV
#  的插件在自己的 CMakeLists 里 find_package(OpenCV) 并链接，运行时解析到 bin/ 那一份。
#  插件目录里再放一份副本会在同一进程内造出第二个 OpenCV（类型/分配器全部割裂），
#  所以 prune_output_dir 会把与 vcpkg 同名的基础依赖从插件目录清走。
# ============================================================================
set(SHOT_APP_OUTPUT_DIR       "${APP_OUTPUT_DIR}")
set(SHOT_PLUGINS_OUTPUT_DIR   "${PLUGINS_OUTPUT_DIR}")
set(SHOT_BUILD_LIB_DIR        "${CMAKE_BINARY_DIR}/lib")
set(SHOT_PDB_DIR              "${PROJECT_PDB_DIR}/plugins")
set(SHOT_CONTRACT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${INC_DIR}")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShotPlugin.cmake")

file(GLOB SHOT_PLUGIN_PROJECTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/*/CMakeLists.txt"
)

if(SHOT_PLUGIN_PROJECTS)
    foreach(_plugin_project IN LISTS SHOT_PLUGIN_PROJECTS)
        get_filename_component(_plugin_dir  "${_plugin_project}" DIRECTORY)
        get_filename_component(_plugin_name "${_plugin_dir}" NAME)
        add_subdirectory("${_plugin_dir}" "${CMAKE_BINARY_DIR}/plugins/${_plugin_name}")
        message(STATUS "插件工程 ${_plugin_name} -> ${PLUGINS_OUTPUT_DIR}")
    endforeach()
else()
    message(STATUS "跳过插件工程：plugins/*/CMakeLists.txt 尚未创建")
endif()


# ============================================================================
#  16. 平台特定设置
# ============================================================================
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        opengl32 gdi32 user32 kernel32 shell32 ole32 ws2_32 ntdll advapi32 bcrypt userenv
    )

    if(EXISTS "${APP_ICON}")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APP_ICON}" "${APP_OUTPUT_DIR}/${PROJECT_NAME}.ico"
            COMMENT "Copying app icon"
        )
    endif()

    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

elseif(APPLE)
    # --- macOS 常用框架（按需取消注释） ---
    find_library(COCOA_LIBRARY     Cocoa)
    find_library(IOKIT_LIBRARY     IOKit)
    # find_library(COREVIDEO_LIBRARY CoreVideo)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${COCOA_LIBRARY}
        ${IOKIT_LIBRARY}
        # ${COREVIDEO_LIBRARY}
    )

elseif(UNIX)
    # --- Linux：如需 GUI，取消注释 ---
    # find_package(X11 QUIET)
    # if(X11_FOUND)
    #     target_include_directories(${PROJECT_NAME} PRIVATE ${X11_INCLUDE_DIR})
    #     target_link_libraries(${PROJECT_NAME} PRIVATE ${X11_LIBRARIES})
    # endif()

    # --- glfw 别名（若你使用 glfw） ---
    # if(NOT TARGET glfw)
    #     find_package(glfw3 REQUIRED)
    #     if(TARGET glfw3)
    #         set_target_properties(glfw3 PROPERTIES IMPORTED_GLOBAL TRUE)
    #         add_library(glfw ALIAS glfw3)
    #     endif()
    # endif()
endif()


# ============================================================================
#  17. WSL 环境优化
# ============================================================================
if(UNIX AND NOT APPLE)
    execute_process(
        COMMAND grep -q Microsoft /proc/version
        RESULT_VARIABLE IS_WSL
        OUTPUT_QUIET ERROR_QUIET
    )
    if(IS_WSL EQUAL 0)
        message(STATUS "WSL environment detected, applying optimizations")
        target_compile_definitions(${PROJECT_NAME} PRIVATE WSL_BUILD=1)
        target_compile_options(${PROJECT_NAME} PRIVATE
            -fno-strict-aliasing
            -fstack-protector-strong
            -D_FORTIFY_SOURCE=2
            -fno-omit-frame-pointer
            $<$<CONFIG:Debug>:-O1;-g;-fsanitize=address>
        )
        target_link_options(${PROJECT_NAME} PRIVATE
            -Wl,-z,relro,-z,now
            $<$<CONFIG:Debug>:-fsanitize=address>
        )
        target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS} pthread)
    endif()
endif()


# ============================================================================
#  18. 安装规则
# ============================================================================
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)


# ============================================================================
#  19. 复制资源文件到输出目录
# ============================================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${RESOURCE_DIR}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/${RESOURCE_DIR}"
            "${APP_OUTPUT_DIR}/files"
        COMMENT "Copying resources if files changed"
    )
endif()


# ============================================================================
#  20. DLL 复制与 SDK 导出
#
#  产物布局（硬约束）：
#    bin/          只放运行依赖，禁止 *.pdb
#    bin/plugins/  只放插件自身产物，禁止 OpenCV 等基础依赖（dll/pdb 都不许）
#    sdk/          离线 SDK：include(契约头 + opencv4) / lib(仅 *.lib) / bin(仅 *.dll)
#
#  所有通配符清理都写在 cmake -P 脚本里，用 file(GLOB) + file(REMOVE) 展开。
#  禁止再写 cmake -E remove -f "DIR/*.pdb"：cmake -E 不展开通配符，
#  这条命令在历史上从未删掉过任何 pdb。
# ============================================================================
if(WIN32)
    set(VCPKG_TRIPLET_DIR   "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
    set(VCPKG_BIN_DIR       "${VCPKG_TRIPLET_DIR}/bin")          # release 运行时 DLL
    set(VCPKG_DEBUG_BIN_DIR "${VCPKG_TRIPLET_DIR}/debug/bin")    # debug   运行时 DLL
    set(VCPKG_LIB_DIR       "${VCPKG_TRIPLET_DIR}/lib")
    set(VCPKG_INC_DIR       "${VCPKG_TRIPLET_DIR}/include")

    # 缺陷3：运行时依赖目录必须按【配置】选择，不能用固定的 release bin。
    #   Debug   -> <triplet>/debug/bin  （opencv_core4d.dll 等带 d 后缀）
    #   Release -> <triplet>/bin
    # 用生成器表达式实现：单配置 Ninja（CONFIG=CMAKE_BUILD_TYPE）与多配置
    # 生成器（逐配置解析）都能得到正确路径。
    # 选错目录的后果：Debug 宿主找不到 opencv_core4d.dll，一调用 OpenCV 就启动失败。
    set(RUNTIME_BIN_DIR "${VCPKG_TRIPLET_DIR}/$<$<CONFIG:Debug>:debug/>bin")

    set(COPY_DEPS_SCRIPT  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_deps.cmake")
    set(EXPORT_SDK_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/export_sdk.cmake")
    set(PRUNE_SCRIPT      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/prune_output_dir.cmake")
    set(SDK_OUTPUT_DIR    "${CMAKE_SOURCE_DIR}/sdk")

    # ---- 运行时依赖：本配置的 vcpkg bin/*.dll -> bin/ ----
    # 泄漏检测同时对照 release 与 debug 两个 bin 目录（缺陷3-b）
    add_custom_target(copy_dlls ALL
        COMMAND ${CMAKE_COMMAND}
            "-DRUNTIME_BIN_DIR=${RUNTIME_BIN_DIR}"
            "-DVCPKG_BIN_DIR=${VCPKG_BIN_DIR}"
            "-DVCPKG_DEBUG_BIN_DIR=${VCPKG_DEBUG_BIN_DIR}"
            "-DAPP_OUTPUT_DIR=${APP_OUTPUT_DIR}"
            "-DPLUGINS_OUTPUT_DIR=${PLUGINS_OUTPUT_DIR}"
            -P "${COPY_DEPS_SCRIPT}"
        COMMENT "布局运行时 DLL（按配置选 debug/release）"
        VERBATIM
    )

    # ---- 产物剪枝（缺陷1 / 缺陷2 的根治）----
    # copy_dlls 是 ALL 目标，Ninja 可以把它排到链接之前（实测 [1/7]），那时宿主
    # 的宿主 pdb 还没生成，清理永远清不到。POST_BUILD 挂在目标自身的
    # 链接边上，链接完成后才执行，所以一定能清到链接器刚产出的 pdb/ilk。
    set(PRUNE_OUTPUT_COMMAND
        ${CMAKE_COMMAND}
        "-DAPP_OUTPUT_DIR=${APP_OUTPUT_DIR}"
        "-DPLUGINS_OUTPUT_DIR=${PLUGINS_OUTPUT_DIR}"
        -P "${PRUNE_SCRIPT}"
    )

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${PRUNE_OUTPUT_COMMAND}
        COMMENT "剪枝宿主输出目录：删除 *.pdb / *.ilk"
        VERBATIM
    )

    # ---- 离线 SDK 导出（手动 target：cmake --build <dir> --target export_sdk）----
    add_custom_target(export_sdk
        COMMAND ${CMAKE_COMMAND}
            "-DVCPKG_BIN_DIR=${VCPKG_BIN_DIR}"
            "-DVCPKG_LIB_DIR=${VCPKG_LIB_DIR}"
            "-DVCPKG_DEBUG_BIN_DIR=${VCPKG_DEBUG_BIN_DIR}"
            "-DVCPKG_DEBUG_LIB_DIR=${VCPKG_TRIPLET_DIR}/debug/lib"
            "-DVCPKG_INC_DIR=${VCPKG_INC_DIR}"
            "-DCONTRACT_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include"
            "-DSHOT_CMAKE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cmake"
            "-DSDK_OUTPUT_DIR=${SDK_OUTPUT_DIR}"
            -P "${EXPORT_SDK_SCRIPT}"
        COMMENT "导出离线 SDK：release 在 lib|bin，debug 在 lib\\debug|bin\\debug，均无 PDB"
        VERBATIM
    )
else()
    add_custom_target(copy_dlls ALL COMMAND ${CMAKE_COMMAND} -E echo "Non-Windows platform")
endif()
