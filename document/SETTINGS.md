# 设置页与配置数据（`shot.host.plugin_lists` / `shot.host.plugin_registry`）

这份文档说明"设置页"这条链路：插件如何声明配置、宿主如何把配置聚成 `config_list` 快照、
设置窗口如何渲染、写值与提交如何走事件，以及禁用插件为什么必须热卸载。**改这条链路、
改 `plugins/settings_ui` 之前先读这里**；通用列表协议（读服务、事件负载、线程与信任模型）
见 `document/LISTS.md`。

## 1. 总体结构

宿主**没有主窗口**，设置窗口本身也是一个插件（`plugins/settings_ui`，实现
`shot.ui_provider`）。设置页这条链路只用到两个 append-only 宿主服务（列表服务 + 插件注册表）；
设置页不直接调用其他插件。

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| 配置声明 | 各插件的 `shot.configurable` | 插件说自己有哪些配置项（`ShotConfigDesc`） |
| 宿主设置 | `core` 伪插件（宿主内部实现 `ShotConfigurableV1`） | 宿主自己的设置，和插件配置用同一套描述符 |
| 配置数据 | `shot.host.plugin_lists` → `config_list` | 把上面两者聚成**只读 JSON 快照**；写值 / 落盘走 `lists.update` / `lists.commit` 事件 |
| 插件注册表 | `shot.host.plugin_registry` → `ShotHostPluginRegistryV1` | 已安装插件清单 + 异步启停（热卸载/重新加载） |
| 设置窗口 | `plugins/settings_ui` | 读 `config_list` 渲染描述符表单 + 插件管理页 + 应用/取消 |

数据流（设置页 UI 线程发起，宿主在 main 线程串行处理）：

```
settings_ui（自己的 UI 线程）                  宿主（main 线程）
  │ shot::PluginLists.json("config_list") ───▶ list_count/list_id/list_json（只读快照）
  │ query_interface(plugin_registry) ────────▶ installed_count/info/is_enabled（快照）
  │ publish lists.update {tag, changes} ─────▶ 逐项 set_config → 重建 config_list
  │                                            → publish lists.updated（同 tag，逐项结果）
  │ publish lists.commit {tag} ──────────────▶ saveConfigFile
  │                                            → 成功 publish config.applied（空负载）
  │                                            → 失败 publish lists.updated + error
  │ publish plugin_registry.set_enabled(id, 0/1) ▶ 只登记 pending，立刻返回
  │                                            tick：依赖级联 → deinit/destroy → 卸载 DLL
  │                                            → 存禁用列表 → 清理列表贡献 → publish plugins.changed
  │ ◀──────────── 事件总线（宿主分发线程回调）─────────────────────┘
```

设置页的订阅回调**只做入队与唤醒自己的 UI 线程**；所有 ImGui 绘制都只在它自己的
UI 线程里发生（见 `document/EVENT_BUS.md` 的线程模型）。

## 2. 配置数据：`config_list` + `lists.*` 事件

`config_list` 是**宿主生成**的只读投影（不是插件贡献的列表）：宿主把 `core` 与**所有当前
已加载、实现了 `shot.configurable` 的实例**的配置项，按 `ShotConfigDesc` 聚成 JSON 条目。
完整条目 schema（`owner` / `owner_name` / `id` / `label` / `kind` / `value` / `default` /
`options` / `min` / `max` / `step` / `tooltip`）与顺序规则见 `document/LISTS.md` §5.1。

| 操作 | 怎么做 |
| --- | --- |
| 读 | `shot.host.plugin_lists::list_json("config_list")`（两段式）；插件侧用 `shot::PluginLists::json()`。任意线程可读，拿到的是副本 |
| 写值 | 发布 `lists.update`：`{"list":"config_list","tag":"...","changes":[{"owner","id","value"}]}`；宿主逐项 `set_config` 并回 `lists.updated` |
| 落盘 | 发布 `lists.commit`：`{"list":"config_list","tag":"..."}`；成功回既有 `config.applied`（空负载），失败回 `lists.updated` + `error` |
| 刷新 | 订阅 `lists.changed`（`config_list` 重建、或 `i18n_list` 语言快照变化时）、`config.applied`、`plugins.changed`；打开设置页时再读一次 |

