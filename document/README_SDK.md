# 插件开发 SDK

离线 SDK，供第三方插件在**不安装 OpenCV** 的机器上编译链接。
当前只覆盖 Windows（MSVC / vcpkg `x64-windows`）。

## 目录

| 目录 | 内容 | 用途 |
| --- | --- | --- |
| `include/` | `plugin_api.h`（契约汇总头，只做 include）、`plugin_api/`（契约正文，每类一个子头）、`plugin_api_export.h`、`shot_plugin_helpers.h`（汇总头，只做 include）、`shot_plugin_helpers/`（每类一个头文件）、`opencv4/` | 契约头 + 插件侧小工具（能力目录 / `shot::Logger` / `shot::Config` / `shot::EventBus` / `shot::PluginLists` / `shot::PluginRegistry`） + OpenCV 头 |
| `lib/` | release 版 `*.lib` 导入库（OpenCV + 图像编解码/压缩传递依赖） | Release 构建链接期使用 |
| `lib/debug/` | debug 版 `*.lib`（`opencv_*4d.lib` 等） | Debug 构建链接期使用 |
| `bin/` | release 版 `*.dll` 运行时库（同一份白名单） | **不要拷进插件目录**，见下 |
| `bin/debug/` | debug 版 `*.dll`（`opencv_*4d.dll` 等） | Debug 宿主/插件运行时使用 |
| `cmake/` | `ShotPlugin.cmake`、`AppendPluginMeta.cmake`、`ValidatePluginMeta.cmake`、`ReadContractHeader.cmake`、`prune_output_dir.cmake` | 用 `shot_add_plugin()` 构建插件工程（含 metadata 校验） |

`lib/` 与 `bin/` 只导出 OpenCV 以及 OpenCV 图像编解码/压缩必需的传递依赖
（jpeg / libpng16 / tiff / webp / zlib / lzma / sharpyuv / turbojpeg）。
宿主私有依赖（fmt / spdlog / glfw3dll / imgui / nfd）**不会**出现在 SDK 里：
它们不是 SDK 承诺的公共能力；第三方插件需要时请自带，不要指望宿主目录里恰好有。

`include/plugin_api/` 是契约正文：版本宏与读取规则（`basics.h`）、interface id
（`interfaces.h`）、各能力与宿主服务（`image_processor.h` / `host_services.h` …）、
布局锚点（`abi_layout.h`）。插件源码**不要**直接 include 子头，照旧只写
`#include "plugin_api.h"`——汇总头会按依赖顺序把子头全部带进来。

## 编译

> **依赖**：插件汇总头 `shot_plugin_helpers.h` 会包含 `shot_plugin_helpers/shot_i18n.h`，
> 后者用 header-only 的 **nlohmann-json** 解析多语言表（宿主仓库的 `document/I18N.md`）。用本 SDK
> 的插件工程需要自己提供 nlohmann-json（vcpkg 清单加 `"nlohmann-json"`，或把
> `single_include/nlohmann/json.hpp` 放进 include 路径）。宿主仓库内的插件已由根
> `vcpkg.json` 提供。如果插件要落盘 PNG/JPEG（例如 `image_saver`），还需要
> **stb**（vcpkg 的 `"stb"` 端口，或把 `stb_image.h` / `stb_image_write.h` 放进 include
> 路径）；SDK 不内置这两个第三方头，和 nlohmann-json 一样由插件工程自备。

最省事的方式是用 SDK 自带的构建模块（宿主仓库的 `plugins/example_processor` 就是完整样板）：

```cmake
list(APPEND CMAKE_MODULE_PATH "<SDK>/cmake")
include(ShotPlugin)

shot_add_plugin(my_plugin
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp src/my_processor.cpp
    INCLUDE_DIRS include
    LINK_LIBRARIES opencv_core4 opencv_imgproc4   # 可选；Debug 换成 opencv_core4d / opencv_imgproc4d
)
```

`shot_add_plugin()` 统一处理 /MD、隐藏符号、产物目录、元数据贴尾与产物剪枝。

**跨平台**：这份 SDK 的离线 **导出**目前只在 Windows 提供（`export_sdk` 是 `WIN32` 分支）；
宿主本身可以在 Linux 构建——宿主仓库根目录的 `CMakePresets.json` + `.github/workflows/cmake.yml`
以 Linux / macOS / Windows 为目标，Linux 已在 WSL2 Ubuntu 24.04.5 / GCC 13.3.0 / x64-linux
实测 configure + build 通过（0 warning，产物与冒烟结果见宿主仓库的 `README.md`「平台」段）；
macOS 尚无真机构建结论。CI workflow 已就绪（Windows job 已跑绿），Linux / macOS job 仍待
下一轮运行确认。

