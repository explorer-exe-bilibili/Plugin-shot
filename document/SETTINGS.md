# 设置页与配置目录（`shot.host.plugin_config` / `shot.host.plugin_registry`）

这份文档说明"设置页"这条链路：插件如何声明配置、宿主如何把配置聚成一张目录、
设置窗口如何渲染与提交、禁用插件为什么必须热卸载以及它的状态机。**改这两个服务、
改 `plugins/settings_ui` 之前先读这里。**

## 1. 总体结构

宿主**没有主窗口**，设置窗口本身也是一个插件（`plugins/settings_ui`，实现
`shot.ui_provider`）。宿主只提供两个 append-only 宿主服务；设置页不直接调用其他插件。

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| 配置声明 | 各插件的 `shot.configurable` | 插件说自己有哪些配置项（`ShotConfigDesc`） |
| 宿主设置 | `core` 伪插件（宿主内部实现 `ShotConfigurableV1`） | 宿主自己的设置，和插件配置用同一套描述符 |
| 配置目录 | `shot.host.plugin_config` → `ShotHostPluginConfigV1` | 把上面两者聚成一张可枚举目录，转发读写，负责存盘与启动回灌 |
| 插件注册表 | `shot.host.plugin_registry` → `ShotHostPluginRegistryV1` | 已安装插件清单 + 异步启停（热卸载/重新加载） |
| 设置窗口 | `plugins/settings_ui` | 描述符驱动表单 + 插件管理页 + 应用/取消 |

数据流（设置页 UI 线程发起，宿主负责把调用安全地转到插件）：

```
settings_ui（自己的 UI 线程）                  宿主（main 线程）
  │ query_interface(plugin_config)               │
  │ configurable_count/info/desc ──────────────▶ │ 目录：索引 0 = core，其余 = 可配置插件
  │ get_value / set_value ─────────────────────▶ │ 同步转调插件的 get_config / set_config
  │ save ──────────────────────────────────────▶ │ core::Config 落盘 → publish config.applied
  │ query_interface(plugin_registry)             │
  │ installed_count/info/is_enabled ───────────▶ │ 注册表快照
  │ set_enabled(id, 0/1) ──────────────────────▶ │ 只登记 pending，立刻返回
  │                                              │ tick：依赖检查 → deinit/destroy → 卸载 DLL
  │                                              │      → 存禁用列表 → publish plugins.changed
  │ ◀──────────── 事件总线（宿主分发线程回调）─────────────────────┘
```

设置页的订阅回调**只做入队与唤醒自己的 UI 线程**；所有 ImGui 绘制都只在它自己的
UI 线程里发生（见 `document/EVENT_BUS.md` 的线程模型）。

## 2. 配置目录：`shot.host.plugin_config`

`include/plugin_api.h` 的 `ShotHostPluginConfigV1`（版本 1），插件侧封装是
`shot_plugin_helpers.h` 里的 `shot::PluginConfig`。

| 字段 | 语义 |
| --- | --- |
| `configurable_count` | 当前可配置提供者个数；**索引 0 固定是宿主自己**（id `"core"`） |
| `configurable_info` | 取第 index 个提供者的 id / name / version |
| `config_count` | 按提供者 id 取配置项个数；id 不认识返回 0 |
| `config_desc` | 取第 item 项 `ShotConfigDesc`；返回前校验 key 非空、kind 已知 |
| `get_value` / `set_value` | 转调提供者的 `get_config` / `set_config`；`set_value` 成功时把值记入宿主配置 |
| `apply_stored` | 把已存值灌回所有提供者；没有已存值时用 `default_value` 并记入配置 |
| `save` | 配置落盘；**成功**返回 0 并发布 `config.applied`，落盘失败返回负数、不发布 |

规则：

- **存盘键是 `<plugin_id>/<key>`**（如 `example.invert/keep_alpha`、`core/logging.level`），
  值一律字符串，落在宿主配置 JSON 的 `configItems` 里。插件只负责 get/set，
  **落盘归宿主**——插件不需要也不应该自己写配置文件。
- **启动时序**：`core::Config::init()` → 扫描并加载插件 → `apply_stored()` 把旧值灌回插件。
  第一次运行时没有旧值，就用 `default_value`，于是第一次运行后配置文件就是完整的。
- `set_value` 同步生效（值已经通过 `set_config` 推给插件），所以**没有单独的 reload 调用**。
  `save()` 的 `config.applied` 只是"这批值已提交"的通知，给缓存了派生状态的插件重算用。