规则：

- **`label` / `tooltip` / ENUM 候选的显示名是 key 或字面量**：设置页统一按
  `display(owner, 值) = i18n_list[(owner,值)] ?? 值` 解析（`document/I18N.md` §2.3）。
  插件把 label 写成中文/英文原文也能工作，只是不随语言切换。
- **存盘键是 `<owner>/<id>`**（如 `example.invert/keep_alpha`、`core/language`），值一律
  字符串，落在宿主配置 JSON 的 `configItems` 里。插件只负责 `get_config` / `set_config`，
  **落盘归宿主**——插件不需要也不应该自己写配置文件。
- **启动时序**：`core::Config::init()` → 扫描并加载插件 → 宿主回灌已存值（没有已存值就用
  `default_value`，并记进配置）→ 重建 `config_list`。第一次运行后配置文件就是完整的。
- **写值与落盘分离**：`lists.update` 只改插件内存与宿主内存配置，`lists.commit` 才写
  `config.json`。设置页的"应用"因此是"先 update 等逐项结果，再 commit 等落盘"。
- **没有单独的 reload 调用**：值已经通过 `set_config` 推给插件了；`config.applied` 只是
  "这批值已提交"的通知，给缓存了派生状态的插件重算用。
- **热卸载后列表自动收缩**：插件被禁用/卸载后，它的条目在下一次重建时消失（值仍留在
  `config.json` 里）；设置页收到 `plugins.changed` / `lists.changed` 后必须整表重读，
  **不能继续用旧的 index 或旧声明里的字符串指针**。读一次 JSON 拿到的全是副本，天然安全。
- **插件在 `create` / `init` / `deinit` 里不要同步等待** `lists.update` / `lists.commit` 的
  结果，也不要调用会回调插件能力的宿主服务并阻塞——宿主的卸载路径持有同一把生命周期锁，
  互等会卡死宿主。只读的 `list_json` 可以在任意线程调用，不受这条限制。
- `plugins.disabled` 是宿主内部键（JSON 字符串数组，如 `["example.invert"]`），由宿主读写，
  插件不感知；它的值在插件扫描之前读取，所以"禁用 → 重启"仍然生效。

## 3. 插件注册表：`shot.host.plugin_registry`

`ShotHostPluginRegistryV1`（版本 1），插件侧封装是 `shot::PluginRegistry`。
禁用不是配置项，而是**插件生命周期**，所以单独一个服务。

| 字段 | 语义 |
| --- | --- |
| `installed_count` / `installed_info` | 已安装插件（**含被禁用与加载失败的**）的状态：id / name / version / path / state / reason |
| `is_enabled` | 1 = 启用，0 = 禁用，-1 = 没有这个 id；**加载失败（FAILED）仍返回 1**——用户没有关它，只是这次没起来，界面用 `state` 显示失败原因 |
| `set_enabled` | **异步**登记启用/禁用；0 = 已登记，-1 = 参数非法/没有这个 id，-2 = 宿主不接受（预留） |

状态常量：`SHOT_PLUGIN_STATE_LOADED`(1) / `SHOT_PLUGIN_STATE_DISABLED`(2) /
`SHOT_PLUGIN_STATE_FAILED`(3)。加载失败也占一条记录（`reason` 里写原因），
这样用户能看到"为什么这个插件没出现"。

### 3.1 禁用 = 热卸载

勾选禁用并应用后，宿主必须在运行期真的把插件摘掉，而不是等下次启动：