插件里访问宿主服务用 `shot_plugin_helpers.h` 的封装：`shot::Logger`（写宿主日志）、
`shot::Config`（读写宿主配置）、`shot::EventBus`（主题发布/订阅）、`shot::PluginLists`
（读取宿主聚合的列表 JSON 快照，如 `config_list` / `tray_command_list`）、`shot::PluginRegistry`
（插件启停）。汇总头本身只做 include，每个类各有一个头文件（在 `shot_plugin_helpers/` 子目录）；
它们对老宿主自动降级，不需要逐处手写 `SHOT_HAS_FIELD` 判空。
手工搭建时，等价的最小配置是按**配置**选择链接目录与库名（不能写死 `<SDK>/lib`）：

```cmake
include_directories(<SDK>/include <SDK>/include/opencv4)

# Debug 链接 <SDK>/lib/debug 的 opencv_*4d.lib，Release 链接 <SDK>/lib 的
# opencv_*4.lib。两个变体混用会链接通过、运行期找不到另一个后缀的 DLL。
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SDK_LIB_DIR "<SDK>/lib/debug")
    set(SDK_OPENCV_LIBS opencv_core4d opencv_imgproc4d)
else()
    set(SDK_LIB_DIR "<SDK>/lib")
    set(SDK_OPENCV_LIBS opencv_core4 opencv_imgproc4)
endif()

link_directories(${SDK_LIB_DIR})
target_link_libraries(my_plugin PRIVATE ${SDK_OPENCV_LIBS})
```

单配置生成器（Ninja）看 `CMAKE_BUILD_TYPE`；多配置生成器（如 Visual Studio）在
configure 时还定不下配置，请给 Debug / Release 各配一个构建目录，或改用
`$<CONFIG:Debug>` 生成表达式。**Debug 宿主的 exe 旁边是 `bin/debug` 那一套**
（`opencv_*4d.dll`、`libpng16d.dll`、`zd.dll` …），所以 Debug 插件必须链接
`opencv_*4d.lib`；拿 release 库编出来的 Debug 插件会在加载或第一次调用 OpenCV 时
因为找不到 `opencv_core4d.dll` 失败。详见下面「debug 变体（重要）」一节。

## 必须遵守的 ABI 约定

1. **运行库必须是 /MD（MultiThreadedDLL）**
   MSVC 下用 `set_property(TARGET my_plugin PROPERTY
   MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")`。
   宿主用 /MD 动态运行时，插件若用 /MT 静态运行时会得到两份 CRT，
   跨 DLL 传递 `std::string`/`cv::Mat` 会在释放时崩溃。

2. **Debug/Release 必须与宿主一致**
   Debug 构建的插件只能加载进 Debug 宿主，反之亦然。

3. **插件不得自带基础依赖**
   `opencv_*.dll` 等基础依赖由宿主构建期从 vcpkg 运行时目录布置到 `bin/`
   （`cmake/copy_runtime_deps.cmake`）；宿主本身不链接 OpenCV。
   插件目录里再放一份会造成「同一进程加载两个 OpenCV 实例」——
   类型信息、全局状态、内存分配器全部割裂，表现为莫名崩溃或
   `cv::Exception`。插件产物目录只应出现插件自己的 DLL。

4. **只导出 shot_module_entry，一个 DLL 可以装多个插件**
   插件包只导出 shot_module_entry() 一个符号，它返回自描述的 ShotModule 表
   （struct_size / abi_major / abi_minor / plugin_count / create / destroy /
   query_interface，以及可选的 init / shutdown / get_info）。
   索引与 metadata.json 的 plugins[] 顺序一一对应。

5. **只加字段，不加符号**
   加能力靠往结构体末尾追加字段；读任何字段前先检查 struct_size
   （契约头 `plugin_api/basics.h` 里的 SHOT_HAS_FIELD）。这样加东西永远不需要改版本号。

6. **能力用 interface id 查询**
   query_interface(instance, "shot.image_processor", 1) 返回函数表，
   没有的能力返回 NULL。已注册的插件能力：shot.image_processor、shot.configurable、
   shot.lib_provider、shot.ui_provider、shot.screen_capture、shot.image_saver。
   宿主服务也走同一个入口（`shot.host.*`）：`shot.host.config`、`shot.host.board_msg`、
   `shot.host.event_bus`、`shot.host.plugin_lists`、`shot.host.plugin_registry`。

