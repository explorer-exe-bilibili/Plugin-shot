# plugin-shot

**中文** | [English](README.en.md)

> 万物皆插件的截图工具。宿主只做「宿主」该做的事——加载、校验、调度；
> 截图、图像处理、录屏……所有能力都由插件提供。

## 设计理念

核心保持极小且稳定，能力通过插件无限扩展（类似 DeepSeek Harness 的思路）。

**宿主**（当前产物名为 `Plugin-shot.exe`）负责：

- 扫描插件目录，动态加载 `.dll` / `.so` / `.dylib`
- 用 `plugin_abi_version()` 校验 ABI，版本不匹配立刻拒绝加载
- 把 `CPluginHandle` 包装成 C++ 对象，按 `PluginType` 分类索引
- 只做生命周期管理——插件对象由插件自己销毁，宿主绝不 `delete`

**插件**（动态库）负责一切具体能力。当前主线是截图，之后扩展录屏等。

## 当前状态

| 能力 | 状态 |
| --- | --- |
| 插件加载 / ABI 校验 / 按类型索引 | ✅ 可用 |
| 一 DLL 多插件（`plugin_get_count()` + `plugin_create(index)`） | ✅ 可用 |
| `ImageProcessor` 插件类型 | ✅ 可用（`plugins/example_processor` 一个 DLL 装两个插件） |
| 插件元数据贴尾（`metadata.json` → DLL 尾部，纯 CMake） | ✅ 可用（宿主尚未读取，见路线图） |
| 插件工程公共构建规则（`shot_add_plugin()`） | ✅ 可用（宿主与离线 SDK 共用） |
| 离线插件 SDK 导出（`export_sdk` 目标） | ✅ 可用 |
| `Window` / `Tray` 插件类型 | 🚧 仅有类型枚举，头文件待实现 |
| 正式截图采集流程 | 🚧 未实现（`main.cpp` 目前用 2×2 假图做冒烟验证） |
| 录屏等新插件类型 | 📋 规划中 |

> 产物名取自 `CMakeLists.txt` 的 `PROJECT_NAME`（当前 `Plugin-shot`）：宿主叫 `Plugin-shot.exe`，
> 插件落 `bin/plugins/`。应用图标缺失时 `.rc` 会自动降级成不含 ICON 行，所以改名不会把构建搞挂。

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
├── plugins/                # 每个子目录都是一个独立插件工程
│   └── example_processor/  # 完整示例：CMakeLists + metadata.json + include/ + src/
├── cmake/
│   ├── ShotPlugin.cmake        # shot_add_plugin()：插件工程公共构建规则
│   ├── AppendPluginMeta.cmake  # 把 metadata.json 贴到 DLL 尾部
│   ├── prune_output_dir.cmake  # 产物目录剪枝（不留 pdb/ilk/exp/lib）
│   └── ...                     # 运行时依赖布局、SDK 导出、链接前杀进程
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
cd build/Windows/bin && ./Plugin-shot.exe
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

跨边界头只有一个：`include/plugin_api.h`。插件必须导出四个 C 函数：

| 符号 | 作用 |
| --- | --- |
| `plugin_abi_version()` | 返回 `PLUGIN_ABI_VERSION`，不一致则宿主拒绝加载 |
| `plugin_get_count()` | 本模块里有几个插件，`<= 0` 视为空包 |
| `plugin_create(int index)` | 在插件自己的堆上分配第 `index` 个 `CPluginHandle`；越界返回 `nullptr` |
| `plugin_destroy(handle)` | 销毁句柄——**只能由插件自己调用**，宿主绝不 `delete` |

一个 DLL 就是**一个插件包**：索引工厂让宿主不必提前知道包里有什么，
`metadata.json` 的 `plugins[]` 顺序与 `plugin_create(index)` 一一对应。

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

### 元数据贴尾

`metadata.json` 由 `cmake/AppendPluginMeta.cmake` 在**链接之后**追加到 DLL 尾部：

```text
<原始二进制>
---PLUGIN_META_START---
{ "package": "example_processor", "plugins": [ ... ] }
---PLUGIN_META_END---
```

- 纯文本标记，不需要 Python、不需要贴尾小工具，宿主 `rfind` 就能取出来。
- 文件尾的额外数据不在 PE/ELF 的节表里，加载器直接忽略，不影响装载。
- 元数据跟着文件走：用户复制或重命名插件也不会出现「清单与 DLL 分家」。
- 标记字面量只在 `plugin_api.h` 里定义一次，构建脚本构建期直接读头文件取宏值。

### 写一个插件

完整样板见 [`plugins/example_processor/`](plugins/example_processor/README.md)：
一个 DLL 里装两个插件，目录结构 `CMakeLists.txt` + `metadata.json` + `include/` + `src/`。
SDK 目录说明与完整编译约束见 [`document/README_SDK.md`](document/README_SDK.md)。

```cpp
#include "plugin_api.h"

// 索引表：顺序必须与 metadata.json 的 plugins[] 一致
constexpr Entry kEntries[] = {
    {::ImageProcessor, "acme.blur",   &process_blur},
    {::ImageProcessor, "acme.sharpen", &process_sharpen},
};

extern "C" {

SCREENSHOT_API int plugin_abi_version(void) { return PLUGIN_ABI_VERSION; }

SCREENSHOT_API int plugin_get_count(void) {
    return static_cast<int>(std::size(kEntries));
}

SCREENSHOT_API CPluginHandle* plugin_create(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kEntries))) return nullptr;
    auto* handle = new CPluginHandle{};
    handle->type = kEntries[index].type;
    handle->name = kEntries[index].name;
    handle->internal_data = nullptr;
    handle->process = kEntries[index].process;
    return handle;
}

SCREENSHOT_API void plugin_destroy(CPluginHandle* handle) { delete handle; }

}
```

插件工程的 CMakeLists 只有三件事——找模块、列源文件、指元数据：

```cmake
list(APPEND CMAKE_MODULE_PATH "${HOST}/cmake")   # 或 <SDK>/cmake
include(ShotPlugin)

shot_add_plugin(acme_tools
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp src/blur.cpp src/sharpen.cpp
    INCLUDE_DIRS include)
```

`shot_add_plugin()` 统一处理 `/MD`、`PREFIX ""`、隐藏符号、产物目录、pdb 重定向、
贴元数据与产物剪枝——这些规则以前散落在根 `CMakeLists.txt` 里，容易和文档漂移。

## 路线图

- [x] 插件宿主骨架：扫描、ABI 校验、按类型索引、生命周期
- [x] `ImageProcessor` 契约 + 示例插件 + 离线 SDK 导出
- [ ] 正式截图采集（全屏 / 区域 / 窗口）与 `Window`、`Tray` 插件
- [ ] 录屏类插件（新增 `PluginType` 与对应契约，届时递增 `PLUGIN_ABI_VERSION`）
- [x] 插件元数据贴尾（纯 CMake 追加 `metadata.json`）与一 DLL 多插件
- [ ] 宿主读取元数据（校验 id/type/依赖后再 `LoadLibrary`）
- [ ] 插件热重载与插件间依赖声明

## 平台

Windows 是主要开发与验证平台（MSVC + vcpkg `x64-windows`）。
`CMakeLists.txt` 已含 macOS / Linux / WSL 分支，`dllHander.h` 也已支持 `dlopen`，但**未经验证**。

## 许可

尚未指定。