```
已加载 ──set_enabled(id, 0)──▶ 待卸载（pending）
  ├─ 有已加载插件 requires 它（id 或 provides 名）→ 拒绝，状态不变，plugins.changed 带原因
  ├─ UI 插件：先 post "host.shutdown" 再 deinit（插件返回前必须停线程、退订、销毁窗口）
  ├─ destroy 实例；同包还有别的已加载实例 → 只摘这个实例，DLL 留着
  ├─ 同包最后一个实例也没了 → module->shutdown() → 放掉最后一个 DLL 句柄（FreeLibrary）
  ├─ 写入禁用列表（`plugins.disabled`）→ publish plugins.changed
  └─ 清掉该 owner 在所有贡献列表里的条目 + 重建 config_list → lists.changed
```

重新启用走反方向：包 DLL 已卸载就重新 `LoadLibrary` + `module->init` + `create`，然后宿主
重新回灌已存配置（值来自 `config.json`），并把插件的配置项重新加回 `config_list`；包 DLL
还在就只重新 create 实例。

**启用级联**：启用一个插件前，宿主先递归启用它 `requires` 里的提供者（深度优先）：

- 跳过 `core`（宿主自身，没有实体可启用）；提供者按"插件 id"或 `metadata.provides`
  里的名字匹配，与加载期解析依赖用同一套规则；
- `visited` 跨整棵依赖树共享：既防元数据成环，也保证同一个提供者只启用一次；
- 提供者启用失败 → 整条启用失败，`reason` 形如
  `依赖 <providerId> 启用失败：<下层原因>`，设置页把原因显示在插件行上；
- 已经 LOADED 的提供者按幂等成功处理，不会重复 create；
- 找不到提供者不在这里判失败，交给单插件启用按现状尝试（与启动期"元数据只是声明"的
  宽松策略一致）；
- 部分成功不回滚：级联里已启用的 provider 保持启用，宿主按实际状态写回
  `plugins.disabled`（配置与运行时保持一致），只是本次请求返回失败。

**启动入口兜底**：常驻模式启动时，宿主检查注册表里 `tray.main` 是否为 LOADED；
不是（被禁用 / 加载失败 / 未安装）就自动发布一次 `settings.open`，并在日志里写
`tray.main 未加载（state=N / 未安装），自动发布 settings.open 作为入口兜底`。
这样把托盘禁用后重启，也不会既没有托盘、又看不到设置页；托盘正常时启动不会自动打开设置页
（`--settings` 仍可显式打开；两者同时命中只发布一次）。

硬性要求：

- **加载/卸载只在宿主 main 线程执行**（与事件分发同线程），设置页只发请求。
- 插件 `deinit` 必须自己停线程、退订事件总线、销毁窗口——热卸载让它从"建议"变成硬要求。
  `PluginType::Generic` 没有 `deinit`（见 §3.3）：订阅在 `create`、退订在 `destroy`；
  自己起后台线程的 Generic 插件必须在 `destroy` 里停线程并 `join`。
- **被依赖的插件不允许单独禁用**：v1 直接拒绝并提示"被 X 依赖"，不做级联禁用。
- **只有设置页自己不允许禁用**（`settings_ui.main`）：设置页把这一行置灰，宿主不为此
  增加特判。托盘 `tray.main` 是普通可启停插件——热卸载会注销托盘窗口类，重新加载时
  可以再注册，因此允许反复禁用/启用。
- 卸载后如果重新加载失败（`LoadLibrary`/`init`/`create` 任一失败），记录
  `SHOT_PLUGIN_STATE_FAILED` + 原因，禁用列表保留；用户可以再试一次。
- **生命周期锁不可重入**：插件的 `create` / `init` / `deinit` 回调里不要同步等待
  列表写路径（`lists.update` / `lists.commit`）的结果，也不要在这些回调里等待一个正在
  调用插件的线程——宿主的卸载路径持有同一把锁，互等会卡死宿主。只读的 `list_json`
  任意线程可调，不受这条限制。入口 UI 插件（`settings_ui.main`）正是设置页本身，
  不允许禁用。