7. **abi_major 必须与宿主一致**
   当前 `SHOT_ABI_MAJOR = 2` / `SHOT_ABI_MINOR = 0`；版本不匹配的插件会被宿主拒绝加载。
   2.0 删除了旧的配置目录服务，改为通用列表服务 `shot.host.plugin_lists`（见下一节）；
   删除已发布的服务属于破坏性变更，所以 major 涨到 2。
   升级 SDK 后请整体替换 include/，不要新旧混用。

8. **metadata.json 会被贴到 DLL 尾部**
   构建期由 `cmake/AppendPluginMeta.cmake` 追加纯文本标记（见 `plugin_api/plugin_info.h` 的
   `PLUGIN_META_START_MARKER` / `PLUGIN_META_END_MARKER`）。文件尾的额外数据不影响
   装载，元数据跟着插件文件走，不会与 DLL 分家。

## 纯订阅者 / 纯配置项插件（`PluginType::Generic`）

插件也可以**不实现任何宿主可调用的具名能力**，只做两件事：

- 声明 `shot.configurable`（让设置页出现自己的配置项）；
- 在 `create(index, host)` 里用事件总线订阅它关心的主题（如 `capture.done`），在
  `destroy` 里退订。

这类插件宿主会按 `PluginType::Generic` 加载：注册表照常列 LOADED、可启停、`config_list`
照常枚举它的配置项。**加载条件是 metadata 里声明过这个插件**；没有 metadata、又没有
任何具名能力的 DLL 会被销毁并跳过（"来源解释不清的实例不挂进宿主"）。

| 要点 | 规则 |
| --- | --- |
| 生命周期 | 没有 `init` / `deinit` / `post_to_plugin`；订阅在 `create(index, host)` 里建立（唯一拿到宿主服务表的时机），在 `destroy` 里退订 |
| 后台线程 | 需要线程的（如通知插件）自己在 `create` 起、在 `destroy` 里停线程并 `join`；宿主卸载时先 `destroy` 再 `shutdown`/放 DLL |
| 配置项 | 仍然实现 `shot.configurable`；`config_list` 与设置页不区分它是不是 Generic |
| 能力目录 | `interfaces` 只写真实实现的 id（如只有 `shot.configurable`）；**不要**为了拿 `host.shutdown` 就假装 `shot.ui_provider`——UI provider 的语义是"宿主可以给你投 `post_to_plugin`" |
| 事件回调 | 订阅回调仍然只在宿主 main 线程执行；回调里只做入队/唤醒，耗时工作转到自己的线程 |

仓库里的两个完整例子在宿主仓库：`plugins/clipboard`（订阅 `capture.done` 写剪贴板）
与 `plugins/notify`（订阅 `capture.done` / `capture.failed`，隐藏占位托盘图标 + 通知线程）。

## 给 UI 提供数据：用列表 + 事件，不加 ABI

插件想给宿主/设置页/托盘提供**一批数据**（菜单项、配置项、目标列表、热键动作……）时，
**不要**新增 ABI 结构体、也不要新增宿主服务。ABI 2.0 起统一走**通用列表**：

| 动作 | 做法 |
| --- | --- |
| 定义一种新列表 | 选一个 list id + 定义条目的 JSON schema，在宿主仓库的 `document/LISTS.md` §5 登记 |
| 运行期贡献条目 | `lists.add` 事件：`{"list":"...","owner":"<自己的插件 id>","items":[...]}`；`owner` 必须等于自己，插件卸载时宿主按它自动清理 |
| 撤回条目 | `lists.remove` 事件（`ids` 省略 = 撤回该 owner 全部） |
| 读列表 | `shot::PluginLists(host).json("<list id>")`：两段式只读 JSON 快照，任意线程可调 |
| 变化通知 | 宿主发布 `lists.changed`（负载只有 list id）；收到后重新读快照，**不要每帧轮询** |
| 写值 / 落盘 | 仅宿主生成的 `config_list`：`lists.update` 写值、`lists.commit` 落盘，结果走 `lists.updated` / `config.applied` |

```cpp
// 贡献一条托盘命令（插件侧，运行时发一次；完全不碰托盘代码）
shot::EventBus bus(host);
bus.publish("lists.add", R"json({
  "list": "tray_command_list",
  "owner": "acme.tools",
  "items": [{"id": "acme.capture", "label": "Acme 截屏", "event": "capture.request", "order": 20}]
})json");

// 读宿主聚合出来的快照
shot::PluginLists lists(host);
const std::string text = lists.json("tray_command_list");
```

