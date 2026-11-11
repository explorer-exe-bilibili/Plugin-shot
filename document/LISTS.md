# 通用列表与事件驱动（`shot.host.plugin_lists`）

这份文档定义插件与宿主之间"一批数据"的唯一通用通道：宿主把 core 与各插件的贡献按
**list id** 聚成 JSON 快照，插件通过 `shot.host.plugin_lists` 只读枚举，通过 `lists.*` 事件
在运行期注册、撤回、写值与提交。**新增 list id、改 `lists.*` 负载、写列表消费方之前先读这里**；
主题与线程模型的总则见 `document/EVENT_BUS.md`，配置/设置页那条链路见 `document/SETTINGS.md`。

## 1. 理念：数据形状不进 ABI

ABI 1.x 的配置目录是"一种数据配一个宿主服务"的写法。插件生态长大以后，托盘菜单、热键、
目标列表……每一种新数据都想要一个 ABI 结构体和一个新服务，契约就再也冻结不了。ABI 2.0
起换成**一个通用容器**：

- **数据结构 = 某个 list id 下的一组 JSON 条目**。条目的 schema 由 list id 决定，不跨边界
  传结构体，所以"新增一种列表"只改 JSON，不改 ABI。
- **读** = `shot.host.plugin_lists` 的 `list_count` / `list_id` / `list_json`：两段式只读快照。
- **写** = `lists.add` / `lists.remove` / `lists.update` / `lists.commit` 事件。事件只在宿主
  main 线程分发处理，所以写路径天然单线程，不需要跨线程写接口。
- **变化通知** = `lists.changed`（宿主 → 所有，负载只有 list id）。同一轮里的多次变化合并成
  一条，由 `HostRuntime::tick` 统一 flush。i18n 也只复用这条通知，不新增事件主题
  （`document/I18N.md`）。

由此得到三条**长期约定**：

1. **禁止为新的数据形状新增 ABI 结构体或宿主服务。** 新数据 = 新 list id + JSON schema +
   `lists.*` 事件；ABI 只描述**能力与行为**（谁能处理图像、谁保存、谁提供 UI），不描述数据
   长什么样。
2. **metadata 保持最小**：只放身份、依赖与能力声明。运行期贡献（菜单项、配置值、目标列表
   条目……）由插件在运行时用 `lists.add` 声明，**不进 metadata**（见 `document/METADATA.md` §3）。
3. **消费方只在事件之后刷新**，不每帧轮询：宿主内部保存解析后的结构、按列表缓存序列化
   结果（见 §8）。

| 想加什么 | 做法 | 要动 ABI 吗 |
| --- | --- | --- |
| 给 UI / 宿主提供一批新数据（菜单项、配置项、目标列表、热键动作……） | 新 list id + JSON schema + `lists.*` 事件（本文件） | **不需要** |
| 一种新的可调用能力（算法、采集、保存、UI……） | 新 capability id + 函数表（`document/ABI_POLICY.md` §3） | 不需要（minor 内） |
| 给已有能力加一个可选操作 | 该结构体**末尾**追加函数指针字段 | 不需要 |
| 改已有字段的类型 / 语义 | 出 `...V2` 结构体，旧结构体永久冻结 | 不需要（minor 要涨） |
| 改导出符号 / 重排已有结构 | 破坏性变更 | **必须涨 major** |

## 2. 读：`shot.host.plugin_lists`

接口 id `shot.host.plugin_lists`，版本 1，结构体 `ShotHostPluginListsV1`
（`include/plugin_api.h`）；插件侧 C++ 封装是 `shot::PluginLists`
（`include/shot_plugin_helpers/shot_plugin_lists.h`）。

| 字段 | 签名 | 语义 |
| --- | --- | --- |
| `list_count` | `uint32_t (*)(void* ctx)` | 当前已注册的列表个数；列表 id 枚举顺序是宿主内部实现，**不要依赖** |
| `list_id` | `int32_t (*)(void* ctx, uint32_t index, char* buf, uint32_t buf_size)` | 取第 index 个 list id；`buf == NULL` 时只查长度 |
| `list_json` | `int32_t (*)(void* ctx, const char* list_id, char* buf, uint32_t buf_size)` | 取某个列表的 JSON 快照（UTF-8 文本）；`buf == NULL` 时只查长度 |

**两段式读取（三个函数同一套规则）**：

1. 第一段：`buf = NULL`、`buf_size = 0`，返回所需**字节数（不含结尾 NUL）**；
2. 分配 `长度 + 1` 字节，再调一次，`buf_size` **必须大于**长度（给 NUL 留位）；
3. 返回负数 = list id 不认识 / 缓冲不足 / 参数非法。**未知 list id 一律返回负数**，插件按
   "没有这个列表"安静降级，不报错、不崩。

JSON 形状由 list id 决定，但所有列表都必须是**条目对象组成的 JSON 数组**（空列表是 `[]`）。
条目的字段由 schema 定义（见 §5），宿主对插件贡献的条目**只解释 `id` 与 `order`**，其余字段
原样保存、原样返回——所以给条目加字段不需要宿主升级，需要的是贡献方与消费方按 schema 对齐。

