# ============================================================================
#  screen_capture —— 屏幕采集插件（"shot.screen_capture" + "shot.configurable"）
#
#  两段式能力：describe 报告几何，capture 把 BGRA8 像素写进宿主自己分配的缓冲——
#  插件因此从不分配图像内存，也没有跨 DLL 的所有权问题。
#  另声明两个配置项：include_cursor（Windows/X11 是否合成鼠标光标）与 source
#  （Wayland 共享整个输出还是单个窗口）。
#
#  平台实现（源文件按平台选择，互不参与对方编译）：
#    Windows：GDI（GetDC + BitBlt + GetDIBits + DrawIconEx）。
#    Linux：同一份 .so 编入两条后端，运行期按会话类型选择——
#      · Wayland：xdg-desktop-portal 的 ScreenCast 门户 + PipeWire
#        （Wayland 不允许应用直接读屏幕）；
#      · X11：XShmGetImage / XGetImage 抓整个 X screen + XFixes 光标合成。
#    macOS：尚未实现，构建时跳过整个插件。
#
#  构建方式一（宿主仓库内）：
#    根 CMakeLists 用 file(GLOB plugins/*/CMakeLists.txt) 自动纳入本工程，
#    shot_add_plugin() 与 SHOT_* 变量由宿主提供，无需额外操作。
#
#  构建方式二（离线 SDK，独立构建）：
#    cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
#    模块默认取 <SDK>/cmake/../include 当契约头目录，产物落 build/bin/plugins。
# ============================================================================

set(SHOT_PLUGIN_NAME screen_capture)

# macOS 用 CGDisplayCreateImage / ScreenCaptureKit 实现，尚未补齐；在补齐之前直接
# 跳过整个插件，而不是留一份跑不起来的 stub。被宿主 add_subdirectory 时用
# PARENT_SCOPE 告诉根工程"这个插件被跳过了"（根 CMakeLists 据此打印提示）。
#  TODO: macOS 用 ScreenCaptureKit 实现，并在 CMake 里加 APPLE 源与对应 framework。
if(APPLE)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)
    endif()
    message(STATUS "screen_capture：macOS（ScreenCaptureKit）待补，当前平台跳过")
    return()
endif()

# 独立构建时才自成一个 CMake 项目；被宿主 add_subdirectory 时不重复声明。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(${SHOT_PLUGIN_NAME} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# 宿主仓库内构建时下面的路径指向 <repo>/cmake；SDK 独立构建时把 <SDK>/cmake
# 加进 CMAKE_MODULE_PATH 即可（见上方说明）。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")

if(NOT COMMAND shot_add_plugin)
    include(ShotPlugin)
endif()

# header-only 的 nlohmann-json：解析 i18n 语言表；插件不链接 spdlog（日志走宿主服务）。
find_package(nlohmann_json CONFIG REQUIRED)

# 平台相关的源文件与系统库。Windows 的 GDI 源保持原样；Linux 同时编译 Wayland
# 的"门户 / PipeWire / 帧缓冲"与 X11 的"抓取 / 像素转换 / 光标"两条后端。
set(_screen_capture_sources)
set(_screen_capture_libraries nlohmann_json::nlohmann_json)
set(_screen_capture_include_dirs)

if(WIN32)
    set(_screen_capture_sources
        src/plugin_entry.cpp
        src/screen_capture.cpp
        src/capture_settings.cpp
        src/gdi_desktop_capture.cpp
    )
    #   gdi32  —— BitBlt / CreateCompatibleDC / GetDIBits / DeleteObject 等 GDI 对象
    #   user32 —— GetDC / GetSystemMetrics / GetCursorInfo / GetIconInfo / DrawIconEx
    list(APPEND _screen_capture_libraries gdi32 user32)