**硬规则**：

- 新增数据形状 = 新 list id + JSON schema + `lists.*` 事件；**禁止**为它新增 ABI 结构体/服务。
- 运行期贡献**不写进 `metadata.json`**：metadata 只放身份、依赖与能力（语义见宿主仓库的
  `document/METADATA.md` §3，SDK 不附带该文档）。
- 消费方在 `lists.changed` / `settings.open` / `config.applied` 等事件之后刷新；
  完整协议（负载 schema、owner 校验、`order` / `separator` 规则、线程与信任模型）见
  宿主仓库的 `document/LISTS.md`。

## 采集与保存能力

SDK 目录结构不变：采集/保存插件仍是普通插件（`shot_add_plugin()` + `metadata.json`），
通过能力函数表与宿主交互。完整数据流见宿主仓库的 `document/CAPTURE.md`，宿主仓库的
`plugins/screen_capture/` 与 `plugins/image_saver/` 是可直接编译的样板。

| 能力 | 结构体 | 实现要点 |
| --- | --- | --- |
| `shot.screen_capture`（屏幕采集） | `ShotScreenCaptureV1` | `describe` 填虚拟桌面的 `width` / `height` / `origin_x` / `origin_y` / `pixel_format`，**几何与像素都是物理像素**（多显示器合成后的虚拟桌面；origin 可能为负）；`capture` 只往宿主分配的 BGRA8 缓冲里写像素，**绝不自己分配图像内存**，必须按 `stride` 逐行寻址；返回码 `0` / `1` / `2` / `3`。区域截图由宿主先整屏抓取再按 `region` 裁剪，采集插件不感知取景范围。平台相关实现（如 Windows GDI）在 CMake 里按平台跳过，不写未验证的伪实现。 |
| `shot.image_saver`（图像保存） | `ShotImageSaverV1` | `save` 把 `ShotImage` 落盘并把最终绝对路径写回 `path`（缓冲不足返回 `2`）；`directory` / `basename` 为 `NULL` 时用插件自己的配置项；格式/质量/命名策略全在插件侧，宿主不硬编码。 |

**能力回调的线程**：`screen_capture` / `image_processor` / `image_saver` 的虚函数在
采集流水线的**工作线程**上被调用（一次一张、串行执行），而 `shot.configurable` 的
`set_config` 在宿主 main 线程（启动回灌 / 设置页应用）。跨线程共享状态必须自己同步；
不要在能力回调里直接操作 UI 或假设"永远在 main 线程"（流水线线程模型见宿主仓库的
`document/CAPTURE.md` §7）。

## 写一个 lib 插件（`shot.lib_provider`）

有些包不提供宿主能调用的算法，只负责把某个运行库带进进程（典型：OpenCV）。这类插件实现
`shot.lib_provider`，宿主据此把它登记成 `PluginType::Lib`（运行库提供者），而不是一个
"没有能力的坏图像处理器"：它报告的库名与版本会被宿主记录，版本还是依赖校验的依据；
别的插件通过 `provides` / `requires` 与它建立加载顺序。

### 接口

`include/plugin_api/lib_provider.h` 的 `ShotLibProviderV1` 只有两个 getter（外加
`struct_size` / `version` 两个头字段）：

| 字段 | 语义 |
| --- | --- |
| `get_library_name(void* self)` | 库名（标识提供者身份；metadata 的 `provides` 约定写同一个名字）；**不能返回 `NULL`** |
| `get_library_version(void* self)` | 运行库版本（如 `"4.12.0"`），依赖校验按它比较；须为可解析数字版本（缺段按 0） |

两个 getter 返回的都是插件内的静态字符串，有效期到 DLL 卸载，宿主只读、不释放。
版本按数字段比较，尾部 `-dev` 之类的附加信息会被忽略。函数表用
`SHOT_OPS(ShotLibProviderV1, .get_library_name = ..., .get_library_version = ...)` 构造
（`struct_size` / `version` 自动填好），再把 `{SHOT_IFACE_LIB_PROVIDER, 1, &kLibOps}` 登记进
能力目录；其它能力 id 一律返回 `NULL`。宿主仓库的 `plugins/opencv_provider` 是完整样板。

`get_library_name` 返回 `NULL` 等于没提供这个能力：宿主会继续按其它能力识别，最终可能只把
它当 Generic 插件。`get_library_version` 返回 `NULL` 不会阻止加载，但依赖方写
`opencv>=4.12` 时宿主只能退用**插件自身版本**来比较，通常会因版本不匹配跳过依赖方；
只有名字的 `opencv` 依赖不受影响。