```cpp
// 读一次 config_list（两段式已经封在 shot::PluginLists 里）。
shot::PluginLists lists(host);
const std::string text = lists.json("config_list");
nlohmann::json items = nlohmann::json::parse(text, nullptr, false);
```

线程模型与缓存：

- `list_json` **任意插件线程可调**：宿主内部用互斥保护数据，返回的是**调用方缓冲里的副本**，
  与列表后续变化无关。
- 序列化结果**按列表缓存**：只有列表内容变了才标脏，下一次读取时重建一次缓存，之后直接返回
  缓存字符串。消费方仍然要按事件刷新，不要每帧轮询（§8）。

## 3. 写：`lists.*` 事件

写路径没有同步函数，全部是事件。插件可以在任意线程 `publish`，但事件只在**宿主 main 线程**
分发处理；宿主对"非 main 线程的列表写入口"有运行期检查（直接拒绝并警告），这把"约定"变成了
可观测的行为。

| 主题 | 方向 | 负载 | 语义 |
| --- | --- | --- | --- |
| `lists.add` | 插件 → 宿主 | `{"list","owner","items":[...]}` | 贡献 / 覆盖条目；任意 list id 首次出现时自动注册 |
| `lists.remove` | 插件 → 宿主 | `{"list","owner","ids"?}` | 撤回条目；`ids` 省略 = 撤回该 owner 全部 |
| `lists.update` | 消费方（设置页）→ 宿主 | `{"list","tag"?,"changes":[...]}` | **仅 `config_list`**：写值 |
| `lists.commit` | 消费方（设置页）→ 宿主 | `{"list","tag"?}` | **仅 `config_list`**：落盘 |
| `lists.updated` | 宿主 → 所有 | `{"list","tag"?,"results":[...],"error"?}` | 写值 / 落盘的结果（成功、逐项失败、整体失败都回） |
| `lists.changed` | 宿主 → 所有 | `{"list"}` | 列表内容变化的**唯一通知**；同轮合并，每 list 一条 |

所有负载都是 UTF-8 JSON 文本（可以没有结尾 NUL）。宿主用**不抛异常**的解析器：非法负载只
记为 WARN 并丢弃，绝不让一条坏事件把宿主带崩。单条事件负载上限、队列上限见
`document/EVENT_BUS.md` §5（**列表条目里不要塞像素/文件内容等大对象**）。

### 3.1 `lists.add`（插件 → 宿主）

```json
{
  "list": "tray_command_list",
  "owner": "example.invert",
  "items": [
    {"id": "example.capture", "label": "示例：截屏（示例插件）",
     "event": "capture.request", "data": "", "order": 20, "separator": false}
  ]
}
```

规则（逐条都由宿主实现校验）：

- `list` / `owner` 必须是非空字符串，否则整条事件丢弃；
- `owner` **必须等于发布插件自己的 id**（约定，见 §7）；宿主只校验"这个 id 当前有已加载实例"，
  不是已加载插件就丢弃。`owner` 保留给宿主自己（`"core"`），插件不得使用；
- 宿主生成的列表（`config_list` / `i18n_list`）**不能 add/remove**（`config_list`
  写值走 `lists.update`）；宿主对任意 `generated` 列表都拒绝写入口，另有一道双保险；
- `items` 必须是数组；数组里每个元素必须是 JSON 对象且带**非空字符串 `id`**，否则跳过该条，
  其余条目照常接受；
- 同 `(list, owner, id)` 再 add = **upsert**：只更新内容与 `order`，不新增条目；
- `order` 可选，数字（整数/浮点都接受，越界按 int32 截断），缺省 `1000`；条目按 `order`
  **升序稳定排序**，同 order 保持插入顺序（消费方刷新时不会因为一次无关 add 而互相换位）；
- 任意 list id **第一次 add 时自动注册**（不需要宿主预先声明这个列表）；
- **列表没有所有者**：任何插件都可以向任意 list id 贡献条目，宿主按 `owner` 各记各的账；
  来自不同 owner 的条目一起按 `order` 排序，消费方通常不关心条目是谁贡献的（除非 schema 要求）；
- 宿主只为**被接受的条目**（`accepted > 0`）标记 `lists.changed`。

宿主不解释条目里的业务字段：`label` / `event` / `data` / `separator` / `default` 等由对应
list id 的 schema 与消费方解释（见 §5）。因此给条目加字段只改 schema 与消费方，不改宿主。
**唯一例外**：宿主在保存贡献条目时会注入 `owner`（贡献者 id）——消费方要用它按
`display(owner, label)` 解析"文本 key"（`document/I18N.md` §2.3）。条目自带 `owner` 时
以列表级 owner 为准，插件不能借别人的 owner 解析文案。

### 3.2 `lists.remove`（插件 → 宿主）

```json
{"list": "tray_command_list", "owner": "example.invert", "ids": ["example.capture"]}
```

- `ids` 省略、`null` 或空数组 = 移除该 owner 在这个列表里的**全部**条目；非空数组 = 只移除
  列出的 id（非字符串项被忽略）；