- 提供者被热卸载后，`configurable_count` 与索引会变化：设置页收到 `plugins.changed`
  后必须整表重新枚举，不能继续用旧的 index / 旧声明里的字符串指针。
- 所有函数任意线程可调；宿主用生命周期锁保证"读值/写值"期间对应插件不会被卸载。
- `plugins.disabled` 是宿主内部键（JSON 字符串数组，如 `["example.invert"]`），
  由宿主读写，插件不感知；它的值在插件扫描之前读取，所以"禁用 → 重启"仍然生效。

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
  └─ 写入禁用列表（`plugins.disabled`）→ publish plugins.changed
```

重新启用走反方向：包 DLL 已卸载就重新 `LoadLibrary` + `module->init` + `create`，
然后 `apply_stored` 把该插件的已存配置再灌回去；包 DLL 还在就只重新 create 实例。

硬性要求：

- **加载/卸载只在宿主 main 线程执行**（与事件分发同线程），设置页只发请求。
- 插件 `deinit` 必须自己停线程、退订事件总线、销毁窗口——热卸载让它从"建议"变成硬要求。
- **被依赖的插件不允许单独禁用**：v1 直接拒绝并提示"被 X 依赖"，不做级联禁用。
- **入口插件不允许禁用**（当前的 `settings_ui.main`，以后的托盘/热键插件）：
  设置页把这些行置灰，宿主不为此增加特判。
- 卸载后如果重新加载失败（`LoadLibrary`/`init`/`create` 任一失败），记录
  `SHOT_PLUGIN_STATE_FAILED` + 原因，禁用列表保留；用户可以再试一次。
- **生命周期锁不可重入**：插件的 `create` / `init` / `deinit` 回调里不要同步调用
  配置目录服务（`shot.host.plugin_config`），也不要在这些回调里等待一个正在调用
  配置目录的线程——宿主的卸载路径持有同一把锁，互等会卡死宿主。入口 UI 插件
  （`settings_ui.main`）正因为这条规则不允许禁用。

### 3.2 `plugins.changed` 负载

每次 `set_enabled` 真正执行完（成功或拒绝）后发布一条 JSON（UTF-8，不含 NUL）：

```json
{"plugin":"example.invert","enabled":false,"state":2,"reason":""}
```

| 字段 | 含义 |
| --- | --- |
| `plugin` | 插件 id |
| `enabled` | 处理后的状态（true = 已启用/已加载） |
| `state` | `SHOT_PLUGIN_STATE_*` 的数值 |
| `reason` | 拒绝/失败原因；成功时为空串 |

设置页收到后重新枚举注册表与配置目录。

## 4. 应用 / 取消

设置页采用**本地编辑 + 应用/取消**：打开时把值读进本地副本，拖滑块只改副本，
点应用才写回，点取消直接丢弃（避免每帧调用 `set_config` 打断插件）。

```
点击"应用"：
  1. 脏配置项逐条 plugin_config.set_value(provider, key, value)   // 同步生效，失败项标红
  2. 变化的启停逐条 plugin_registry.set_enabled(id, enabled)      // 只登记
  3. plugin_config.save()                                         // 落盘 + publish config.applied
  4. 宿主 main 线程下一轮执行启停，完成后 publish plugins.changed
  5. 设置页收到通知：刷新模型、清脏标记；等待期间显示"应用中…"
```

`config.applied` 负载为空。它和 `plugins.changed` 都是**通知**，不是"让插件重新读配置"
的命令——值在步骤 1 已经推给插件了。

批量中间态：`set_value` 会一条条到达插件，插件可能短暂看到半套值。约定：**插件只在
收到 `config.applied` 之后才做不可逆的全局重算**；单值级别的副作用就在 `set_config` 里做。
以后需要严格事务时再加 `begin/commit`，v1 不加。

### 4.1 相关主题

| 主题 | 方向 | 负载 |
| --- | --- | --- |
| `settings.open` | 托盘/热键（测试期是宿主自己）→ 设置页 | 空 |
| `config.applied` | 宿主 → 所有插件 | 空 |
| `plugins.changed` | 宿主 → 所有插件 | 见 3.2 的 JSON |

测试阶段宿主在加载完插件后自己发布一次 `settings.open`，让窗口随程序启动打开；
代码里留 `// TODO`，等托盘/热键插件就位后改由它发布。

## 5. 宿主自己的设置（`core` 伪插件）

宿主内部实现一个 `ShotConfigurableV1`（id `core`，name"宿主"，version 来自 `version.h`），
配置目录把它作为索引 0 暴露。于是"宿主设置"不是特例：和插件配置一样被枚举、编辑、
应用、存盘，设置页只写一套渲染逻辑。