### 3.2 `plugins.changed` 负载

`set_enabled` 真正执行完（成功或拒绝）后，宿主对**每个状态变化**各发布一条 JSON
（UTF-8，不含 NUL）。一次 `set_enabled` 可能产生多条（级联启用的 provider 各一条），
设置页不能假设一次点击只收到一条通知。

```json
{"plugin":"example.invert","enabled":false,"state":2,"reason":""}
```

| 字段 | 含义 |
| --- | --- |
| `plugin` | 插件 id |
| `enabled` | 处理后的状态（true = 已启用/已加载） |
| `state` | `SHOT_PLUGIN_STATE_*` 的数值 |
| `reason` | 拒绝/失败原因；成功时为空串 |

- 被拒绝 / 失败的请求即使状态没变，也会收到一条带 `reason` 的通知（否则设置页拿不到
  "为什么没生效"）；
- 状态没变、也没有拒绝/失败原因的请求不发通知（幂等请求不打扰设置页；设置页的
  "应用中…"有 5 秒超时兜底）；
- 设置页每收到一条通知就重新枚举注册表并重读 `config_list`；如果它正在"应用中…"，
  第一条通知就会结束等待，后续通知继续刷新列表。

### 3.3 `PluginType::Generic`：纯订阅者 / 纯配置项插件

宿主把「有具名能力」的插件按五个包装类分类（图像处理 / 库提供者 / UI / 采集 / 保存）。
**五种都认不出来、但 metadata 声明过它的插件**，按 `PluginType::Generic` 用基类包装
继续加载：

- 注册表照常列 LOADED，可以像其它插件一样启用/禁用/热卸载；
- `config_list` 照常用 `query_interface(shot.configurable)` 枚举它的配置项——
  "没有宿主可调用的能力"不等于"没有配置项"；
- 加载条件：**必须有 metadata**。没有 metadata 又没有宿主认识的任何具名能力时，宿主会
  销毁实例并跳过（`reason = "没有 metadata 且没有本宿主认识的任何能力"`），不把来源
  不明的 DLL 挂进宿主。这也是 metadata「加载前声明」价值的落点。
- **没有 `init` / `deinit` / `post_to_plugin`**：事件订阅必须在 `create(index, host)`
  里建立（这是唯一能拿到宿主服务表的时机），在 `destroy` 里退订。宿主热卸载/退出时先
  让实例走 `destroy`（插件自己收线程、退订），再 `module->shutdown`、最后放 DLL 句柄。
- 判定口径：**有没有 `shot.ui_provider` 只取决于"宿主是否需要给插件投 `post_to_plugin`"**。
  自己开线程/建隐藏窗口、但不需要宿主投递事件的插件（如 `plugins/notify`）仍然是
  Generic，不需要假装成 UI 插件。
- 现有例子：`plugins/clipboard`（只声明 `shot.configurable`，在 `create` 里订阅
  `capture.done`）、`plugins/notify`（`shot.configurable` + 订阅
  `capture.done` / `capture.failed` / `app.quit`，通知线程在 `create` 起、`destroy` 收）。

## 4. 应用 / 取消

设置页采用**本地编辑 + 应用/取消**：打开时把值读进本地副本，拖滑块只改副本，
点应用才写回，点取消直接丢弃（避免每帧调用 `set_config` 打断插件）。

```
点击"应用"：
  1. 脏配置项打包发布 `lists.update`（带本插件自增的 tag）
       {"list":"config_list","tag":"7","changes":[{"owner":..,"id":..,"value":..}, ...]}
  2. 宿主逐项 set_config → 重建 config_list → 发布 `lists.updated`（同 tag、逐项 ok/reason）
  3. `ok=false` 的项标红显示 reason 并保留脏标记；其余项以提交值为新基线
  4. 自动发布 `lists.commit`（同 tag）：{"list":"config_list","tag":"7"}
       ├─ 成功 → 宿主发布 config.applied（空负载，不回显 tag）
       └─ 失败 → 宿主发布 lists.updated（同 tag，results 为空、带 error）
  5. 配置阶段结束后再登记插件启停（shot.host.plugin_registry.set_enabled）
     等 plugins.changed（一次启用可能级联多条）
  6. 每一步都有 5 秒超时兜底；超时/失败都保留未应用的修改，可以重试
```

