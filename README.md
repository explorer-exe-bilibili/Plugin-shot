# plugin-shot

**中文** | [English](README.en.md)

> 万物皆插件的截图工具。宿主只做「宿主」该做的事——加载、校验、调度；
> 截图、图像处理、录屏……所有能力都由插件提供。

## 设计理念

核心保持极小且稳定，能力通过插件无限扩展（类似 DeepSeek Harness 的思路）。

**宿主**（当前产物名为 `my-project.exe`）负责：

- 扫描插件目录，动态加载 `.dll` / `.so` / `.dylib`
- 用 `plugin_abi_version()` 校验 ABI，版本不匹配立刻拒绝加载
- 把 `CPluginHandle` 包装成 C++ 对象，按 `PluginType` 分类索引
- 只做生命周期管理——插件对象由插件自己销毁，宿主绝不 `delete`

**插件**（动态库）负责一切具体能力。当前主线是截图，之后扩展录屏等。

## 当前状态

| 能力 | 状态 |
| --- | --- |
| 插件加载 / ABI 校验 / 按类型索引 | ✅ 可用 |
| `ImageProcessor` 插件类型 | ✅ 可用（`plugins/example_processor` 为最小示例） |
| 离线插件 SDK 导出（`export_sdk` 目标） | ✅ 可用 |
| `Window` / `Tray` 插件类型 | 🚧 仅有类型枚举，头文件待实现 |
| 正式截图采集流程 | 🚧 未实现（`main.cpp` 目前用 2×2 假图做冒烟验证） |
| 录屏等新插件类型 | 📋 规划中 |

> ⚠️ `CMakeLists.txt` 里的 `PROJECT_NAME` 仍是模板占位值 `my-project`，所以产物叫
> `my-project.exe`。改名只需改这一个变量。

## 目录结构

```
.
├── CMakeLists.txt          # 跨平台构建脚本（vcpkg 清单模式 + OpenCV）
├── vcpkg.json              # 依赖清单
├── include/
│   ├── plugin_api.h        # ★ 宿主 ⇄ 插件的 ABI 契约（唯一的跨边界头）
│   ├── plugin_api_export.h # 导出宏：插件默认 dllexport，宿主定义 SCREENSHOT_HOST_EXPORTS
│   ├── dllHander.h         # LoadLibrary / dlopen 封装
│   ├── pluginManager.h     # 插件扫描、ABI 校验、按类型索引
│   └── PluginInstances/    # 宿主侧插件对象包装（base / ImageProcessor / window / tray）
├── src/                    # 宿主实现与入口
├── plugins/
│   └── example_processor/  # 最小示例插件：RGB 取反，保留 alpha
├── cmake/                  # 运行时依赖布局、产物剪枝、SDK 导出、链接前杀进程
└── sdk/                    # export_sdk 生成的离线 SDK（不入库，见 .gitignore）
```

## 构建