- 列表本来就不存在 = **幂等空操作**，不报错；
- 宿主生成的列表（`config_list` / `i18n_list`）不能 remove；`owner = "core"` 不能 remove
  （宿主预置项插件删不掉）；
- `owner` 不是已加载插件就丢弃；
- 只有真的删掉了条目（`removed > 0`）才标记 `lists.changed`。

### 3.3 `lists.update`（消费方 → 宿主，仅 `config_list`）

```json
{
  "list": "config_list",
  "tag": "7",
  "changes": [
    {"owner": "image_saver.main", "id": "format", "value": "jpeg"}
  ]
}
```

- 只支持 `config_list`；`list` 不是它、`changes` 不是数组、负载不是 JSON 对象 → 回
  `lists.updated`（`results` 空、带 `error`），**不静默丢弃**（消费方可能正等回执）；
- 每个 change 的 `owner` / `id` / `value` 都必须是字符串，否则逐项 `ok=false`；
- 宿主在生命周期锁内按 `owner` 找到提供者（`core` 或当前已加载、实现了 `shot.configurable`
  的实例），转调其 `set_config`：
  - 成功：值已经推给插件，同时写入宿主内存配置（键 `owner/id`，**落盘由 commit 负责**），
    `ok=true`、`reason=""`；
  - 失败：`ok=false`、`reason` 为"提供者未加载或不支持 `shot.configurable`"或
    "插件拒绝（返回码 N）"；
- 逐项结果**全部**收集后发布 `lists.updated`（`tag` 原样回显）；
- 处理完立刻重建 `config_list`，所以消费方收到 `lists.updated` 再读快照时看到的就是新值，
  不会出现"响应说成功、列表还是旧值"的窗口；
- **失败项不回滚**：已经成功的项保持生效，消费方按逐项结果决定重试/提示。

### 3.4 `lists.commit`（消费方 → 宿主，仅 `config_list`）

```json
{"list": "config_list", "tag": "7"}
```

- 只支持 `config_list`；其它 list 回 `lists.updated`（`results` 空 + `error`）；
- 宿主在 main 线程**同步**调用 `core::Config::saveConfigFile()` 把当前内存配置写进
  `config.json`，然后重建 `config_list`；
- **成功**：发布既有主题 `config.applied`（空负载，**不回显 tag**）。这是"这批值已经落盘"
  的通知，设置页以收到它作为本次提交成功；
- **失败**：发布 `lists.updated`（`results` 空、`error = "保存失败"`）；
- `lists.commit` 只负责**落盘**，不负责写值——值必须先用 `lists.update` 写入内存。它不是
  事务：没有 begin/rollback，失败时以内存里的最新值为准（下次 tick 还会尝试落盘）。

### 3.5 `lists.updated`（宿主 → 所有）

```json
{
  "list": "config_list",
  "tag": "7",
  "results": [
    {"owner": "image_saver.main", "id": "format", "ok": true,  "reason": ""},
    {"owner": "tray.main",          "id": "notify", "ok": false, "reason": "插件拒绝（返回码 1）"}
  ]
}
```

- `tag` 原样回显请求里的值（推荐用字符串，方便消费方自增匹配）；
- `results` 与请求里 `changes` **同序**、逐项给出 `ok` / `reason`；
- 整体失败（负载非法 / 不支持的 list / `changes` 不是数组）时 `results = []`、带 `error`；
- **失败也一定回一条**：否则消费方会一直等到超时；
- 这是广播主题，同一个进程里的其它消费者也可能收到别人的结果——消费方按自己的 `tag` 过滤。

### 3.6 `lists.changed`（宿主 → 所有）

```json
{"list": "config_list"}
```

- 列表内容变化的**唯一**通知；负载只有 list id（消费方必须自己回读快照拿新内容）；
- 宿主把同一轮里的多次变化合并成一个集合，由 `HostRuntime::tick` 统一 flush，**每 list 一条**；
- 触发来源：`lists.add` / `lists.remove` 真的改了内容；`config_list` 重建后内容真的变了；
  插件被禁用/卸载时清掉它的贡献（§6）；
- 消费方按 `list` 过滤（托盘对空负载按"可能是任意列表"保守处理，见插件 README）；
- 消费方刷新时机见 §4。

## 4. 变化通知与刷新时机（消费方）

列表是**快照**，不是增量流。消费方的正确姿势是"本地缓存模型 + 事件触发重读"：

| 时机 | 消费方应做什么 |
| --- | --- |
| `init` 之后（或第一次显示窗口） | 读一次需要的列表，建立初始模型 |
| `settings.open` | 设置页打开：重读 `config_list`（并保留未应用的本地修改） |
| `lists.changed` 且 `list` 与本消费方相关 | 重读该列表，重建模型（`i18n_list` 变化 = 语言变了：设置页/托盘刷新显示文本） |
| `config.applied` | 一批配置已落盘：结束等待、可重算派生状态 |
| `plugins.changed` | 已加载集合变了，`config_list` 可能变：整表重读 |
| 用户点「应用」 | 按 §3.3 / §3.4 发 `lists.update` / `lists.commit`，等结果事件 |

