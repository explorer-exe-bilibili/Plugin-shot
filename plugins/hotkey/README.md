# hotkey —— 列表驱动的全局热键 provider（`shot.ui_provider`）

无主窗口架构下的"按键入口"。热键**不再写死在本插件里**：任何插件都可以向通用
列表 `hotkey_list` 贡献条目（`hotkey.main` 自己也贡献 `capture` / `region` /
`settings` 三条内置项），本插件只负责按列表顺序注册，按下时**只往事件总线发布
条目声明的事件**，不直接调用任何其它插件。

实际组合键仍然存在宿主配置 `hotkey.main/<id>` 里，设置页用
`options="hotkey"` 的"当前组合键 + 录入…"控件修改；改完应用即生效。

默认：`Ctrl+Alt+S` 截屏、`Ctrl+Alt+A` 区域截屏、`Ctrl+Alt+O` 打开设置。

## 1. 目录

```
plugins/hotkey/
├── CMakeLists.txt                  # 平台无关 provider + Windows / Linux X11 注册实现 + 降级
├── metadata.json                   # 插件清单（id hotkey.main，shot.ui_provider + shot.configurable）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── hotkey_binding.h            # 热键字符串 → 修饰键位 + 虚拟键码（不依赖 <windows.h>）
│   ├── hotkey_catalog.h            # hotkey_list 快照、内置贡献、动态配置值（HotkeyEntry / HotkeyCatalog）
│   ├── hotkey_config.h             # HotkeyConfig：enabled + 按快照动态生成的配置项
│   ├── hotkey_topics.h             # 内部头：平台实现共用的订阅/投递主题常量
│   ├── hotkey_window.h             # HotkeyWindow 公共接口（PIMPL，不含 Win32 / Xlib 类型）
│   ├── hotkey_window_impl.h        # 内部头（Windows）：HotkeyWindow::Impl、窗口消息与注册表
│   └── hotkey_window_x11_impl.h    # 内部头（Linux X11）：HotkeyWindow::Impl、Display 与抓取表
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 + ABI 胶水 + 语言表（kLangTables）
    ├── hotkey_binding.cpp          # 热键语法解析（纯函数，可单独测试）
    ├── hotkey_catalog.cpp          # 订阅 lists.changed、读快照、贡献内置项、旧配置迁移
    ├── hotkey_config.cpp           # 动态配置声明与值读写（转调 HotkeyCatalog）
    ├── hotkey_window.cpp           # Windows：生命周期 / 线程握手 / 事件订阅
    ├── hotkey_window_message.cpp   # Windows：窗口类 / 消息循环 / 暂停状态 / WM_HOTKEY 分发
    ├── hotkey_window_register.cpp  # Windows：按快照注册/撤销、冲突与非法值处理、发布事件
    ├── hotkey_window_x11.cpp       # Linux X11：生命周期 / 线程握手 / 请求分发 / Wayland 降级
    ├── hotkey_window_x11_loop.cpp  # Linux X11：事件循环（poll + self-pipe）与唤醒管道
    ├── hotkey_window_x11_register.cpp # Linux X11：keysym/keycode、锁修饰符、XGrabKey、发布事件
    └── hotkey_window_stub.cpp      # macOS 等无热键能力的平台：不注册全局热键
```

公共头一个类一个（`HotkeyCatalog` / `HotkeyConfig` / `HotkeyWindow`），
`hotkey_binding` 是一组纯函数；`hotkey_window_impl.h`（Windows）与
`hotkey_window_x11_impl.h`（Linux X11）都是**内部头**，只被本插件的 `.cpp` 包含，
`hotkey_topics.h` 是两边共用的主题常量。`.cpp` 按关注点拆分：解析 / 目录 / 配置 /
线程握手 / 窗口消息 / 注册（`STYLE.md` §6.8），两个平台各自维护自己的实现文件。