前置：CMake ≥ 3.15、支持 C++23 的编译器（Windows 上为 MSVC）、[vcpkg](https://github.com/microsoft/vcpkg)。

```bash
# 工具链优先读 $VCPKG_ROOT / $VCPKG_INSTALLATION_ROOT，都没有则回落到 /opt/vcpkg
export VCPKG_ROOT=/path/to/vcpkg      # Windows: set VCPKG_ROOT=D:\vcpkg

cmake -S . -B build/Windows -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows

# 导出离线 SDK（供第三方插件在不装 OpenCV 的机器上编译）
cmake --build build/Windows --target export_sdk
```

首次 configure 会以清单模式自动安装 OpenCV（`opencv4` + jpeg/png/tiff/webp 特性），耗时较长。

### 运行

```bash
cd build/Windows/bin && ./my-project.exe
```

插件目录按相对路径 `./plugins` 解析，因此**必须在 `bin/` 下启动**。

### 产物布局（硬约束）

| 路径 | 内容 |
| --- | --- |
| `bin/` | 宿主 exe 与本配置的运行时依赖 DLL（**禁止出现 `*.pdb` / `*.ilk`**） |
| `bin/plugins/` | 只放插件自身产物（**禁止出现 OpenCV 等基础依赖**） |
| `build/<platform>/pdb/` | 全部调试符号 |
| `sdk/` | 离线 SDK：`include/`、`lib/`(release)、`lib/debug/`、`bin/`、`bin/debug/` |

这些约束由 `cmake/copy_runtime_deps.cmake` 与 `cmake/prune_output_dir.cmake` 在构建期强制。
理由见下文契约第 3 条：插件一旦自带 OpenCV，同一进程就会出现两份实例。

## 插件契约

跨边界头只有一个：`include/plugin_api.h`。插件必须导出三个 C 函数：

| 符号 | 作用 |
| --- | --- |
| `plugin_abi_version()` | 返回 `PLUGIN_ABI_VERSION`，不一致则宿主拒绝加载 |
| `plugin_create_processor()` | 在插件自己的堆上分配 `CPluginHandle` 并返回 |
| `plugin_destroy(handle)` | 销毁句柄——**只能由插件自己调用**，宿主绝不 `delete` |

`CPluginHandle` 携带 `type`（`Window` / `Tray` / `ImageProcessor`）、`name`、
`internal_data`，以及 `process(internal_data, Image*) -> int`（非 0 表示成功）。

`Image` 描述一块 8 位像素缓冲：

- `width` / `height` 是像素尺寸，`channels` 是每像素通道数（截图场景恒为 4，即 BGRA）
- `stride` 以字节计，**可以为负**（自底向上存储），也可大于 `width * channels`（行尾填充）
- 所以必须按行寻址，不能假设 `data` 是连续块

### 必须遵守的 ABI 约定

1. **运行库必须是 /MD（`MultiThreadedDLL`）**，宿主与插件一致；`/MT` 会得到两份 CRT。
2. **Debug/Release 必须与宿主一致**——Debug 插件只能加载进 Debug 宿主。
3. **插件不得自带基础依赖**（`opencv_*.dll` 等）：宿主启动时已加载那一份，插件目录里再放一份会让同一进程出现两个 OpenCV，类型信息、全局状态与内存分配器全部割裂。
4. **跨边界不要传 STL 容器 / `std::string` / `cv::Mat`**，除非确认两边 CRT 完全相同；跨边界传的是 C++ vtable，不是纯 C 结构体。
5. **`PLUGIN_ABI_VERSION` 必须与宿主一致**；升级 SDK 时整体替换 `include/`，不要新旧混用。

### 写一个插件

最小可用实现见 [`plugins/example_processor/main.cpp`](plugins/example_processor/main.cpp)（不足 60 行）。
SDK 目录说明与完整编译约束见 [`sdk/README.md`](document/README_SDK.md)。

```cpp
#include "plugin_api.h"

// int process_blur(void* internal_data, Image* image);  // 非 0 表示成功

extern "C" {

SCREENSHOT_API int plugin_abi_version(void) { return PLUGIN_ABI_VERSION; }

SCREENSHOT_API CPluginHandle* plugin_create_processor(void) {
    auto* handle = new CPluginHandle{};
    handle->type = PluginType::ImageProcessor;
    handle->name = "acme.blur";
    handle->internal_data = nullptr;
    handle->process = &process_blur;
    return handle;
}

SCREENSHOT_API void plugin_destroy(CPluginHandle* handle) { delete handle; }

}
```

CMake 侧指到 `bin/plugins/` 即可，注意 `PREFIX ""` 与运行库设置：

```cmake
add_library(my_plugin SHARED main.cpp)
set_target_properties(my_plugin PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    RUNTIME_OUTPUT_DIRECTORY "${HOST_BIN}/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${HOST_BIN}/plugins"
    MSVC_RUNTIME_LIBRARY "MultiThreaded<$<$<CONFIG:Debug>:Debug>DLL")
target_include_directories(my_plugin PRIVATE "${SDK}/include" "${SDK}/include/opencv4")
```

## 路线图

- [x] 插件宿主骨架：扫描、ABI 校验、按类型索引、生命周期
- [x] `ImageProcessor` 契约 + 示例插件 + 离线 SDK 导出
- [ ] 正式截图采集（全屏 / 区域 / 窗口）与 `Window`、`Tray` 插件
- [ ] 录屏类插件（新增 `PluginType` 与对应契约，届时递增 `PLUGIN_ABI_VERSION`）
- [ ] 插件元数据（版本、作者、依赖声明）与热重载

## 平台

Windows 是主要开发与验证平台（MSVC + vcpkg `x64-windows`）。
`CMakeLists.txt` 已含 macOS / Linux / WSL 分支，`dllHander.h` 也已支持 `dlopen`，但**未经验证**。

## 许可

尚未指定。