**禁止**每帧轮询 `list_json`：宿主有序列化缓存也扛不住 60Hz 调用，而且事件驱动已经够用。
事件回调跑在宿主分发线程上，只做"深拷贝 + 唤醒自己的线程"（设置页与托盘都是这么做的），
解析 JSON、重画 UI 都在自己的线程里做。

`lists.changed` 可能连续到达（一次启停可能级联多个插件），不要假设"一次操作一条通知"；
它也可能在消费方自己的写值/提交过程中到达（`config_list` 被重建）——刷新时按 `owner + id`
合并回未应用的本地修改，不要清掉用户正在编辑的值。

## 5. 已知 list id 登记表

**新列表必须在本节登记**：list id、条目 schema（字段 / 类型 / 必填 / 谁解释）、读写语义、
刷新时机。不登记就往列表里写，等于给半年后的人留一个没有文档的私有协议。

| list id | 类型 | 谁写 | 主要读方 | schema |
| --- | --- | --- | --- | --- |
| `config_list` | 宿主生成（只读投影） | 宿主从 `core` + 所有 `shot.configurable` 实例生成；插件写值走 `lists.update`、落盘走 `lists.commit` | 设置页 | §5.1 |
| `tray_command_list` | 宿主预置 core 四项 + 插件贡献 | 宿主预置 core；插件 `lists.add` / `lists.remove` | 托盘 | §5.2 |
| `i18n_source_list` | 插件 / 语言包贡献 | 插件在 `create()`/`init()` 里 `lists.add` 语言表（helper 拼好）；外部语言包由翻译插件贡献 `target`/`priority` 与 `@language/` 声明；宿主只读、合并生成 `i18n_list` | 宿主 | §5.3 |
| `i18n_list` | 宿主生成（只读快照） | 宿主按当前语言把 core 文案 + `i18n_source_list` 投影；插件不能 add/remove | 设置页 / 托盘 / 插件 helper | §5.4 |

### 5.1 `config_list`

宿主实时生成（`HostListCatalog::rebuildConfigListLocked`），**不接受插件 add/remove**。
条目 shape 与 `ShotConfigDesc` 一一对应，额外带上 owner：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `owner` | string | 提供者 id：宿主自己是 `"core"`，插件是它的插件 id（如 `example.invert`） |
| `owner_name` | string | 提供者显示名：**字面量或文本 key**（`core` 是 key `core.owner_name`）；消费方按 `display(owner, owner_name) ?? owner_name` 解析 |
| `owner_name_key` | string | 插件显示名的固定文本 key：插件条目恒为 `"plugin.name"`，core 为 `"core.owner_name"`；消费方按 `resolve(owner, owner_name_key) ?? owner_name` 解析（当前设置页对非 core 分区固定用 `plugin.name`，与字段值相同），老消费方可忽略 |
| `id` | string | 配置键（`ShotConfigDesc::key`），在同一 owner 内唯一；存盘键是 `owner/id` |
| `label` | string | 界面显示名，可以是**字面量或文本 key**；插件没给时宿主用 key 兜底。消费方按 `display(owner,label) ?? label` 用 `i18n_list` 解析（`document/I18N.md` §2.3） |
| `kind` | number | `SHOT_CONFIG_KIND_*`（1=Bool / 2=Int / 3=Float / 4=String / 5=Enum / 6=Color / 7=Path） |
| `value` | string | 当前值（一律字符串；读不出来时给空串并记 WARN） |
| `default` | string | 默认值；没有时为 `""` |
| `options` | string | 由设置页解释的交互提示：ENUM 是候选列表 `"low\|mid\|high"`；PATH 用 `"directory"` / `"file"` 声明对话框类型；STRING 用 `"hotkey"` 声明"按组合键录入"控件，以 `#` 开头的 `"#4C9AFF\|#34C759\|…"` 声明"预设色块 + 任意取色"控件；其它 kind 为 `""`（语义见本节末尾） |
| `min` / `max` / `step` | number | INT / FLOAT 的取值范围与步长；其它 kind 忽略 |
| `tooltip` | string | 悬停提示，同样允许是字面量或文本 key；没有时为 `""`，解析规则同 `label` |

- 顺序：**`core` 最前，然后按插件加载顺序**；同一提供者内按 `get_config_desc` 的声明顺序。
- 只包含**当前已加载**的实例：插件被禁用/卸载后条目消失（值仍留在 `config.json` 里），
  重新启用会先回灌已存值再重建列表。
- 插件不能往 `config_list` 里塞自己的运行期数据——那是另一种 list id 要做的事。
- `options` 的字符串语义**由设置页解释**（宿主只原样透传）：ENUM 是候选列表，PATH 是
  对话框类型，STRING 的 `"hotkey"` 是"按组合键录入"控件，STRING 的 `"#RRGGBB|…"` 是
  "预设色块 + 任意取色"控件（候选本身就是预设色，值统一 `#RRGGBB`）。给 `options`
  新增一种语义必须同步 `document/SETTINGS.md` §6 的控件映射表，否则它就成了没有文档的
  私有协议。
