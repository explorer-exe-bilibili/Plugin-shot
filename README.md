# plugin-shot

**中文** | [English](README.en.md)

> 万物皆插件的截图工具。宿主只做「宿主」该做的事——加载、校验、调度；
> 截图、图像处理、录屏……所有能力都由插件提供。

## 设计理念

核心保持极小且稳定，能力通过插件无限扩展（类似 DeepSeek Harness 的思路）。

**宿主**（当前产物名为 `Plugin-shot.exe`）负责：

- 扫描插件目录，动态加载 `.dll` / `.so` / `.dylib`
- 只解析**一个**导出符号 `shot_module_entry`，拿到自描述的模块表
- 先看 `struct_size` 再读字段、比对 `abi_major`，不兼容立刻拒绝加载
- 用 `query_interface("shot.image_processor", 1)` 问能力，不认识的能力安静跳过
- 把实例包装成 C++ 对象，按 `PluginType` 分类索引；插件对象由插件自己销毁，宿主绝不 `delete`

**插件**（动态库）负责一切具体能力。当前主线是截图，之后扩展录屏等。

## 当前状态

| 能力 | 状态 |
| --- | --- |
| 插件加载 / 长度与版本校验 / 按类型索引 | ✅ 可用 |
| 单入口 `shot_module_entry` + 自描述模块表（`struct_size` 约定） | ✅ 可用 |
| 一 DLL 多插件（`plugin_count` + `create(index)` + `get_info`） | ✅ 可用 |
| 能力查询 `query_interface("shot.image_processor", 1)` | ✅ 可用 |
| 运行库提供者 `shot.lib_provider`（`PluginType::Lib`） | ✅ 可用（`plugins/opencv_provider` 把 OpenCV 带进进程） |
| 插件配置 `shot.configurable`（插件声明、宿主渲染与存盘） | ✅ 契约与示例可用（宿主界面未做） |
| 插件元数据贴尾（`metadata.json` → DLL 尾部，纯 CMake） | ✅ 可用（宿主尚未读取，见路线图） |
| 元数据构建期校验（id 唯一 / interfaces 已注册 / abi 与头一致 / 依赖语法） | ✅ 可用（`cmake/ValidatePluginMeta.cmake`） |
| 插件级依赖声明（`requires` / `provides`，带版本区间，`core` 为宿主） | ✅ 可用（`document/METADATA.md`） |
| 依���驱动加载顺序（拓扑排序，不满足只跳过该插件） | ✅ 可用 |
| 插件工程公共构建规则（`shot_add_plugin()`） | ✅ 可用（宿主与离线 SDK 共用） |
| 离线插件 SDK 导出（`export_sdk` 目标） | ✅ 可用 |
| `Window` / `Tray` 能力 | 📋 只需新增 interface id + 结构体，契约不用动 |
| 正式截图采集流程 | 🚧 未实现（`main.cpp` 目前用 2×2 假图做冒烟验证） |
| 录屏等新能力 | 📋 只需新增 interface id，契约不用动 |

> 产物名取自 `CMakeLists.txt` 的 `PROJECT_NAME`（当前 `Plugin-shot`）：宿主叫 `Plugin-shot.exe`，
> 插件落 `bin/plugins/`。应用图标缺失时 `.rc` 会自动降级成不含 ICON 行，所以改名不会把构建搞挂。

## 目录结构