`config.applied` / `lists.updated` / `plugins.changed` 都是**通知**，不是"让插件重新读配置"
的命令——值在步骤 1 已经推给插件了。`lists.commit` 只负责**落盘**，不是事务：没有
begin/rollback，失败时以内存里的最新值为准（宿主下一轮 tick 还会尝试落盘）。

批量中间态：`lists.update` 会一条条到达插件，插件可能短暂看到半套值。约定：**插件只在
收到 `config.applied` 之后才做不可逆的全局重算**；单值级别的副作用就在 `set_config` 里做。
以后需要严格事务时再加 begin/commit，v1 不加。

**tag**：消费方自增的字符串，宿主在 `lists.updated` 里原样回显。`config.applied` 是既有
主题、负载为空、**不带 tag**，所以设置页约定"处于等待 commit 时收到 `config.applied`
即成功"。事件是广播的，其它消费者也可能收到别人的响应——按自己的 tag 过滤，不匹配就忽略。

### 4.1 相关主题

| 主题 | 方向 | 负载 |
| --- | --- | --- |
| `settings.open` | 托盘（`plugins/tray`）→ 设置页 | 空 |
| `hotkey.suspend` | 设置页 → hotkey | 空；开始"按组合键录入"时发布，hotkey 插件撤销全部全局注册（见 §6） |
| `hotkey.resume` | 设置页 → hotkey | 空；结束/取消录入时发布，hotkey 插件按当前配置恢复注册 |
| `lists.update` | 设置页 → 宿主 | JSON，见 §2 与 `document/LISTS.md` §3.3 |
| `lists.commit` | 设置页 → 宿主 | JSON，见 §2 与 `document/LISTS.md` §3.4 |
| `lists.updated` | 宿主 → 所有 | JSON：`{"list","tag","results":[{"owner","id","ok","reason"}],"error"?}` |
| `lists.changed` | 宿主 → 所有 | JSON：`{"list"}`；`config_list` 被重建时通知设置页刷新 |
| `config.applied` | 宿主 → 所有插件 | 空（`lists.commit` 成功后发布） |
| `plugins.changed` | 宿主 → 所有插件 | JSON，见 §3.2；一次 `set_enabled` 可能发多条（级联启用） |

托盘菜单「打开设置」发布 `settings.open`；宿主不再在启动时自动打开设置页
（`--settings` 只是调试入口，见 `README.md`「运行」）。

## 5. 宿主自己的设置（`core` 伪插件）

宿主内部实现一个 `ShotConfigurableV1`（id `core`，显示名是 key `core.owner_name`，
version 来自 `version.h`），`config_list` 里 `owner = "core"` 的条目排在最前。于是
"宿主设置"不是特例：和插件配置一样被枚举、编辑、写值、提交，设置页只写一套渲染逻辑。

| key | kind | 默认 | 副作用 |
| --- | --- | --- | --- |
| `language` | ENUM `auto\|<语言注册表 tag>`（当前 27 个） | `auto` | 立即切换界面语言；下一次 `HostRuntime::tick` 重建 `i18n_list` 并通知消费方（`document/I18N.md` §3；当前只有 zh-CN/en-US 有译文，其余回退 en-US） |
| `logging.level` | ENUM `debug\|info\|warn\|error` | `info` | 立即 `spdlog::set_level`（host / plugin 两个 logger） |
| `debug.console` | BOOL | `1` | 立即开/关控制台 sink（文件日志不受影响） |
| `logging.file` | BOOL | `1` | 立即开/关文件 sink（`<dataPath>/logs/host.log`） |