- ENUM 的候选值是**技术值**（如 `auto` / `debug` / `png`），显示名统一按
  `display(owner, 候选值)` 解析：core 已注册语言与日志级别的原生显示名；插件也可以给
  候选值贡献文案（key 就是候选值本身）。解析不到时显示技术值，不影响写值。
- **插件显示名**：`owner_name` 是 metadata 的 name 字面量；`owner_name_key` 是固定
  key。设置页对非 core 分区用固定 key `plugin.name`（即 `owner_name_key` 的值）先
  `resolve(owner, "plugin.name")`，没有译文时退回 `resolve(owner, owner_name) ??
  owner_name`；插件贡献 `target=自己, id=plugin.name` 的译文即可本地化配置分区标题。
- `core/language` 的 ENUM 候选来自语言目录，不是固定表：`auto` + 内置/外部 tag；
  `@language/` 声明增删会重建 `config_list`，`options` 整串重新序列化，
  语言下拉与 `i18n_list` 同步变化（`document/I18N.md` §3.1、§3.3）。

### 5.2 `tray_command_list`

托盘菜单的唯一数据源。宿主在 `HostListCatalog` 构造时预置 core 四项（owner = `"core"`，
插件不能 remove）：

```json
[
  {"id":"capture.request","label":"capture.request","event":"capture.request","data":"","order":10,"separator":false},
  {"id":"region.capture","label":"region.capture","event":"capture.request","data":{"mode":"region"},"order":15,"separator":false},
  {"id":"settings.open","label":"settings.open","event":"settings.open","data":"","order":20,"separator":false},
  {"id":"app.quit","label":"app.quit","event":"app.quit","data":"","order":100,"separator":false}
]
```

core 四项的 `label` 是**文本 key**（恰好等于命令 id）：托盘按
`display("core", label)` 用 `i18n_list` 解析成「截屏 / 区域截屏 / 打开设置 / 退出」
或英文。插件贡献的 `label` 同样允许 key 或字面量；托盘不认识 key 时原样显示。

`region.capture` 的 `data` 是 **JSON 对象** `{"mode":"region"}`（不是字符串）：托盘把
`data` 原样透传，点击后所有订阅者收到的事件负载就是这段 JSON 文本。`capture.request`
的 `mode=region` 语义见 `document/CAPTURE.md` §3.1——`plugins/region_selector` 收到
“带 mode、不带 region”的请求会弹框选覆盖层，用户确认后再发一条**带 `region`** 的
`capture.request`。这条触发请求会被流水线**忽略**（不采集、不落盘、不发 `capture.*`），
不会再额外存一张全屏图；如果 region_selector 没加载/被禁用，只是没人弹框选。

插件贡献的条目 schema（由托盘解释，宿主只用于 upsert 与排序）：

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `id` | string | ✅ | 稳定标识；upsert 键的一半，发布后不要改 |
| `owner` | string | 宿主注入 | 贡献者 id（`core` 或插件 id）；插件**不需要**在条目里写，宿主按列表级 owner 注入，托盘用它解析 `label` 文本 key |
| `label` | string | 命令必填 | 菜单文字：**字面量或文本 key**（UTF-8），托盘按 `display(owner,label)` 解析；为空时退回 `id`，两者都空则跳过该条目 |
| `event` | string | 命令必填 | 点击时发布的事件主题；`separator:true` 的条目可空 |
| `data` | 任意 JSON | 可选 | 事件负载：字符串原样发布，对象/数组/数字序列化成 JSON 文本发布，缺省/`null` = 空负载 |
| `order` | number | 可选 | 升序排序键，缺省 `1000`；同 order 保持宿主顺序 |
| `separator` | bool | 可选 | `true` = 渲染成分隔线（不可点击） |
| `default` | bool | 可选 | `true` = 左键单击托盘图标时执行该条目；多个时取排序后第一个 |

托盘不认识、也不直接调用任何插件：点击任何条目都只把条目声明的 `event`（负载 `data`）
发到事件总线。`label` 显示前按 `display(owner,label)` 用 `i18n_list` 解析；解析不到
（旧插件写中文原文、或没有 i18n 协议的老宿主）就原样显示。`event` 对应的主题有没有人听，条目本身不保证。

### 5.3 `i18n_source_list`

插件 / 语言包贡献的多语言文本（完整协议见 `document/I18N.md` §2.1、§3.3）。插件侧通常
不手写 JSON，而是用 `shot::I18n::loadTables()` 把作者写的"每语言一张 JSON 表"归一成
条目后 `lists.add`；外部语言包由翻译插件按同一 schema 贡献：

