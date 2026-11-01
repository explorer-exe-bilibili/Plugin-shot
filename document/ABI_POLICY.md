# ABI 政策与登记表

这份文档是 `include/plugin_api.h` 的人读部分：契约为什么这么写、什么能改、
什么绝对不能改，以及所有跨边界标识符的登记表。**改契约之前先读这里。**

## 1. 版本规则

| 版本 | 含义 | 什么时候递增 |
| --- | --- | --- |
| `SHOT_ABI_MAJOR` | 布局或语义不兼容 | 改了已有字段的顺序 / 类型 / 含义，或改了导出符号 |
| `SHOT_ABI_MINOR` | 只追加了能力 | 新增能力结构体、新增可选字段、新增 interface id |

宿主的判定：`abi_major` 必须相等；`abi_minor` 大于宿主时照常加载，但只会请求
宿主自己认识的接口版本（插件必须能在"较老的宿主"上降级运行）。

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
| 一种新能力 | 新 interface id + 新结构体 + 在 `query_interface` 里回答它 | 不需要 |
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

命名约定：全小写，`shot.` 前缀，多词用下划线分隔。第三方扩展用自己的前缀（例如 `acme.sharpen`）。

## 5. 配置 key 约定

- 每个插件内的 `key` 唯一，全小写 + 下划线（`keep_alpha`、`strength`）。
- `key` 一旦发布不要改名（它是存盘的标识）；要换语义就换个新 key。
- 插件遇到不认识的 key 必须返回负值并保持可用状态：宿主升级后可能多传未知项。

## 6. 布局锚点（改这里 = 改 ABI）

`plugin_api.h` 末尾的 `static_assert` 是 ABI 的机器可读副本。当前值：

| 结构体 | `sizeof` | 关键偏移 |
| --- | --- | --- |
| `ShotImage` | 32 | `data`=8, `channels`=28 |
| `ShotPluginInfo` | 32 | `id`=8 |
| `ShotImageProcessorV1` | 16 | `process`=8 |
| `ShotLibProviderV1` | 24 | `get_library_version`=16 |
| `ShotConfigDesc` | 72 | `min`=40 |
| `ShotConfigurableV1` | 40 | `set_config`=32 |
| `ShotHostServices` | 40 | `log`=24 |
| `ShotModule` | 64 | `destroy`=48, `query_interface`=56 |

追加字段时你会被迫更新这张表与那几行 `static_assert` —— 这正是它们存在的意义：
让"改布局"成为一个必须被看见的动作。

## 7. 改契约前的检查清单

- [ ] 我只在结构体**末尾**加了字段？（没有重排、没有改类型）
- [ ] 新字段在"老宿主不调用它"的情况下语义仍然正确？
- [ ] 新能力用的是新的 interface id，而不是往老 id 里塞不相关的东西？
- [ ] `struct_size` 由构造方填好了（插件用 `SHOT_OPS` 宏就不会漏）？
- [ ] `static_assert` 与上面的布局表同步更新了？
- [ ] `abi_minor`（或 major）按规则递增了？
- [ ] 新增的 id / key 登记进本文档了？
- [ ] `document/README_SDK.md` 里的约定与实现一致？