产物：`<构建树>/bin/plugins/hotkey.dll`（仓库内整体构建时是
`build/Windows/bin/plugins/hotkey.dll`）。根 `CMakeLists.txt` 通过
`plugins/*/CMakeLists.txt` 的 glob 自动纳入本工程，不需要手改根文件。

## 2. 构建

宿主仓库内构建（`shot_add_plugin()` 由根工程提供）：

```bash
cmake -S . -B build/Windows -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows --target hotkey
```

离线 SDK 独立构建：

```bash
cmake -S plugins/hotkey -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

依赖：

| 依赖 | 用途 | 说明 |
| --- | --- | --- |
| `user32` | `RegisterHotKey` / `UnregisterHotKey`、隐藏消息窗口与消息循环 | 仅 Windows 链接 |
| `libX11`（`<X11/Xlib.h>`） | `XGrabKey` / `XUngrabKey` / `XNextEvent` / 键盘映射查询 | 仅 Linux；Ubuntu 24.04 包 `libx11-dev` |
| `pkg-config` | configure 阶段用 `pkg_check_modules(x11)` 找 `libX11` | Linux 构建机需要（Ubuntu 包 `pkg-config`） |
| `nlohmann-json` | 解析 `hotkey_list` 快照与 `kLangTables` 语言表（header-only） | 根 vcpkg 清单已有 |

**平台**：

| 平台 | 实现 | 说明 |
| --- | --- | --- |
| Windows | `hotkey_window*.cpp` | `RegisterHotKey` + 隐藏消息窗口 |
| Linux | `hotkey_window_x11*.cpp` | X11 `XGrabKey`；运行期没有可用 `Display*`（纯 Wayland、无 `DISPLAY`）时降级：只 WARN、不注册，插件不失败 |
| macOS 等 | `hotkey_window_stub.cpp` | 没有全局热键 API，只发布列表与配置项 |

Linux 缺 `libX11` 时 configure 阶段直接 `FATAL_ERROR`（提示 `sudo apt install libx11-dev`），
而不是编出一个没有热键的插件。

### 2.1 Linux（X11）

**依赖**：`sudo apt install pkg-config libx11-dev`。`libx11-dev` 提供 `<X11/Xlib.h>`、
`<X11/keysym.h>` 与 `libX11.so`，链接由 `pkg_check_modules(x11)` 给出的绝对路径完成。

**注册**：在默认 screen 的 root window 上对每个热键调 `XGrabKey`，修饰符映射为
`Ctrl→ControlMask`、`Alt→Mod1Mask`、`Shift→ShiftMask`、`Win→Mod4Mask`；主键走
`hotkey_binding` 的解析结果 → `XKeysymToKeycode` 换成 keycode（`PrintScreen` 用
`XK_Print`）。抓取前先 `XUngrabKey` 同一组合，保证"抓取成功"一定是这一次注册的结果。

**锁修饰符**：X 服务器按**精确的修饰位组合**匹配被动抓取，所以 CapsLock / NumLock /
ScrollLock 开着时事件 state 会多出锁位，只抓 `Ctrl+Alt+S` 会"按了没反应"。实现为每个
组合额外抓一批锁变体（CapsLock 位 × NumLock 位 × ScrollLock 位的全部组合，通常 8 个），
锁位在 `XGetModifierMapping` 里运行时查（查不到按惯例回落 `Mod2`/`Mod5`）；事件匹配只
比较 `Ctrl/Alt/Shift/Win` 四位，锁位不参与比较。

**冲突**：注册期间临时安装 X 错误处理器，只捕获 `BadAccess`（组合键已被其它程序占用），
`XSync` 确认后立刻恢复原处理器（其它 X 错误原样转发）；冲突只 WARN 并跳过该条
（日志含 `注册失败` 与 `已跳过这一项`），已抓到的锁变体一并撤销，不影响其它热键、
不让插件失败。

**事件循环**：独立线程 + `poll(X 连接 fd + self-pipe)` 阻塞等待（空闲零 CPU），用
`XPending`/`XNextEvent` 取按键；`config.applied`、`hotkey.suspend` / `hotkey.resume`
都是"置 pending + self-pipe 唤醒"，`XGrabKey` / `XUngrabKey` 全部在热键线程上执行
（同一个 Display 只在这个线程里用）。

**Wayland**：Wayland 没有进程可用的全局热键 API。没有 `DISPLAY`（纯 Wayland）时 X11
实现在运行期降级，行为与 `hotkey_window_stub.cpp` 相同：仍然发布 `hotkey_list` 与配置
项，启动时 WARN：

> `[hotkey] 没有可用的 X11 Display（Wayland 不支持全局热键），请用 session_control 的 D-Bus 入口或桌面自定义快捷键`

不会让插件加载失败。**XWayland 注意**：`DISPLAY` 存在只说明有 XWayland，X11 抓取只
覆盖 Xwayland 的按键路径（X11 窗口获得焦点时），Wayland 原生前台程序下通常收不到；
需要全局热键时仍建议用 `session_control` 的 D-Bus 入口或桌面自定义快捷键。

## 3. 配置项（`shot.configurable`，动态生成）

| key | kind | 默认 | 发布的事件 | 含义 |
| --- | --- | --- | --- | --- |
| `enabled` | BOOL | `"1"` | —— | 总开关；关闭后撤销全部注册（线程与窗口保留，重新启用立即恢复） |
| `<条目 id>` | STRING + `options="hotkey"` | `<条目的 combo>` | 条目的 `event` / `data` | 每条 `hotkey_list` 条目的组合键 |

- `get_config_count()` = 1（enabled）+ 当前 `hotkey_list` 条目数；
  `get_config_desc()` 每次调用按当前快照现算，**不在 create 时缓存声明**。
  `get_config_count()` 枚举前还会主动重读一次快照：宿主在同一轮 tick 里先处理
  `lists.add`、再重建 `config_list`、最后才 flush `lists.changed`，不主动刷新的话
  新贡献的热键要晚一整轮才出现在设置页。列表变化后的**重注册**仍由
  `lists.changed` 驱动。
- 动态项固定为 `kind=STRING`、`key=<entry.id>`、`label=<entry.label>`（缺省
  `id`）、`default_value=<entry.combo>`、`options="hotkey"`；设置页因此仍然用
  现有的"按组合键录入"控件，不需要新的控件类型。
- 存盘键沿用旧机制：`hotkey.main/<entry.id>`。值一律字符串，落盘归宿主。
- `enabled` 仍是唯一静态项，label/tooltip 是文本 key，由 `kLangTables` 解析。
- 三个内置项的 label key：`config.capture.label` / `config.region.label` /
  `config.settings.label`（tooltip 同名 `.tooltip`）。

修改流程：设置页改值 → 宿主 `lists.update` 转调 `set_config` → 本插件把值写进
宿主配置 `hotkey.main/<id>` 并更新内存快照 → 热键线程按新值重注册 →
`lists.commit` 落盘后宿主发 `config.applied`，再幂等重注册一次。启动时宿主
`apply_stored` 直接 `set_config`（不发 `config.applied`），插件同样会重注册。

### 3.1 多语言（i18n）

> 语言表同时包含插件自身的 `plugin.name` / `plugin.description`：设置页的插件分区标题按
> `display(插件 id, "plugin.name")` 解析（查不到时回退成 key 本身）。

用户可见文案走"语言表 + key"：`src/plugin_entry.cpp` 的 `kLangTables` 用两张
`R"json(...)json"` 静态表（zh-CN / en-US）声明文案，`ui_init(host)` 里构造
`shot::I18n` 并发布到 `i18n_source_list`。设置页按
`display(owner, key)` 解析当前语言文本（本插件 owner 是 `hotkey.main`）。

规则：

- 两张表的 key 集必须**完全一致**，JSON 内不能有裸换行（单表远超 120 列，
  所以拆成相邻字符串字面量，编译期拼成一条 JSON）。
- 配置项文案 key：`config.<配置 key>.label` / `config.<配置 key>.tooltip`；
  插件自绘 UI 用 `ui.*`。
- 日志**不翻译**（`shot::Logger` 一律中文，e2e 按中文日志断言）；事件主题、
  配置 key、ENUM 技术值也不是用户文案。

## 4. 热键语法

```
[Ctrl+][Alt+][Shift+][Win+]<A-Z 或 0-9 或 F1..F12 或 PrintScreen>
```

- **大小写不敏感**；修饰键顺序任意，可省略（`F9`、`Shift+F9`、`Ctrl+Alt+S` 都合法）。
- 修饰键别名：`Ctrl` 也可写 `Control`，`Win` 也可写 `Windows`；重复写只算一次。
- 主键必须恰好一个：`A-Z`、`0-9`、`F1`..`F12`、`PrintScreen`；`F13` 及以上不支持。
- `PrintScreen`（Win32 `VK_SNAPSHOT`）也接受 `Print Screen` / `PrtSc` / `PrtScn` /
  `PrtScr` 拼写。**注意 Win11 的系统级拦截**：默认开启的「设置 → 辅助功能 →
  键盘 → 使用 Print Screen 键打开截图工具」会先把裸按 PrintScreen 交给系统
  截图工具（`RegisterHotKey` 成功但收不到 `WM_HOTKEY`）；带修饰键的组合
  （如 `Ctrl+PrintScreen`）不受影响。注册裸 PrintScreen 成功时会 WARN 提示。
- 修饰键之间/末尾出现多余的 `+`、空 token、未知键位都按**非法**处理。
- 语法允许**没有修饰键**（如 `F9`）；但裸字母/数字会拦截正常输入，建议至少带
  一个修饰键。这是用户配置责任，插件不擅自改用户的值。

解析实现是 `hotkey_binding.cpp` 的纯函数 `parse_binding()`，不依赖 Win32 API。

## 5. `hotkey_list`：列表驱动的数据源

`hotkey.main` 在 create 阶段向通用列表贡献三条内置项（owner 必须是贡献插件
自己的 id；宿主按 `(list, owner, id)` 覆盖）：

```json
{"list":"hotkey_list","owner":"hotkey.main","items":[
  {"id":"capture","combo":"Ctrl+Alt+S","event":"capture.request","data":"","label":"config.capture.label"},
  {"id":"region","combo":"Ctrl+Alt+A","event":"capture.request","data":"{\"mode\":\"region\"}","label":"config.region.label"},
  {"id":"settings","combo":"Ctrl+Alt+O","event":"settings.open","data":"","label":"config.settings.label"}
]}
```

条目 schema（完整登记见 `document/LISTS.md` §5.5）：

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `id` | string | ✅ | 稳定标识，在 owner 内唯一；同时用作配置键 `hotkey.main/<id>`，所以不同 owner 之间也应避免同名 |
| `combo` | string | ✅ | 默认组合键；`hotkey.main/<id>` 不存在或非法时回落到它 |
| `event` | string | ✅ | 按下时要发布的事件主题；为空时该条被 provider 忽略（WARN） |
| `data` | string | 可选 | 事件负载，空串 = 空负载；对象/数组会被序列化成 JSON 文本 |
| `label` | string | 可选 | 设置页显示文本 key；缺省用 `id`。注意设置页按 `display("hotkey.main", label)` 解析（`config_list` 的 owner 是本 provider），贡献方自己的 key 需要提供 `target:"hotkey.main"` 的 i18n 译文，或直接写中文字面量 |

写入规则：

- **只能由 owner 自己贡献**（`lists.add` 的 `owner` 必须是贡献插件自己的 id）；
  宿主按 `(list, owner, id)` upsert，插件卸载时宿主自动清掉它的条目；
- **provider 只读**：`hotkey.main` 订阅 `lists.changed`、读 `shot.host.plugin_lists`
  的 `hotkey_list` 快照，从不修改别人的条目；
- 每次列表变化后 provider 重新读全量快照并**整批重注册**；注册顺序 = 列表顺序
  （宿主按 `order` 升序，缺省 1000），同一组合键冲突时先到者生效；
- `hotkey.main` 自己的三条内置项在 provider 内也有本地兜底：老宿主没有列表服务
  或事件还没被处理时，三条默认热键照常注册。

给功能加热键 = 贡献一条 `hotkey_list` 条目（详见根 `AGENTS.md` §5.3），不要再在
插件里自己调用平台的注册 API（`RegisterHotKey` / `XGrabKey`）。

## 6. 注册流程与冲突处理

热键线程每次重注册都按下面的顺序处理快照里的每一条：

1. 实际值 = 宿主配置 `hotkey.main/<id>`；键不存在时用条目的 `combo`；
2. **显式清空**（配置存在但值为空串）= 这项不注册（沿用设置页"清空 = 不设热键"
   的约定），只记 INFO；
3. 配置值非空但语法非法 → WARN 后回落到条目的 `combo`；`combo` 也非法才跳过；
4. 同一批注册里已有相同"修饰键 + 主键" → WARN 跳过（**先到者注册**，按列表顺序），
   不再依赖系统错误码（Windows `1409` / X11 `BadAccess`），日志能区分"列表自己冲突"
   和"被其它程序占用"；
5. 平台注册成功记 INFO（含 id / 组合键 / 事件，负载非空时打出负载）；失败记 WARN
   （Windows 含系统错误码、X11 含 `BadAccess` 提示），跳过该项，不影响其它键、
   不让插件失败。

非法值/冲突/被占用都只影响当前条目：热键插件少一个键仍然工作，宿主不该因为
一条坏数据丢掉整个插件。

## 7. 事件（只发事件，不直接调用任何插件）

内置三条：

| 条目 id | 主题 | 负载 | 说明 |
| --- | --- | --- | --- |
| `capture` | `capture.request` | 空 | 与托盘「截屏」、`--capture` 走同一条采集流水线 |
| `region` | `capture.request` | `{"mode":"region"}` | "请开始框选"请求：流水线忽略它（不采集），由 region_selector 弹覆盖层（见 `document/CAPTURE.md` §3.1） |
| `settings` | `settings.open` | 空 | 与托盘「打开设置」相同 |

贡献条目按自己的 `event` / `data` 发布（例如 `long_capture` 的
`long_capture.start`）。热键插件**不认识、也不直接调用**任何其它插件：按热键只
`publish` 事件，订阅方是谁、有没有人订阅都不影响它。发布失败（宿主没有事件总线、
队列满）只 WARN，不阻塞、不重试。

订阅（回调都跑在宿主分发线程上，只置 pending + 唤醒热键线程：
Windows 用 `PostMessage`，X11 用 self-pipe）：

| 主题 | 负载 | 处理 |
| --- | --- | --- |
| `lists.changed` | `{"list":"hotkey_list"}` | 重读快照并刷新动态配置值，然后请求整批重注册（订阅在 create 建立） |
| `config.applied` | 空 | 请求重注册（设置页应用/落盘后） |
| `hotkey.suspend` | 空 | 设置页开始录入组合键：撤销全部注册（暂停）；暂停期间 `set_config` / `config.applied` 只撤销、不注册 |
| `hotkey.resume` | 空 | 设置页结束录入：按**目录当前快照**恢复注册（不是复用旧注册表）；重复的 `resume` 幂等 |

`hotkey.suspend` / `hotkey.resume` 是设置页在"按组合键录入"期间发的：录入时用户
按下的可能正是当前生效的组合键，不撤销的话会先触发真实动作。本插件只提供
"撤/按快照恢复"两个动作，不认识设置页的 UI 细节。

## 8. 生命周期与线程模型

| 时机 | 插件做什么 |
| --- | --- |
| `create(index, host)` | 构造目录 → 迁移旧配置 → 发布三条 `hotkey_list` 内置项 → 订阅 `lists.changed` → 读一次快照 |
| `init(self, host)` | 构造 `shot::I18n` 并发布语言表 → 起热键线程 → 建隐藏消息窗口（Windows）/ 打开 X11 连接（Linux）→ 按快照注册热键 |
| `lists.changed` | 宿主 main 线程重读快照；热键线程整批撤销 + 按新快照注册 |
| 按键到达 | Windows：`WM_HOTKEY` 按注册 id 找条目；X11：按 keycode + 修饰位找条目；随后发布 `event`（负载 `data`） |
| `config.applied` / `set_config` | 置"需要重注册"标记 + 唤醒热键线程；热键线程重新解析并整批注册 |
| `hotkey.suspend` / `hotkey.resume` | 置"暂停/恢复"标记 + 唤醒热键线程；注册仍然只在热键线程上 |
| `post_to_plugin`（`host.shutdown`） | 直接走 `stop()`（幂等；deinit 里的 stop 变成空操作） |
| `deinit(self)` | 拒绝新投递 → 线程撤销全部热键 → 退出事件循环 → `join` → 退订事件总线 |
| `destroy(self)` | 再次调用 stop / catalog.stop（幂等），然后释放状态 |

线程只有两个来源：宿主线程（create/init/deinit/post、事件总线回调）与插件热键
线程。**平台注册/撤销（Windows `RegisterHotKey` / `UnregisterHotKey`，X11
`XGrabKey` / `XUngrabKey`）全部在热键线程上执行**（撤销要求同线程，也避免与事件
分发竞争）；`stop()` 返回时线程已 `join`。Windows 的窗口类在 `deinit` / 线程退出时
**显式注销**（`UnregisterClassW`）：窗口类属于进程、不随 DLL 卸载消失，不注销的话
禁用→启用会踩悬空 `WndProc`（`plugins/tray` 实测过 `0xC000041D`）；X11 连接在
线程退出时 `XCloseDisplay`，抓取随连接关闭自动释放，并且退出前也显式
`XUngrabKey` 一次。

## 9. 旧配置迁移

列表驱动之前的三个静态配置键是：

| 旧键 | 新键（条目 id） |
| --- | --- |
| `hotkey.main/capture_hotkey` | `hotkey.main/capture` |
| `hotkey.main/region_hotkey` | `hotkey.main/region` |
| `hotkey.main/settings_hotkey` | `hotkey.main/settings` |

create 时对每一对做"新键不存在、旧键存在 → 把旧值写进新键"的迁移，并在日志里
打一条 INFO。新键存在时（哪怕值是用户清空后的空串）绝不覆盖；禁用→启用会重复
create，但第二次新键已经存在，所以迁移是幂等的，不需要额外的"已迁移"标记。
旧键保留不删，方便回滚；它们不再被读取。

## 10. 平台限制与降级

- **Windows**：`RegisterHotKey` + 隐藏消息窗口；全局热键是系统级资源，同组合键
  只能注册一次，被其它程序占用时只能 WARN 跳过。
- **Linux / X11**：`XGrabKey` 在 root window 上做被动抓取；遇到 `BadAccess`（被其它
  程序占用）只 WARN 跳过。**XWayland 限制**见 §2.1。
- **Wayland（无 `DISPLAY`）**：provider 仍然贡献 `hotkey_list`、暴露动态配置项，
  但 X11 实现在运行期拿不到 `Display*`，按 `hotkey_window_stub.cpp` 同样的方式降级
  （不注册任何热键，启动时 WARN 指向 `session_control` 的 D-Bus 入口 / 桌面自定义
  快捷键）；插件不失败。
- **X11 的自动重复**：没有 `MOD_NOREPEAT` 等价物，按住不放会按服务器的自动重复
  连续触发；抑制需要在 `KeyRelease` 里维护"按下中"集合，而丢失一次 release（别的
  client 抢走键盘抓取）会让该热键永久失效，所以这里按"按住会重复触发"处理。
- 锁屏 / 安全桌面 / 更高权限的前台程序按系统策略决定是否投递热键，插件无法绕过。
- 无修饰键的字母/数字会拦截正常输入（语法允许，这是用户配置的责任）。

| 情况 | 行为 |
| --- | --- |
| 宿主没有事件总线（旧版宿主） | 记 WARN；内置三条仍按默认值注册，只是发布事件会失败；列表变化/配置变化只在重新加载时生效 |
| 宿主没有列表服务 | 记 WARN；使用 `hotkey.main` 的本地内置三条，热键照常注册 |
| `hotkey_list` 某条缺 `event` / 缺 `id` / 重复 id | provider WARN 并跳过该条，其它条照常 |
| 配置值非法 | WARN 后回落到条目的 `combo`，再非法才跳过 |
| 配置值空串 | INFO 后跳过该项（设置页"清空"语义） |
| 同组合键冲突 | 列表顺序先到者注册，后者 WARN 跳过 |
| `enabled = "0"` | 撤销全部注册并记 INFO；线程与窗口保留，改成 `"1"` 应用后立即重新注册 |
| 设置页录入组合键 | `hotkey.suspend` 撤销全部注册 / `hotkey.resume` 按快照恢复，两侧都记 INFO |
| Windows 窗口类注册/清理失败 | `init` 返回失败，宿主按"这个 UI 插件初始化失败"处理，其余功能不受影响 |
| Linux 没有 `DISPLAY`（纯 Wayland） | 按 stub 行为降级：只 WARN、仍提供列表与配置项，`init` 返回成功 |
| Linux 唤醒管道建不出来 | `init` 返回失败（没有管道时 `stop()` 无法唤醒热键线程） |

## 11. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 自己写死热键列表 | 列表驱动后功能插件用 `lists.add` 贡献条目，加动作不用改本插件 |
| 在插件里实现热键录制 UI | 录入控件属于设置页（跨 DLL 不能互调）；本插件只提供 `hotkey.suspend` / `hotkey.resume` 钩子 |
| 为热键数据新增 ABI 结构体/宿主服务 | 热键只是"列表条目 + 配置字符串 + 事件"，属于数据形状，按 `document/LISTS.md` 的理念不加 ABI |
| 按住连发（自动重复） | Windows 用 `MOD_NOREPEAT`；X11 没有等价物，按住会重复触发（见 §10） |
| 自动占用/替换冲突热键 | 冲突只 warn 跳过，不静默换键——用户必须知道自己的配置没生效 |
| 轮询键盘状态 | 平台注册由系统/服务器投递按键事件（Windows `WM_HOTKEY` / X11 被动抓取），轮询既费 CPU 又不可靠 |
| 自己写配置文件 | 配置声明交给 `shot.configurable`，落盘与启动回灌归宿主 |

## 12. 检查清单

- [ ] 新增动作只贡献 `hotkey_list` 条目，没有在插件里自己 `RegisterHotKey`？
- [ ] `id` 稳定且在贡献插件内唯一；不同 owner 之间没有同名 id（配置键会冲突）？
- [ ] `combo` / `event` / `data` 是合法字符串，`label` 是 i18n 文本 key（或缺省）？
- [ ] 配置键小写 + 下划线、默认值是字符串、不认识的 key 返回负值且不改变状态？
- [ ] 热键解析失败/被占用是 warn 跳过，不影响其它键、不让 `init` 失败？
- [ ] `RegisterHotKey` / `XGrabKey` 与其撤销都在热键线程上，`deinit` 返回前已撤销并退订？
- [ ] 列表变化后整批重注册，暂停期间只撤销、`resume` 后按**当前快照**恢复？
- [ ] Win32 / Xlib 类型只在内部头与 `.cpp` 里，公共头不含 `<windows.h>` / `<X11/Xlib.h>`？
- [ ] 异常没有越过 `extern "C"`、`WndProc` 与热键线程入口？
- [ ] 没有 X11 `Display*`（纯 Wayland）或平台无热键能力时仍然发布列表与配置项，且只 WARN 不失败？