```json
{"list":"i18n_source_list","owner":"hotkey.main","items":[
  {"id":"config.capture_hotkey.label","texts":{"zh-CN":"截屏热键","en-US":"Capture hotkey"}},
  {"id":"settings.open","texts":{"ja-JP":"設定"},"target":"core","priority":200}
]}
```

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `id` | string | ✅ | 文案 key，在合并后的 `(target, id)` 内唯一；`@meta` 是保留 id（宿主拒绝）；`@language/<tag>` 是语言声明（不作文本条目） |
| `texts` | object / string | 文本条目 ✅；语言声明可省 | `语言标签 -> 文本`，标签按 BCP-47（大小写不敏感、`_` 视同 `-`）；空串视为缺失 |
| `target` | string | 可选 | 文本服务对象：`"core"` 或当前已加载插件 id；缺省 = 贡献者自己；非字符串或未加载 id → 丢弃 + WARN |
| `priority` | integer | 可选 | 同 `(target, id, 语言)` 的竞争权重，缺省 `100`；内置 100 / 官方包 200 / 用户包 300；高者胜，同 priority 后到者胜 |

- 合并键是 `(target, id, language)`；内置 core 文案先按 priority=100 合并，之后合并
  source 条目，所以同 priority 的 `target=core` 条目也会覆盖内置文本。
- `@language/<tag>` 只扩展语言目录：`texts` 可以是 `{tag: 显示名}` 或字符串，
  `fallback` 可选；显示名/fallback 的取值规则见 `document/I18N.md` §2.1、§3.3。
- 写：插件 `lists.add` / `lists.remove`；owner 必须是已加载插件 id；宿主按
  `(list, owner, id)` upsert；插件卸载时宿主 `removeOwner` 清掉贡献。
- 读：宿主只读，用来生成 `i18n_list`；插件**不要**直接读本列表做解析。
- 刷新：`lists.add`/`lists.remove` 真的改动内容、`@language/` 目录变化、插件启停/卸载后，
  宿主在下一次 `tick` 重建 `i18n_list` 并 `lists.changed`。
- 负载上限 64 KiB：helper 按 56 KiB 预算分块（留 8 KiB 外壳）；单条 item 自身超限时
  跳过并 WARN，`lists.remove` 的 `ids` 数组也分块。

### 5.4 `i18n_list`

宿主生成的当前语言快照，**只读**（插件不能 add/remove，写入口直接拒绝并 WARN）：

```json
[
  {"id":"@meta","language":"en-US","fallback":"en-US"},
  {"owner":"core","id":"core.owner_name","text":"Host"},
  {"owner":"core","id":"settings.open","text":"Open settings"},
  {"owner":"core","id":"auto","text":"Follow system"},
  {"owner":"core","id":"en-US","text":"English"},
  {"owner":"core","id":"fr-FR","text":"Français (untranslated)"},
  {"owner":"hotkey.main","id":"config.capture_hotkey.label","text":"Capture hotkey"}
]
```

`@meta` 结构：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `id` | string | 固定 `"@meta"` |
| `language` | string | 当前生效语言（BCP-47） |
| `fallback` | string | 当前语言回退链的下一站；由语言声明 / 注册表 fallback 计算，缺省 `en-US` |

其余条目：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `owner` | string | 贡献方：`core` 或插件 id |
| `id` | string | 文本 key；或语言 tag（语言下拉显示名） |
| `text` | string | 当前语言文本；语言显示名可能带「（未翻译）」后缀 |

- 条目顺序固定：`@meta` 最前，然后 core 文案，然后语言目录显示名（`owner="core"`,
  `id=<tag>`），最后是合并后的其余 `(target,id)` 文本；同一 `(owner,id)` 只出现一次。
- 解析规则：`display(owner, textOrKey) = i18n_list[(owner,textOrKey)] ?? textOrKey`，
  设置页/托盘/插件 helper 都按这一条实现。
- 生成时机：`HostRuntime::init` 在 `loadPlugins` **之前**先用内置 core 文案重建一次，
  加载完成后再重建；之后 `core/language` 变化、`i18n_source_list` 贡献变化、
  `@language/` 目录变化、插件启停/卸载清理都会在下一次 `tick` 重建并统一 flush。
- 消费方刷新：订阅 `lists.changed`，`list == "i18n_list"` 时刷新；不新增事件主题。

### 5.5 登记一个新列表的规则

1. 选 list id：全小写 + 下划线，建议以 `_list` 结尾；宿主内置列表用 `<名词>_list`
   （如 `config_list`），第三方列表加作者前缀避免与将来的内置列表撞名
   （如 `acme_targets_list`）。
2. 在 §5 表里加一行，并补一个小节写清：
   - 条目 schema：每个字段的类型、是否必填、缺省值、谁解释；
   - 谁写：宿主生成 / 哪个插件贡献 / 是否允许 `lists.update`（当前只有宿主生成的
     `config_list` 支持写值与提交）；
   - 何时刷新：哪些事件后消费方要回读；
   - 生命周期：owner 是谁，插件卸载时宿主清掉什么。
3. 贡献方用 `lists.add` 在运行期注册（可以在 `create()` / `init()` 里发，事件会入队，
   见 §6），消费方订阅 `lists.changed` 回读。
4. **不要**为它新增 ABI 结构体或宿主服务；如果发现"列表表达不了"（例如需要宿主在每帧
   回调、需要跨进程、需要强类型校验），先回到 §1 重新评估，别绕开列表协议另开一条路。