```
.
├── CMakeLists.txt          # 跨平台构建脚本（vcpkg 清单模式 + OpenCV）
├── vcpkg.json              # 依赖清单
├── include/
│   ├── plugin_api.h          # ★ 宿主 ⇄ 插件的 ABI 契约（唯一的跨边界头，纯 C）
│   ├── plugin_api_export.h   # 导出宏：插件默认 dllexport，宿主定义 SCREENSHOT_HOST_EXPORTS
│   ├── shot_plugin_helpers.h # 插件侧小工具：能力目录 + shot_query_interface + SHOT_OPS
│   ├── dllHander.h           # LoadLibrary / dlopen 封装（含不抛异常的 tryGetFunction）
│   ├── pluginManager.h       # 插件扫描、长度/版本校验、按类型索引
│   └── PluginInstances/      # 宿主侧插件对象包装（base / ImageProcessor / LibProvider …）
├── src/                      # 宿主实现与入口
├── plugins/                  # 每个子目录都是一个独立插件工程
│   ├── example_processor/    # 完整示例：一个 DLL 两个插件，都带配置项
│   └── opencv_provider/      # 合法但零能力的插件包（把 OpenCV 带进进程）
├── document/
│   ├── README_SDK.md         # SDK 使用与完整编译约束
│   ├── ABI_POLICY.md         # 只追加规则、interface id 登记表
│   └── METADATA.md           # metadata.json 的字段定义、权威规则与不写什么
├── cmake/
│   ├── ShotPlugin.cmake        # shot_add_plugin()：插件工程公共构建规则
│   ├── AppendPluginMeta.cmake  # 把 metadata.json 贴到 DLL 尾部
│   ├── prune_output_dir.cmake  # 产物目录剪枝（不留 pdb/ilk/exp/lib/基础依赖 DLL）
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

跨边界头只有一个：`include/plugin_api.h`（纯 C，不依赖 STL）。整个插件包只导出**一个** C 函数：

```c
SCREENSHOT_API const ShotModule* shot_module_entry(void);
```

它返回一张自描述的模块表 `ShotModule`，其余一切都在表里：

| 字段 | 作用 |
| --- | --- |
| `struct_size` / `abi_major` / `abi_minor` | 自描述长度 + 版本：宿主先看长度再读字段，然后比对 major |
| `plugin_count` | 本 DLL 里有几个插件 |
| `get_info(index, info)` | 不创建实例就能拿到 id / 名称 / 版本 |
| `create(index, host)` | 在插件自己的堆上分配实例，返回不透明指针 |
| `destroy(instance)` | 销毁实例——**只能由插件自己调用**，宿主绝不 `delete` |
| `query_interface(instance, id, version)` | 问"你会不会这个能力" |
| `init(host, flags)` / `shutdown()` | 可选：模块级初始化与收尾，`nullptr` 表示不需要 |

一个 DLL 就是**一个插件包**：`plugin_count` + `create(index)` 让宿主不必提前知道包里有什么，
`metadata.json` 的 `plugins[]` 顺序与索引一一对应。

### 三条让 ABI 不必再改的规则

1. **只加字段，不加符号**：整个包只有 `shot_module_entry` 一个导出。加能力靠往结构体末尾**追加字段**，不是加导出函数。
2. **读字段前先问长度**：每个跨边界结构体第一个字段都是 `struct_size`，读取方一律先过 `SHOT_HAS_FIELD(ptr, type, field)`。于是老插件（短结构）与新宿主（想读新字段）能安全共存——**加字段永远不需要改版本号**。
3. **能力用字符串 id 问**：`query_interface(instance, "shot.image_processor", 1)` 返回函数表，没有就返回 `NULL`，宿主安静跳过。新增一类能力 = 新增一个 id + 一个结构体，不动任何已有结构体。

`plugin_api.h` 末尾用 `static_assert` 钉住了每个结构体的字段偏移与总大小：
重排或改类型会直接编译失败，而"追加字段"必须手动更新那一行——把破坏 ABI 变成一次有意识的行为。

### 已注册的能力

| interface id | 结构体 | 作用 |
| --- | --- | --- |
| `shot.image_processor` | `ShotImageProcessorV1` | 处理一张图：`process(self, ShotImage*)`，返回 0 表示成功 |
| `shot.configurable` | `ShotConfigurableV1` | 声明并读写配置项，见下 |
| `shot.lib_provider` | `ShotLibProviderV1` | 声明"我是运行库提供者"，报告库名与版本；宿主按 `PluginType::Lib` 登记 |

`ShotImage` 描述一块 8 位像素缓冲：

- `pixel_format` 显式声明布局（当前只有 `SHOT_PIXEL_FORMAT_BGRA8`），不再靠 `channels` 猜
- `stride` 以字节计，**可以为负**（自底向上存储），也可大于 `width * channels`（行尾填充）
- 所以必须按行寻址，不能假设 `data` 是连续块

### 配置：插件声明，宿主渲染与存盘

`shot.configurable` 让插件自己说清"我有哪些配置项"，宿主据此生成界面、保存文件、灌值——
**宿主不硬编码任何配置项名字**：

```c
typedef struct ShotConfigurableV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t (*get_config_count)(void* self);
    int32_t  (*get_config_desc)(void* self, uint32_t index, ShotConfigDesc* out);
    int32_t  (*get_config)(void* self, const char* key, char* buf, uint32_t buf_size);
    int32_t  (*set_config)(void* self, const char* key, const char* value);
} ShotConfigurableV1;
```

- 配置项由 `ShotConfigDesc` 描述：`kind`（BOOL / INT / FLOAT / STRING / ENUM / COLOR / PATH）、
  `key`、显示名、默认值、取值范围、枚举候选。
- **值一律是字符串**：跨边界没有宽度与对齐问题，天然可以存成 JSON，插件也不必为读配置引入 STL。
- 插件遇到不认识的 `key` 必须安全忽略（返回负值）——宿主升级后多传一项，老插件也不会出问题。
- 约定：`set_config` 可能在运行中被调用（用户拖滑块），实现要么线程安全，要么声明"下次 `process` 生效"。

**宿主级别的设置不要塞进插件**：输出格式、保存目录、截图热键属于宿主；插件只管自己的算法参数。

### 必须遵守的 ABI 约定

1. **运行库必须是 /MD（`MultiThreadedDLL`）**，宿主与插件一致；`/MT` 会得到两份 CRT。
2. **Debug/Release 必须与宿主一致**——Debug 插件只能加载进 Debug 宿主。
3. **插件不得自带基础依赖**（`opencv_*.dll` 等）：宿主 exe 旁边的 `bin/` 里已经有一套，插件目录里再放一份会让同一进程出现两个 OpenCV，类型信息、全局状态与内存分配器全部割裂。

   > 宿主本身**不链接** OpenCV（`CMakeLists.txt` 只链接 `nlohmann_json`）。`bin/` 里的
   > `opencv_*.dll` 来自 `cmake/copy_runtime_deps.cmake` 对 vcpkg 运行时目录的整目录布置，
   > 不是链接拉进来的。需要 OpenCV 的插件在**自己的** `CMakeLists.txt` 里
   > `find_package(OpenCV QUIET)` 并链接（见 `plugins/opencv_provider`），
   > 运行时从 `bin/` 里那一份解析到同一个 DLL。
4. **跨边界不要传 STL 容器 / `std::string` / `cv::Mat`**，也不要传带虚函数的 C++ 对象：跨边界只有 POD 结构体 + 函数指针。
5. **只用定宽类型**（`uint32_t` / `int32_t` / `double`），不用 `int` / `long` / `size_t`；枚举不进结构体，改用定宽常量。
6. **异常绝不跨越 DLL 边界**：每个 `extern "C"` 函数内部自行兜底，错误用返回值表达。
7. **`abi_major` 必须与宿主一致**；只追加能力时提高 `abi_minor` 即可，宿主接受 minor ≤ 自己的插件。
8. **结构体只能末尾追加**：永不重排、永不改类型、永不删，作废字段改名 `retired_xxx` 占位。

### 元数据贴尾

`metadata.json` 由 `cmake/AppendPluginMeta.cmake` 在**链接之后**追加到 DLL 尾部：

```text
<原始二进制>
---PLUGIN_META_START---
{ "schema": 1, "abi": { ... }, "plugins": [ ... ] }
---PLUGIN_META_END---
```

- 纯文本标记，不需要 Python、不需要贴尾小工具，宿主 `rfind` 就能取出来。
- 文件尾的额外数据不在 PE/ELF 的节表里，加载器直接忽略，不影响装载。
- 元数据跟着文件走：用户复制或重命名插件也不会出现「清单与 DLL 分家」。
- 标记字面量只在 `plugin_api.h` 里定义一次，构建脚本构建期直接读头文件取宏值。
- 贴尾之前会先校验：`plugins[]` 非空、`id` 唯一、`interfaces` 里的 id 在契约头里有定义、
  `abi` 与 `SHOT_ABI_*` 一致、`package` 与目标名一致。写错就构建失败，不用等宿主发现。
- 字段定义、权威规则（谁是权威、为什么不写配置项、依赖怎么解析）见 [`document/METADATA.md`](document/METADATA.md)。
- 依赖是**插件级**的：`"requires": ["core>=0.1.0", "opencv>=4.12"]`，`core` 指宿主自己的版本
  （来自 CMake 生成的 `version.h`）。宿主先读所有 DLL 的元数据建依赖图，拓扑排序后加载，
  不满足的插件**只跳过它自己**，同包其它插件和整个宿主继续工作。

### 写一个插件

一个插件一个 `.cpp`：状态、`process`、可选的配置读写，最后组装能力函数表：

```cpp
// plugins/acme_tools/src/blur.cpp
static int32_t blur_process(void* self, ShotImage* image) {
    auto* state = static_cast<BlurState*>(state_of(self));   // 注意：self 是不透明指针
    // … 按行处理 image …
    return 0;                                                 // 0 = 成功
}

