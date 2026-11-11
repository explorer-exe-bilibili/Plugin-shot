# ABI 政策与登记表

这份文档是契约头（`include/plugin_api.h` 汇总头 + `include/plugin_api/*.h` 子头）的
人读部分：契约为什么这么写、什么能改、什么绝对不能改，以及所有跨边界标识符的登记表。
**改契约之前先读这里。**

## 0. 契约头的结构

`include/plugin_api.h` 现在只是**汇总头，只做 include**；正文按主题拆在同目录的
`plugin_api/` 下（与 `include/shot_plugin_helpers.h` + `shot_plugin_helpers/` 同一套组织方式）。
子头之间用 `#include "plugin_api/xxx.h"` 显式声明依赖，每个子头各自 `#pragma once`（不用
include guard）；插件源码照旧只写 `#include "plugin_api.h"`，子头位置对插件透明。

| 文件 | 负责什么 |
| --- | --- |
| `plugin_api/basics.h` | 契约 banner 与三条纪律、`SHOT_ABI_MAJOR` / `SHOT_ABI_MINOR`、前向 typedef、`struct_size` 读取规则（`SHOT_REQUIRES` / `SHOT_HAS_FIELD` / `SHOT_STATIC_ASSERT`）、`SHOT_LOG_*` |
| `plugin_api/interfaces.h` | 全部 interface id 宏（`SHOT_IFACE_*` 与 `SHOT_HOST_IFACE_*`），本文档 §4 登记表在代码里的副本 |
| `plugin_api/image.h` | `ShotImage` 与 `SHOT_PIXEL_FORMAT_*`（图像数据契约） |
| `plugin_api/plugin_info.h` | `ShotPluginInfo` 与 `PLUGIN_META_START_MARKER` / `PLUGIN_META_END_MARKER` |
| `plugin_api/image_processor.h` | `shot.image_processor` 函数表 |
| `plugin_api/lib_provider.h` | `shot.lib_provider` 函数表 |
| `plugin_api/configurable.h` | `SHOT_CONFIG_KIND_*`、`ShotConfigDesc`、`shot.configurable` 函数表 |
| `plugin_api/ui_provider.h` | `ShotEvent` 与 `shot.ui_provider` 函数表 |
| `plugin_api/screen_capture.h` | `ShotCaptureInfo` 与 `shot.screen_capture` 函数表 |
| `plugin_api/image_saver.h` | `shot.image_saver` 函数表 |
| `plugin_api/host_services.h` | `ShotHostServices` 与五个宿主服务结构体（config / board_msg / event_bus / plugin_lists / plugin_registry，含 `ShotPluginStatus` 与 `SHOT_PLUGIN_STATE_*`） |
| `plugin_api/module.h` | `ShotModule` 与唯一导出 `shot_module_entry()` |
| `plugin_api/abi_layout.h` | 全部 `static_assert` 布局锚点（§6 的机器可读副本），汇总头里**最后** include |

构建期按文本读契约的脚本（`cmake/ValidatePluginMeta.cmake`、`cmake/AppendPluginMeta.cmake`）
经 `cmake/ReadContractHeader.cmake` 把汇总头与全部子头拼成整份文本再匹配，
所以别把宏或结构体放回汇总头，也别在 `plugin_api/` 下再嵌套子目录。

## 1. 版本规则

| 版本 | 含义 | 什么时候递增 |
| --- | --- | --- |
| `SHOT_ABI_MAJOR` | 布局或语义不兼容 | 改了已有字段的顺序 / 类型 / 含义，或改了导出符号 |
| `SHOT_ABI_MINOR` | 只追加了能力 | 新增能力结构体、新增可选字段、新增 interface id |

宿主的判定：`abi_major` 必须相等；`abi_minor` 大于宿主时照常加载，但只会请求
宿主自己认识的接口版本（插件必须能在"较老的宿主"上降级运行）。

当前版本 `SHOT_ABI_MAJOR = 2` / `SHOT_ABI_MINOR = 0`：2.0 删除了 `shot.host.plugin_config`
（配置目录），改为通用列表服务 `shot.host.plugin_lists`：列表数据是 JSON 快照，注册、写值、
提交全部走 `lists.*` 事件（见 `document/LISTS.md`）。删除已发布的服务属于破坏性变更，所以
major 涨到 2。**从 2.0 起，新增一种 UI/宿主数据不再改 ABI**：新 list id + JSON schema 即可。