### 元数据

```json
{
  "schema": 1,
  "abi": { "major": 2, "minor": 0 },
  "plugins": [
    {
      "id": "acme.opencv",
      "name": "ACME OpenCV Provider",
      "version": "1.0.0",
      "interfaces": ["shot.lib_provider"],
      "requires": ["core>=0.1.0"],
      "provides": ["opencv"]
    }
  ]
}
```

- `interfaces`：写 `"shot.lib_provider"`，宿主加载前用它预筛；与 `query_interface()` 的
  实际回答不一致时只警告、不拒绝加载，但排查会很困难。
- `provides`：**别的插件依赖的名字**，约定与 `get_library_name()` 的返回串一致 ——
  写 `"opencv"`，消费者就写 `"requires": ["opencv"]` 或 `"requires": ["opencv>=4.12"]`。
  宿主做依赖匹配时只看 `provides`（插件 id 和 `provides` 里的名字都算提供者），
  `get_library_name()` 只用于记录运行库身份；两者不一致不会报错，但依赖声明会难以对照。
  `provides` 只放名字、不放版本：库版本以运行期 `get_library_version()` 为准，避免 vcpkg
  升级后 metadata 与真实库版本各说一套。
- `requires`：至少写 `"core>=0.1.0"`（宿主自身版本）；同包内其它插件也可以按 id 依赖它。
- metadata 只是声明，不是权威：加载后宿主以 `get_info()` / `query_interface()` 的真实回答
  为准。字段与校验规则的完整语义见宿主仓库的 `document/METADATA.md`（SDK 不附带该文档）。

### 加载顺序

宿主扫描 `plugins/` 时**先读每个 DLL 尾部的 metadata（不加载）**，建"提供者名字 → 插件"
索引，再按 `requires` 做拓扑排序，所以 **provider 先于消费者加载**：消费者在
`create` / `init` 里需要 OpenCV 时，provider 已经在进程里。运行期重新启用消费者时，
宿主也会先启用它的 provider（深度优先，跳过 `core`）。

声明只是声明：provider 没装、被禁用或加载失败时，宿主只跳过依赖它的插件并在日志写明
原因，不会让整个宿主失败；依赖成环时打印警告后按发现顺序继续；没有 metadata 的包按
"无依赖"最后加载。消费者不要自己 `LoadLibrary` 基础库，也不要假设"provider 一定在" ——
依赖不满足时宿主根本不会创建消费者。

### 版本协商

宿主用 `query_interface(instance, "shot.lib_provider", 1)` 查能力表。SDK 的
`shot_query_interface()` 语义是"只回答不高于请求版本的能力"：插件若只提供更高版本的
函数表，请求 1 会拿到 `NULL`，宿主按"没有该能力"继续处理（不会猜测结构布局，也不会
降级调用）。所以第三方 lib 插件**当前只应实现版本 1**；将来按契约规则往结构体末尾
追加字段、递增版本号，老宿主仍按版本 1 请求、拿到兼容的那张表。

### 分发

`opencv_provider.dll` 随**宿主**的 `plugins/` 目录分发，不在本 SDK 的 `bin/` 里：SDK 的
`bin/` 只放 OpenCV 等基础运行库（宿主构建期从 vcpkg 布置到宿主 exe 旁边），`plugins/`
只放插件自己的 DLL。第三方 lib 插件编好后同样把 DLL 放进宿主 `plugins/`，
**不要**拷进 `bin/` —— 那是运行时目录，混入插件会污染宿主安装，也会破坏"插件目录里
只应出现插件自己的 DLL"这条约束（同一进程出现两份库会导致类型信息、全局状态与内存
分配器割裂）。

## debug 变体（重要）

`lib/debug/` 与 `bin/debug/` 是 Debug 构建专用的一套变体。
**绝对不要把两个变体的 DLL 放进同一个目录**：`jpeg62.dll`、`turbojpeg.dll`、
`liblzma.dll`、`libwebp*.dll` 这些文件名在 debug 与 release 下【完全相同】，
混放会互相覆盖且不会有任何报错。运行时的规则是：宿主 exe 所在目录里的那一套
必须与你编译插件时用的那一套一致（Debug 宿主缺 `opencv_*4d.dll` 会启动失败）。

## 注意

本 SDK **不包含** `*.pdb`（调试符号）与 `pkgconfig` 元数据。
需要调试符号请直接使用构建机上的 `vcpkg_installed` 目录。