`language` 的 ENUM 候选与原生显示名都来自语言注册表（`src/hostI18n.cpp`），
不是硬编码在配置表里；加语种只加注册表行，下拉显示名由 `i18n_list` 自动投影。

**宿主设置里刻意没有 `output.*`**：输出目录/格式/质量/剪贴板这类"保存"相关的配置不在宿主
设置里，由实现保存/剪贴板能力的插件用 `shot.configurable` 自己声明
（`plugins/image_saver` 的 `directory` / `format` / `quality` / `filename`，
`plugins/clipboard` 的 `enabled` / `also_file_drop`）；设置页按 `config_list` 自动多出
对应小节，宿主代码不参与。

## 6. 设置页插件（`plugins/settings_ui`）

- **单窗口、单实例**：插件 `init` 时创建隐藏窗口并起自己的 UI 线程；收到 `settings.open`
  显示；关闭（X / Esc）只隐藏，不销毁线程；`deinit` 时停线程、销毁窗口。
- **ImGui + GLFW + OpenGL3**：ImGui 静态编译进插件（vcpkg `imgui` 的
  `glfw-binding` + `opengl3-binding`），窗口/上下文/输入用 GLFW（跨平台）；宿主完全不依赖
  UI 库、也拿不到窗口句柄。运行期插件依赖 `bin/glfw3.dll`、`bin/nfd.dll`
  （由 `cmake/copy_runtime_deps.cmake` 从 vcpkg 运行时目录布置，插件目录不含这些 DLL）。
- **中文字体**：按平台候选加载系统中文 TTF/TTC（msyh / PingFang / Noto CJK / WenQuanYi），
  失败时退回 ImGui 默认字体（只是显示效果降级，不影响功能）。
- **布局**：一页滚动，按提供者分节（`CollapsingHeader`，宿主 `core` 在最前）：
  1. 宿主设置；
  2. 插件管理（注册表表格：插件 / 版本 / 状态 / 启用复选框）；
  3. 每个可配置插件一节。
- **控件映射**（`ShotConfigDesc::kind`）：

| kind | 控件 | 值格式 |
| --- | --- | --- |
| BOOL | Checkbox | `"1"` / `"0"` |
| INT / FLOAT | Slider + Input（用 `min/max/step`，越界 clamp） | 十进制字符串 |
| ENUM | Combo（`options` 按 `\|` 切） | 选项字符串 |
| STRING | InputText；`options == "hotkey"` 时换成"当前组合键 + 录入…"（见下方热键约定） | 原样 / 规范化组合键 |
| COLOR | ColorEdit4 + hex 输入 | `"#RRGGBB"` 或 `"R,G,B,A"`（统一成 `#RRGGBBAA` 存盘） |
| PATH | InputText + "浏览…"（对话框类型见下方约定） | 原样 |
| 未知 kind | 跳过并显示灰色"不支持的配置类型" | —— |

- **PATH 的对话框类型**：描述符的 `options` 用来声明该 PATH 指文件还是目录——
  `"directory"`（也接受 `"dir"` / `"folder"`）弹"选择文件夹"，`"file"` 弹"打开文件"，
  空或其它值则按当前值推断（当前值是已存在目录时选目录，否则选文件）。保存目录这类配置项
  必须写 `"directory"`，否则默认值为空时会弹成选文件。