## 2. 只追加规则（违反任意一条都是破坏 ABI）

1. 结构体只能**在末尾追加**字段。永不重排、永不改类型、永不删除。
2. 作废字段改名成 `retired_xxx`，用等宽类型占位，**永远不再使用**。
3. 每个跨边界结构体第一个字段必须是 `uint32_t struct_size`，由构造方填自己的 `sizeof`。
4. 读取方读任何字段前都要过 `SHOT_HAS_FIELD(ptr, type, field)`；字段的函数指针还可能为
   `NULL`（= 该实现不提供这个能力），调用前另行判空。
5. 跨边界只用定宽类型：`uint32_t` / `int32_t` / `int64_t` / `double` / 指针。
   禁止 `int`、`long`、`size_t`、`bool`、位域、C 枚举、`std::string`、STL 容器、带虚函数的类。
6. 异常绝不跨越 DLL 边界：每个 `extern "C"` 函数内部兜底，错误用返回值表达。
7. 谁分配谁释放：插件分配的对象只能由插件的 `destroy` 释放，宿主绝不 `delete`。
8. 通过追加加入的字段**必须允许"永远不被调用"**：老宿主不会调它。需要"必须执行"的语义
   只能靠已有的 `destroy` 或版本化的新结构体表达。

## 3. 加东西的正确姿势

| 想加什么 | 做法 | 需要动 `abi_major` 吗 |
| --- | --- | --- |
| 一类新插件（同一种能力的新实现） | 新写一个插件工程，复用已有能力结构体 | 不需要 |
| 纯订阅者 / 纯配置项插件（没有具名能力） | 有 metadata 声明即可按 `PluginType::Generic` 加载；订阅在 `create`、退订在 `destroy`（见 `document/SETTINGS.md` §3.3） | 不需要 |
| 一种新能力 | 新 interface id + 新结构体 + 在 `query_interface` 里回答它 | 不需要 |
| 给 UI/宿主提供一种新数据（菜单项、配置项、目标列表…） | 新 list id + JSON schema + `lists.add` 事件（见 `document/LISTS.md`），**不新增 ABI 结构体** | 不需要 |
| 给已有能力加一个可选操作 | 该结构体**末尾**追加一个函数指针字段 | 不需要 |
| 改已有字段的类型 / 含义 | 出 `...V2` 结构体，旧结构体永久冻结 | 不需要（但 minor 要涨） |
| 改导出符号本身 | —— | **必须涨** |

## 4. interface id 登记表

新加 id 必须在这里登记一行，否则半年后没人知道哪个字符串才是对的。

| id | 结构体 | 版本 | 说明 |
| --- | --- | --- | --- |
| `shot.image_processor` | `ShotImageProcessorV1` | 1 | 处理一张 `ShotImage`，返回 0 = 成功 |
| `shot.configurable` | `ShotConfigurableV1` | 1 | 声明并读写配置项（值一律字符串） |
| `shot.lib_provider` | `ShotLibProviderV1` | 1 | 声明「我是运行库提供者」，报告库名与版本；宿主按 `PluginType::Lib` 登记 |
| `shot.ui_provider` | `ShotUIProviderV1` | 1 | 窗口/托盘由插件自己创建和管理，宿主只调 `init` / `deinit` / `post_to_plugin` |
| `shot.screen_capture` | `ShotScreenCaptureV1` | 1 | 屏幕采集：`describe` 报虚拟桌面几何，`capture` 把像素写进宿主分配的 `ShotImage`（BGRA8），见 `document/CAPTURE.md` |
| `shot.image_saver` | `ShotImageSaverV1` | 1 | 图像保存：把 `ShotImage` 落盘并写回最终路径；格式/质量/目录来自插件配置项，见 `document/CAPTURE.md` |
| `shot.host.config` | `ShotHostConfigV1` | 1 | 宿主提供的配置读写接口 |
| `shot.host.board_msg` | `ShotHostBoardEventV1` | 1 | 宿主提供的广播事件接口 |
| `shot.host.event_bus` | `ShotHostEventBusV1` | 1 | 宿主提供的进程内主题发布/订阅（通配、深拷贝、main 线程分发），见 `document/EVENT_BUS.md` |
| `shot.host.plugin_lists` | `ShotHostPluginListsV1` | 1 | 宿主提供的通用列表服务：按 list id 返回 JSON 快照（`config_list` / `tray_command_list` …）；注册/写值/提交走 `lists.*` 事件，见 `document/LISTS.md` |
| `shot.host.plugin_registry` | `ShotHostPluginRegistryV1` | 1 | 宿主提供的插件注册表：已安装插件状态（已加载/禁用/失败）+ 异步启停（热卸载），见 `document/SETTINGS.md` |