## 6. 生命周期

**启动**（`HostRuntime::init`）：加载插件 → 绑定列表目录 → `applyStoredLocked`（把已存值 /
默认值推给插件）→ `rebuildConfigListLocked`（生成 `config_list`）→ 订阅 `lists.*` 与
`app.quit`。插件在 `create()` 里就 `publish("lists.add", ...)` 不会丢：事件只是入队，宿主
加载完成后才订阅，主循环分发时才取订阅快照。

**启停（`HostRuntime::tick`）**，每轮按顺序做：

1. 若有 pending 启停：先放掉列表目录的提供者快照，再执行启停（整段持生命周期锁）；
2. 对比启停前后的已加载实例集合，对**消失的插件**逐个 `removeOwner(pluginId)`：清掉它在
   **所有插件贡献列表**里的条目并记 `lists.changed`（`config_list` 是宿主生成的，不在这里清）；
3. 重新启用的插件先回灌已存配置（`applyStoredLocked`）；
4. 无论本轮是启用、禁用还是只有失败，都按当前已加载集合**重建 `config_list`**；
5. `flushChanged()`：把本轮 dispatch 处理的事件与上面生命周期清理产生的变化合并，
   每个 list 发布一条 `lists.changed`；
6. 把脏配置落盘（`lists.commit` 是显式同步落盘的例外）。

**插件卸载/禁用**：宿主负责清掉该 owner 的全部贡献并发 `lists.changed`，插件**不需要**
（也不应该依赖）在 `deinit` 里发 `lists.remove` —— 那时实例已经被摘掉，宿主会按协议拒绝
非已加载 owner 的请求；清理逻辑在宿主侧统一做，才不会有"漏删一条死菜单项"。

**重新启用**：DLL 重新加载后插件重新 `create()`，在 `create()`/`init()` 里重新 `lists.add`
自己的条目即可（宿主已经清掉旧的，不会重复）。示例插件用 DLL 内 static 标记 + 实例销毁时
清零来保证"禁用→启用"能重新声明（见 `plugins/example_processor/README.md`）。

## 7. 线程、信任与降级

**线程模型**：

- **读**：`list_json` / `list_id` / `list_count` 任意线程可调，返回调用方缓冲里的副本。
- **写**：`lists.add` / `lists.remove` / `lists.update` / `lists.commit` 都只是 `publish`，
  任意线程可发；真正处理只在宿主 main 线程。宿主 `HostListCatalog` 构造时记下 main 线程
  id，非 main 线程直接调写入口会被拒绝并告警。
- **不要同步等待**：`lists.update` / `lists.commit` 没有同步返回，结果只从 `lists.updated` /
  `config.applied` 回来；在 `create` / `init` / `deinit` 回调里同步等这些事件会等不到
  （宿主在 main 线程串行处理，回调期间不会重入分发）。
- **不要在持锁路径里回调插件**：宿主调用插件能力（`set_config`、`get_config`）一律在
  生命周期锁内、数据锁外；插件代码不会在宿主数据锁里执行。

**信任模型（必须写清）**：事件总线**不携带发送者身份**（`ShotEvent` 只有主题 + 负载），
所以宿主只能校验 `owner` 是"当前已加载的插件 id"，**无法证明这条事件就是该插件发的**。
同一个进程里的插件本来就能读写宿主内存，这不是安全边界：

> **列表协议只防手滑，不防恶意。** 恶意插件可以直接破坏进程；协议要保证的是正常插件
> 写错 owner、写错字段、发错 JSON 时宿主安静丢弃并留一条 WARN，而不是因此崩掉或让列表
> 永远卡在坏状态。

因此：`owner` 必须等于发布者自己的插件 id（这是约定，也是插件被禁用/卸载时宿主能清理
贡献的前提）；宿主对 `owner` 的校验只做"这个 id 有没有已加载实例"。

**降级**：

- 老宿主没有 `shot.host.plugin_lists` → `query_interface` 返回 `NULL`，
  `shot::PluginLists::available()` 为 `false`，读列表返回空串；插件显示降级提示、其余功能照常。
- 列表不存在 / 读失败 / JSON 非法 → 消费方按"没有这份数据"处理（例如托盘只保留本地兜底
  的「退出」），不崩。
- `lists.add` 的负载坏、owner 不合法、条目缺 `id` → 宿主 WARN 并丢弃该条/该事件；
  `lists.update` 的单条参数坏 → 该项 `ok=false`，其它项照常处理。

## 8. 性能

- **宿主侧**：每个列表保存"已排序的条目 + 每条的序列化 JSON 对象字符串"，`list_json` 返回
  的内容按列表缓存；插件贡献列表变化只标脏，下一次读取时才重新拼装（`config_list` 重建时
  为了判断"内容是否真的变了"会立刻拼一次）。所以"没人读就不反复序列化"，
  "反复读也只是拷内存"。
- **消费方侧**：只在事件后刷新（`settings.open` / `lists.changed` / `config.applied` /
  `plugins.changed`），**不要每帧轮询**；解析出的模型缓存起来，事件来了才重解析。