- **热键组合的录入控件**：`kind == STRING` 且描述符 `options == "hotkey"`（大小写不敏感）时，
  设置页不再画文本框，而是显示当前组合键 + 「录入…」按钮。点按钮弹一个**模态窗**独占键盘
  （其它控件不响应），按下 `[Ctrl+][Alt+][Shift+][Win+]<A-Z|0-9|F1..F12|PrintScreen>` 就把**规范化文本**
  （修饰键固定顺序 `Ctrl+Alt+Shift+Win`；小键盘数字按 `0-9` 算）写回本地缓冲——仍然走
  §4 的"本地编辑 + 应用"流程，不会立刻写宿主。Esc 取消（值不变）；Backspace / Delete
  清空：空串的语义是**"这个动作不设热键"**，hotkey 插件解析时按"热键为空" warn 跳过该项，
  不报错、也不让插件失败。裸键（无修饰键，如 `F9` 或裸字母）语法合法、允许录入，但会
  拦截正常输入，界面用提示行说明（用户配置责任，设置页不擅自改）。录入期间设置页发布
  `hotkey.suspend`、结束/取消时发布 `hotkey.resume`，让 hotkey 插件撤掉/恢复全局注册，
  避免按下旧组合键先触发真实动作（比如立刻截一张图）；hotkey 插件未加载或宿主没有事件
  总线时安静降级（录入照常可用，只是不能避免触发已注册热键，日志记 WARN）。旧配置里的
  非法值**原样显示、不做语法校验**——解析仍归 hotkey 插件，注册时非法只 warn 跳过；重新
  录入一次即被规范化覆盖。录入中关窗/隐藏/宿主退出时，窗口层取消录入并补发
  `hotkey.resume`（漏发会让热键永久停在暂停状态）。**PrintScreen 的 Win11 系统拦截**：
  Windows 11 默认把裸按 PrintScreen 交给系统截图工具（设置→辅助功能→键盘→"使用
  Print Screen 键打开截图工具"），此时 `RegisterHotKey` 成功但 `WM_HOTKEY` 可能收不到；
  带修饰键的组合（如 `Ctrl+PrintScreen`）不受影响。要用裸 PrintScreen 需关闭该 Windows
  设置，hotkey 插件注册裸 PrintScreen 时会打 WARN 提示。
- **每项**：label + 悬停 `tooltip` + "恢复默认"小按钮（写 `default_value` 到本地副本）。
- **状态**：脏标记（没有脏项时"应用"置灰）；`lists.updated` 里 `ok=false` 的项标红
  "写入失败：reason"并保留脏标记；插件已被卸载 → 该节灰掉；窗口位置/大小存宿主配置的
  `settings_ui.window`（`"x,y,w,h"`），下次显示时恢复。
- **插件管理**：只有 `settings_ui.main` 自己那一行置灰（入口插件不能禁用自己）；禁用行
  显示原因；勾选只改本地状态，应用时才 `set_enabled`。

### 6.1 DPI 与多显示器

宿主在启动最早期（任何窗口创建 / 屏幕采集之前、日志初始化之前调用，结果随后写日志）
显式声明 **per-monitor v2 DPI 感知**：`user32!SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`
→ 回退 `shcore!SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)` → 回退
`SetProcessDPIAware()`，全部失败只 warn、不阻止启动。日志行是 `DPI awareness: <模式>`。
不声明的话，没有设置页的进程默认 DPI-unaware，采集会拿到逻辑坐标（150% 下
3584x1600 的虚拟桌面会变成 2389x1066 一类），设置页一初始化 GLFW 又"碰巧"改对——
必须由宿主统一声明。

设置窗口自己的 DPI 处理：

- **content scale 只用于字号，绝不用于坐标**：GLFW 报告的是物理像素坐标；代码把
  content scale 传给 `ScaleAllSizes` + `FontScaleMain` 放大字体，并且每帧从"未缩放的
  默认样式基线"重算，**乘法缩放不累积**。
- **跨屏拖动**：GLFW content scale 回调置脏标记，下一帧按新屏 DPI 重缩放；运行中
  150% ↔ 100% 拖动不会字体变大两次或变小两次。