命名约定：全小写，`shot.` 前缀，多词用下划线分隔。第三方扩展用自己的前缀（例如 `acme.sharpen`）。

## 5. 配置 key 约定

- 每个插件内的 `key` 唯一，全小写 + 下划线（`keep_alpha`、`strength`）。
- `key` 一旦发布不要改名（它是存盘的标识）；要换语义就换个新 key。
- 插件遇到不认识的 key 必须返回负值并保持可用状态：宿主升级后可能多传未知项。
- **持久化键是 `<owner>/<id>`**（如 `example.invert/keep_alpha`、
  `core/logging.level`）：插件只负责 get/set，落盘与启动回灌由宿主做
  （`config_list` 的条目 schema 与写值/提交协议见 `document/LISTS.md` §5.1、
  `document/SETTINGS.md`）。`core` 是宿主自己的配置提供者 id，插件不要占用。

## 6. 布局锚点（改这里 = 改 ABI）

`plugin_api/abi_layout.h` 里的 `static_assert` 是 ABI 的机器可读副本（汇总头最后 include 它）。当前值：

| 结构体 | `sizeof` | 关键偏移 |
| --- | --- | --- |
| `ShotImage` | 32 | `data`=8, `channels`=28 |
| `ShotEvent` | 32 | `name`=8, `data`=16, `data_len`=24 |
| `ShotPluginInfo` | 32 | `id`=8 |
| `ShotImageProcessorV1` | 16 | `process`=8 |
| `ShotLibProviderV1` | 24 | `get_library_version`=16 |
| `ShotConfigDesc` | 72 | `min`=40 |
| `ShotConfigurableV1` | 40 | `set_config`=32 |
| `ShotUIProviderV1` | 32 | `init`=8, `deinit`=16, `post_to_plugin`=24 |
| `ShotCaptureInfo` | 24 | `width`=4, `origin_x`=12, `pixel_format`=20 |
| `ShotScreenCaptureV1` | 24 | `describe`=8, `capture`=16 |
| `ShotImageSaverV1` | 16 | `save`=8 |
| `ShotHostServices` | 48 | `log`=24, `query_interface`=32, `post_to_host`=40 |
| `ShotHostEventBusV1` | 32 | `publish`=8, `subscribe`=16, `unsubscribe`=24 |
| `ShotPluginStatus` | 56 | `id`=8, `state`=40, `reason`=48 |
| `ShotHostPluginRegistryV1` | 40 | `installed_count`=8, `set_enabled`=32 |
| `ShotHostPluginListsV1` | 32 | `list_count`=8, `list_id`=16, `list_json`=24 |
| `ShotModule` | 64 | `destroy`=48, `query_interface`=56 |

追加字段时你会被迫更新这张表与那几行 `static_assert` —— 这正是它们存在的意义：
让"改布局"成为一个必须被看见的动作。

## 7. 改契约前的检查清单

- [ ] 我只在结构体**末尾**加了字段？（没有重排、没有改类型）
- [ ] 我改的是 `plugin_api/*.h` 里的子头，没有把内容塞回只做 include 的汇总头？
- [ ] 新字段在"老宿主不调用它"的情况下语义仍然正确？
- [ ] 新能力用的是新的 interface id，而不是往老 id 里塞不相关的东西？
- [ ] `struct_size` 由构造方填好了（插件用 `SHOT_OPS` 宏就不会漏）？
- [ ] `plugin_api/abi_layout.h` 的 `static_assert` 与上面的布局表同步更新了？
- [ ] `abi_minor`（或 major）按规则递增了？
- [ ] 新增的 id / key 登记进本文档了？
- [ ] `document/README_SDK.md` 里的约定与实现一致？