elseif(UNIX AND NOT APPLE)
    # Linux 只出一份 .so：Wayland（门户 + PipeWire）与 X11（XShm/XGetImage）都在
    # 同一个二进制里，运行期按会话类型选后端。所以两条后端的开发包都必须存在，
    # 缺一个就在 configure 阶段失败，而不是让插件到运行期才发现没有可用的后端。
    #
    #   libportal-1.0      —— ScreenCast 门户的异步封装（CreateSession/SelectSources/Start）
    #   libpipewire-0.3    —— 接收门户打开的视频流（BGRx/RGBx，MemFd）
    #   x11                —— XOpenDisplay / XGetImage / XDestroyImage、屏幕几何
    #   xext               —— MIT-SHM 的 XShmGetImage（头文件来自 libxext-dev）
    #   xrandr             —— 只在 debug 日志里列出显示器布局（XRandR 1.5+，可选路径）
    #   xfixes             —— XFixesGetCursorImage：光标位图 + 热点，用于光标合成
    #
    # 不用 pkg_check_modules(... REQUIRED ...)：它的默认报错只说"找不到模块"，
    # 不会告诉用户装哪个包；先 QUIET 查一次再自己 FATAL_ERROR 信息更可执行。
    find_package(PkgConfig REQUIRED)
    # 模块名在不同发行版里不一样：Ubuntu 24.04 的 libportal-dev 提供 libportal.pc，
    # 上游/其它发行版可能是 libportal-1.0.pc，所以用 pkg_search_module 两个都试。
    pkg_search_module(LIBPORTAL QUIET libportal libportal-1.0)
    if(NOT LIBPORTAL_FOUND)
        message(FATAL_ERROR
            "screen_capture：缺少 libportal（Wayland 的 ScreenCast 门户依赖）。\n"
            "  Ubuntu/Debian 安装：sudo apt install libportal-dev libpipewire-0.3-dev\n"
            "  （试过的 pkg-config 模块：libportal / libportal-1.0）")
    endif()
    pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
    if(NOT PIPEWIRE_FOUND)
        message(FATAL_ERROR
            "screen_capture：缺少 PipeWire 开发文件（Wayland 视频流依赖）。\n"
            "  Ubuntu/Debian 安装：sudo apt install libportal-dev libpipewire-0.3-dev\n"
            "  （缺失的 pkg-config 模块：libpipewire-0.3）")
    endif()

    # X11 后端依赖。xext 是 MIT-SHM 的客户端库（XShmAttach/XShmGetImage），
    # libxext-dev 的 XShm.h 通常由 libxrandr-dev 的依赖链带进来，但链接时必须
    # 显式加 -lXext，所以这里也单独查一次。
    pkg_check_modules(X11 QUIET x11)
    pkg_check_modules(XRANDR QUIET xrandr)
    pkg_check_modules(XFIXES QUIET xfixes)
    pkg_check_modules(XEXT QUIET xext)
    set(_x11_missing "")
    if(NOT X11_FOUND)
        list(APPEND _x11_missing x11)
    endif()
    if(NOT XRANDR_FOUND)
        list(APPEND _x11_missing xrandr)
    endif()
    if(NOT XFIXES_FOUND)
        list(APPEND _x11_missing xfixes)
    endif()
    if(NOT XEXT_FOUND)
        list(APPEND _x11_missing xext)
    endif()
    if(_x11_missing)
        string(JOIN " " _x11_missing_text ${_x11_missing})
        message(FATAL_ERROR
            "screen_capture：缺少 X11 开发文件（Linux X11 会话的采集依赖）。\n"
            "  Ubuntu/Debian 安装：sudo apt install libx11-dev libxrandr-dev libxfixes-dev "
            "libxext-dev pkg-config\n"
            "  （缺失的 pkg-config 模块：${_x11_missing_text}；"
            "libxext-dev 通常随 libxrandr-dev 一起装）")
    endif()

    find_package(Threads REQUIRED)

    set(_screen_capture_sources
        src/plugin_entry.cpp
        src/screen_capture_linux.cpp
        src/capture_settings.cpp
        src/wayland_capture.cpp
        src/wayland_portal_session.cpp
        src/wayland_portal_callbacks.cpp
        src/wayland_pipewire_stream.cpp
        src/wayland_frame_buffer.cpp
        src/x11_capture.cpp
        src/x11_image_convert.cpp
        src/x11_cursor.cpp
    )
    list(APPEND _screen_capture_libraries
        ${LIBPORTAL_LINK_LIBRARIES}
        ${PIPEWIRE_LINK_LIBRARIES}
        ${X11_LINK_LIBRARIES}
        ${XRANDR_LINK_LIBRARIES}
        ${XFIXES_LINK_LIBRARIES}
        ${XEXT_LINK_LIBRARIES}
        Threads::Threads
    )
    set(_screen_capture_include_dirs
        ${LIBPORTAL_INCLUDE_DIRS}
        ${PIPEWIRE_INCLUDE_DIRS}
        ${X11_INCLUDE_DIRS}
        ${XRANDR_INCLUDE_DIRS}
        ${XFIXES_INCLUDE_DIRS}
        ${XEXT_INCLUDE_DIRS}
    )
else()
    message(FATAL_ERROR "screen_capture：当前平台既不是 Windows 也不是 Linux/UNIX，尚未实现")
endif()

shot_add_plugin(${SHOT_PLUGIN_NAME}
    METADATA     metadata.json
    SOURCES      ${_screen_capture_sources}
    INCLUDE_DIRS include ${_screen_capture_include_dirs}
    LINK_LIBRARIES
                 ${_screen_capture_libraries}
)

if(UNIX AND NOT APPLE)
    # pkg-config 的库目录不在链接器默认搜索路径时（自定义 prefix / 交叉编译）显式
    # 告诉链接器；发行版默认装在 /usr/lib 时这些变量是空串，等于没有影响。
    target_link_directories(${SHOT_PLUGIN_NAME} PRIVATE
        ${LIBPORTAL_LIBRARY_DIRS}
        ${PIPEWIRE_LIBRARY_DIRS}
        ${X11_LIBRARY_DIRS}
        ${XRANDR_LIBRARY_DIRS}
        ${XFIXES_LIBRARY_DIRS}
        ${XEXT_LIBRARY_DIRS}
    )
endif()

# 新插件要求 /W4 干净；不加 /WX，避免不同 MSVC 小版本的第三方头差异卡死构建。
if(MSVC)
    target_compile_options(${SHOT_PLUGIN_NAME} PRIVATE /W4)
endif()