- **几何恢复按目标显示器 DPI clamp**：`settings_ui.window` 保存的是内容区 `x,y,w,h`；
  恢复时先把（隐藏）窗口搬到目标显示器，再按**目标屏** DPI 取标题栏/边框尺寸，把外层
  矩形 clamp 进目标屏 `glfwGetMonitorWorkarea`（先夹尺寸、再夹位置），换分辨率 / 拔掉
  副屏后窗口不会跑到屏幕外或只露出标题栏。

**已验证**（本机 Windows 11，双屏混合 DPI）：主屏 150% 2560x1600 @(0,0) +
副屏 100% 1024x768 @(2560,0) 下——运行中跨屏拖动自动重缩放且不累积；几何恢复到
另一块屏时第一帧就按目标 DPI 正确；按目标屏工作区（含边框）clamp 通过。

**未验证**（代码按 GLFW content scale / 显示器矩形的通用逻辑写，不是硬编码 1.5/1.0，
但没有真机结论）：其它 DPI 组合（125% / 200% 等）、三屏及以上、多屏负 origin
（副屏在主屏左侧/上方）、POSIX 多屏。详见 `plugins/settings_ui/README.md` §2。

采集侧另有自己的兜底：`screen_capture` 在每次 `describe` / `capture` 进入平台层时
临时把调用线程的 DPI 上下文提升到 PMv2，返回前恢复；所以即使宿主线程原本是别的
上下文，截图尺寸与像素也仍是物理值（见 `document/CAPTURE.md` §2.1）。

## 7. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 单独的 reload 调用 / 消息 | 值已经通过 `lists.update` → `set_config` 推给插件；再加一层"请重读"只会多一种状态不同步 |
| begin/commit 事务 | `lists.commit` 只负责落盘；v1 用 `config.applied` 兜底批量通知，插件各自在 `set_config` 里做单值副作用 |
| 级联禁用 | 被依赖时直接拒绝更可解释。启用则按相反方向级联（见 §3.1）；禁用不做级联。 |
| 在设置页安装/删除插件 | 只管理**已安装**插件的启停；安装属于另一个层（安装器） |
| 多个设置窗口 | 单实例足够；多实例会带来"两处同时改同一项"的冲突 |
| 把 `output.*` 塞进宿主设置 | 保存/剪贴板逻辑在各自插件里（`plugins/image_saver` / `plugins/clipboard`），配置跟着插件自己走 |

## 8. 检查清单

- [ ] 新增配置项只改插件自己的 `kConfig[]`，`config_list` 自动多一条，没有改宿主代码？
- [ ] 新增的是**纯订阅者 / 纯配置项插件**？→ 确认它在 metadata 里声明过、订阅在 `create`
      建立 / `destroy` 退订，并且没有为了拿 `post_to_plugin` 而假装 `shot.ui_provider`（§3.3）
- [ ] 配置值一律用字符串，没有跨边界传结构体/数字？
- [ ] 不认识的 key / 不认识的 kind 是安静跳过，而不是报错崩溃？
- [ ] 读写配置只走 `lists.update` / `lists.commit` / `lists.changed` / `config.applied`，
      没有同步写接口？
- [ ] 改了列表协议（`config_list` 字段、`lists.*` 负载）？→ 同步 `document/LISTS.md`
      登记表与 §3、`document/EVENT_BUS.md` §4、本文件、相关插件 `README.md`
- [ ] 给 `ShotConfigDesc::options` 新增了语义（如 `"hotkey"`）？→ 同步 `document/LISTS.md`
      §5.1 与本文件 §6 的控件映射表，以及提供方/消费方插件的 `README.md`
- [ ] 改了两个宿主服务之一？→ 同步 `include/plugin_api.h`、`document/ABI_POLICY.md`
      登记表与布局锚点、`SHOT_ABI_MINOR`、各插件 `metadata.json` 的 `abi.minor`
- [ ] 涉及插件卸载？→ 插件 `deinit` 里停线程、退订总线、销毁窗口了吗？
- [ ] 改了启停/级联/入口兜底或 `plugins.changed` 语义？→ 同步本文件 §3.1–§3.2 与 README