- **读取用两段式**，别先猜一个固定大小缓冲反复试；`shot::PluginLists` 已经封装好。
- **事件里只放元数据**：`lists.changed` 只带 list id；`lists.add` 只带条目 JSON。像素、文件
  内容、大块二进制走能力/服务，不进总线，也不要塞进列表。
- 列表条目数量级是"UI 数据"（几个到几百条）；如果需要传输大列表或高频变化的数据，先回到
  §1 重新设计，不要把它当数据通道。

## 9. 代码示例

**贡献方（插件）**：在 `create()` / `init()` 里发一次 `lists.add`。

```cpp
// C++（插件侧）：事件总线封装。
#include "shot_plugin_helpers.h"

const char kPayload[] = R"json({
  "list": "tray_command_list",
  "owner": "acme.tools",          // 必须等于自己的插件 id
  "items": [
    {"id": "acme.capture", "label": "Acme 截屏", "event": "capture.request",
     "data": "", "order": 20}
  ]
})json";

shot::EventBus bus(host);
if (!bus.publish("lists.add", kPayload)) {
    // 老宿主没有总线 / 队列满：功能降级，不致命
}
```

**消费方（插件）**：读快照 + 按事件刷新。

```cpp
shot::PluginLists lists(host);
if (lists.available()) {
    const std::string text = lists.json("tray_command_list");
    // 解析一次，缓存在自己的模型里
}

shot::EventBus bus(host);
bus.subscribe("lists.changed", &on_lists_changed, this);   // 回调里只做入队 + 唤醒
// deinit 返回前：bus.unsubscribe(this);
```

**写值 / 提交（设置页，消费方 → 宿主）**：

```text
// 1) 写值（批量、带 tag）
{"list":"config_list","tag":"7","changes":[{"owner":"image_saver.main","id":"format","value":"jpeg"}]}
// 2) 等同 tag 的 lists.updated，逐项看 results[].ok / reason
// 3) 落盘（同 tag）
{"list":"config_list","tag":"7"}
// 4) 成功等 config.applied（空负载）；失败等同 tag 的 lists.updated + error
```

完整可用例子：设置页 `plugins/settings_ui/src/settings_apply.cpp`、
托盘 `plugins/tray/src/tray_menu.cpp` + `tray_icon_menu.cpp`、
示例插件 `plugins/example_processor/src/plugin_entry.cpp`。

## 10. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 每种数据一个 ABI 结构体 / 宿主服务 | 数据形状会一直长，契约必须冻结；list id + JSON schema 可以自由加字段 |
| 同步写接口（`set_value` / `save` 之类） | 跨 DLL 同步调用要在锁内碰插件，容易和热卸载互等；事件在 main 线程串行处理，天然单线程 |
| 把运行期贡献写进 metadata | metadata 是加载前的声明、可能被手改；运行期数据由 owner 兑现，卸载时宿主自动清理 |
| 每个条目一条细粒度变更事件 | 消费方总是读整份快照；合并成每 list 一条，事件量小、实现简单 |
| 发送者身份 / 权限校验 | 事件总线没有发送者字段；同进程信任模型，`owner` 只防手滑（§7） |
| 每帧轮询列表 | 序列化缓存再快也不该被渲染循环调用；事件驱动刷新（§8） |
| 列表持久化（`config_list` 里的配置值除外） | 运行期列表随插件加载重建；只有配置值需要落盘，走 `lists.commit` 写 `config.json` |
| 用列表传大对象（像素、文件内容） | 事件负载上限 64 KiB、队列 4096 条（`document/EVENT_BUS.md` §5）；大对象走专门的能力或服务 |

## 11. 检查清单

- [ ] 新增的数据形状 = 新 list id + JSON schema + `lists.*` 事件，没有新增 ABI 结构体/服务？
- [ ] 新 list id 已在 §5 登记（schema、读写语义、刷新时机、owner 生命周期）？
- [ ] list id 全小写、下划线、以 `_list` 结尾，第三方带作者前缀避免撞名？
- [ ] 运行期贡献没有写进 `metadata.json`？
- [ ] `lists.add` 的 `owner` 等于发布插件自己的 id？条目 `id` 稳定且在该 owner 内唯一？
- [ ] 条目字段只用 schema 里定义的；宿主不解释的字段由消费方按 schema 解释？
- [ ] 消费方只在 `settings.open` / `lists.changed` / `config.applied` / `plugins.changed`
      之后刷新，没有每帧轮询？
- [ ] 读取用两段式（`buf = NULL` 查长度；第二段 `buf_size > 长度` 给 NUL 留位）？
- [ ] 坏负载 / 未知 list id 是安静降级 + WARN，而不是报错或崩溃？
- [ ] 事件负载是合法 UTF-8 JSON、远小于 64 KiB，像素/大对象没进总线？
- [ ] 改 `lists.*` 负载了吗？→ 同步本文件 §3、`document/EVENT_BUS.md` §4、消费方/贡献方
      插件 `README.md`，以及 `document/ABI_POLICY.md`（若同时改了能力）。