const ShotImageProcessorV1 kImageOps =
    SHOT_OPS(ShotImageProcessorV1, .process = &blur_process);

const ShotIfaceEntry kIfaces[] = {                            // 能力目录
    { SHOT_IFACE_IMAGE_PROCESSOR, 1, &kImageOps },
    { nullptr, 0, nullptr },                                  // 结束标记
};
```

`plugin_entry.cpp` 只做三件事：把各插件登记成 `kEntries[]`、转发 `query_interface`、返回模块表：

```cpp
extern "C" {

SCREENSHOT_API const ShotModule* shot_module_entry(void) {
    static const ShotModule module = {
        .struct_size     = sizeof(ShotModule),
        .abi_major       = SHOT_ABI_MAJOR,
        .abi_minor       = SHOT_ABI_MINOR,
        .plugin_count    = kEntryCount,
        .get_info        = &get_info,
        .create          = &create_instance,
        .destroy         = &destroy_instance,
        .query_interface = &query_instance,   // 转发 shot_query_interface(ifaces, id, ver)
    };
    return &module;
}

}
```

完整可编译样板见 [`plugins/example_processor/`](plugins/example_processor/README.md)：一个 DLL 装两个插件、
都带配置项，目录结构 `CMakeLists.txt` + `metadata.json` + `include/` + `src/`。
SDK 说明见 [`document/README_SDK.md`](document/README_SDK.md)，
只追加规则与 interface id 登记表见 [`document/ABI_POLICY.md`](document/ABI_POLICY.md)。

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

- [x] 插件宿主骨架：扫描、长度/版本校验、按类型索引、生命周期
- [x] 单入口 `shot_module_entry` + 自描述模块表 + 能力查询 `query_interface`
- [x] `shot.image_processor` 与 `shot.configurable` 契约、示例插件、离线 SDK 导出
- [x] 插件元数据贴尾（纯 CMake 追加 `metadata.json`）与一 DLL 多插件
- [ ] 宿主侧通用配置界面（读 `ShotConfigDesc` 自动生成）与配置存盘
- [x] 宿主读取元数据（加载前预筛 + 依赖拓扑排序 + 不满足的插件单独跳过）
- [x] 元数据与 `get_info` 的交叉校验（id / name / version 不一致时告警，以 `get_info` 为准）
- [ ] 正式截图采集（全屏 / 区域 / 窗口）与 `Window`、`Tray` 能力
- [ ] 录屏能力（新增 interface id，契约本身不用改）
- [ ] 插件热重载与插件间依赖声明

## 平台

Windows 是主要开发与验证平台（MSVC + vcpkg `x64-windows`）。
`CMakeLists.txt` 已含 macOS / Linux / WSL 分支，`dllHander.h` 也已支持 `dlopen`，但**未经验证**。

## 许可

尚未指定。