| key | kind | 默认 | 副作用 |
| --- | --- | --- | --- |
| `logging.level` | ENUM `debug\|info\|warn\|error` | `info` | 立即 `spdlog::set_level`（host / plugin 两个 logger） |
| `debug.console` | BOOL | `1` | 立即开/关控制台 sink（文件日志不受影响） |
| `logging.file` | BOOL | `1` | 立即开/关文件 sink（`<dataPath>/logs/host.log`） |

**宿主设置里刻意没有 `output.*`**：输出目录/格式/剪贴板属于"保存"这件事，
以后由独立的 save 插件用 `shot.configurable` 声明，设置页会自动多出一节，宿主代码不参与。

## 6. 设置页插件（`plugins/settings_ui`）

- **单窗口、单实例**：插件 `init` 时创建隐藏窗口并起自己的 UI 线程；收到 `settings.open`
  显示；关闭（X / Esc）只隐藏，不销毁线程；`deinit` 时停线程、销毁窗口。
- **ImGui + Win32 + OpenGL3**：ImGui 静态编译进插件（vcpkg `imgui` 的
  `win32-binding` + `opengl3-binding`），宿主完全不依赖 UI 库、也拿不到 HWND。
- **中文字体**：优先加载系统中文 TTF/TTC（微软雅黑/黑体/宋体），失败时退回 ImGui 默认字体
  （只是显示效果降级，不影响功能）。
- **布局**：一页滚动，按提供者分节（`CollapsingHeader`，宿主 `core` 在最前）：
  1. 宿主设置；
  2. 插件管理（注册表表格：插件 / 版本 / 状态 / 启用复选框）；
  3. 每个可配置插件一节。
- **控件映射**（`ShotConfigDesc::kind`）：

| kind | 控件 | 值格式 |
| --- | --- | --- |
| BOOL | Checkbox | `"1"` / `"0"` |
| INT / FLOAT | Slider + Input（用 `min/max/step`，越界 clamp） | 十进制字符串 |
| ENUM | Combo（`options` 按 `|` 切） | 选项字符串 |
| STRING | InputText | 原样 |
| COLOR | ColorEdit4 + hex 输入 | `"#RRGGBB"` 或 `"R,G,B,A"`（统一成 `#RRGGBBAA` 存盘） |
| PATH | InputText + "浏览…" | 原样 |
| 未知 kind | 跳过并显示灰色"不支持的配置类型" | —— |

- **每项**：label + 悬停 `tooltip` + "恢复默认"小按钮（写 `default_value` 到本地副本）。
- **状态**：脏标记（没有脏项时"应用"置灰）；`set_value` 返回负值 → 该项标红"插件拒绝"；
  插件已被卸载 → 该节灰掉；窗口位置/大小存宿主配置的 `settings_ui.window`
  （`"x,y,w,h"`），下次显示时恢复。
- **插件管理**：`settings_ui.main` 自己与以后的入口插件行置灰；禁用行显示原因；
  勾选只改本地状态，应用时才 `set_enabled`。

## 7. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 单独的 reload 调用 / 消息 | 值已经通过 `set_config` 推给插件；再加一层"请重读"只会多一种状态不同步 |
| begin/commit 事务 | v1 用 `config.applied` 兜底批量通知；插件各自在 `set_config` 里做单值副作用 |
| 级联禁用 | 被依赖时直接拒绝更可解释；级联涉及"重新启用时按什么顺序恢复" |
| 在设置页安装/删除插件 | 只管理**已安装**插件的启停；安装属于另一个层（安装器） |
| 多个设置窗口 | 单实例足够；多实例会带来"两处同时改同一项"的冲突 |
| 把 `output.*` 塞进宿主设置 | 保存逻辑以后是 save 插件，配置跟着它自己走 |

## 8. 检查清单

- [ ] 新增配置项只改插件自己的 `kConfig[]`，没有改宿主代码？
- [ ] 配置值一律用字符串，没有跨边界传结构体/数字？
- [ ] 不认识的 key / 不认识的 kind 是安静跳过，而不是报错崩溃？
- [ ] 改了两个宿主服务之一？→ 同步改 `include/plugin_api.h`、`document/ABI_POLICY.md`
      登记表与布局锚点、`SHOT_ABI_MINOR`、各插件 `metadata.json` 的 `abi.minor`
- [ ] 涉及插件卸载？→ 插件 `deinit` 里停线程、退订总线、销毁窗口了吗？
- [ ] 改了主题/负载格式？→ 同步本文件与 `document/EVENT_BUS.md` 的主题表
