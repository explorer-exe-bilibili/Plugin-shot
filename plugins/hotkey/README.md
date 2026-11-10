# hotkey —— Windows 全局热键插件（`shot.ui_provider`）

无主窗口架构下的"按键入口"：插件自己开线程、自己建隐藏消息窗口，用 `RegisterHotKey`
注册全局热键，按下后**只往事件总线发一条事件**（截屏 / 区域截屏 / 打开设置），不直接
调用任何其它插件。热键字符串与总开关都是 `shot.configurable` 配置项，设置页改完应用
即生效。

默认：`Ctrl+Alt+S` 截屏、`Ctrl+Alt+A` 区域截屏、`Ctrl+Alt+O` 打开设置。

三个热键项的描述符带 `options="hotkey"`：设置页据此把它们渲染成"显示当前组合键 + 录入…"
的控件（按一次组合键即可录入，不用手输字符串，见 `document/SETTINGS.md` §6 与
`plugins/settings_ui/README.md` §4.1）。值语法、解析与存值都不受这个标记影响。

## 1. 目录

```
plugins/hotkey/
├── CMakeLists.txt                  # shot_add_plugin + user32；非 Windows 直接跳过
├── metadata.json                   # 插件清单（id hotkey.main，接口 shot.ui_provider + shot.configurable）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── hotkey_binding.h            # 热键字符串 → 修饰键位 + 虚拟键码（不依赖 <windows.h>）
│   ├── hotkey_config.h             # HotkeyConfig 公共接口（四个配置项）
│   ├── hotkey_window.h             # HotkeyWindow 公共接口（PIMPL，不含 Win32 类型）
│   └── hotkey_window_impl.h        # 内部头：HotkeyWindow::Impl、窗口消息与注册表类型
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 + ABI 胶水（状态/回调/能力目录）
    ├── hotkey_binding.cpp          # 热键语法解析（纯函数，可单独测试）
    ├── hotkey_config.cpp           # 配置声明、字符串值读写（跨线程安全）
    ├── hotkey_window.cpp           # 生命周期 / 线程握手 / 事件订阅（start / stop / post）
    └── hotkey_window_message.cpp   # 窗口类 / 消息循环 / RegisterHotKey / WM_HOTKEY 发布事件
```

公共头一个类一个（`HotkeyConfig` / `HotkeyWindow`）、`hotkey_binding` 是一组纯函数；
`hotkey_window_impl.h` 是**内部头**：只被本插件的 `.cpp` 包含，放 PIMPL 定义与 Win32 类型。
`.cpp` 按关注点拆分：解析 / 配置 / 线程握手 / 窗口与注册，每个文件都能一句话说清职责
（`STYLE.md` §6.8）。

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
| `user32` | `RegisterHotKey` / `UnregisterHotKey`、隐藏消息窗口与消息循环 | Windows 系统库 |

插件不链接 `nlohmann-json`（事件负载是两个固定 JSON 字面量）也不链接 `spdlog`
（日志走宿主的 `log` 服务，见 `STYLE.md` §6.4）。

**仅 Windows**：非 Windows 平台本工程直接跳过（`SHOT_PLUGIN_SKIPPED`，根工程打印
"当前平台不支持"）。移植点写在 CMakeLists 与窗口实现里（`// TODO:`）。

## 3. 配置项（`shot.configurable`）

| key | kind | 默认 | 发布的事件 | 含义 |
| --- | --- | --- | --- | --- |
| `enabled` | BOOL | `"1"` | —— | 总开关；关闭后撤销全部注册（线程与窗口保留，重新启用立即恢复） |
| `capture_hotkey` | STRING | `"Ctrl+Alt+S"` | `capture.request`（空负载） | 截屏 |
| `region_hotkey` | STRING | `"Ctrl+Alt+A"` | `capture.request`，负载 `{"mode":"region"}` | 区域截屏 |
| `settings_hotkey` | STRING | `"Ctrl+Alt+O"` | `settings.open`（空负载） | 打开设置 |

值一律用字符串表达；存盘键是 `hotkey.main/<key>`（宿主负责落盘与启动回灌，见
`document/SETTINGS.md`）。不认识的 key 返回负值且不改变状态。

三个热键项的 `ShotConfigDesc::options` 写 `"hotkey"`：设置页据此把 STRING 渲染成
"当前组合键 + 录入…"控件（`document/LISTS.md` §5.1、`document/SETTINGS.md` §6）。
`options` 只是给设置页的交互提示，值语法 / 解析 / 存值完全不变。

修改流程：设置页改值 → 宿主 `lists.update` 转调 `set_config` → 热键线程重新解析并注册
→ `lists.commit` 落盘后宿主发 `config.applied`，再幂等重注册一次。启动时宿主
`apply_stored` 直接 `set_config`（不发 `config.applied`），插件同样会重注册，所以已存
热键在启动阶段就生效。

## 4. 热键语法

```
[Ctrl+][Alt+][Shift+][Win+]<A-Z 或 0-9 或 F1..F12 或 PrintScreen>
```

- **大小写不敏感**；修饰键顺序任意，可省略（`F9`、`Shift+F9`、`Ctrl+Alt+S` 都合法）。
- 修饰键别名：`Ctrl` 也可写 `Control`，`Win` 也可写 `Windows`；重复写只算一次。
- 主键必须恰好一个：`A-Z`、`0-9`、`F1`..`F12`、`PrintScreen`；`F13` 及以上不在支持范围。
- `PrintScreen`（Win32 `VK_SNAPSHOT`）也接受 `Print Screen` / `PrtSc` / `PrtScn` / `PrtScr`
  拼写，统一规范化成 `PrintScreen`。**注意 Win11 的系统级拦截**：默认开启的
  「设置 → 辅助功能 → 键盘 → 使用 Print Screen 键打开截图工具」会先把裸按 PrintScreen
  交给系统截图工具（`RegisterHotKey` 成功但收不到 `WM_HOTKEY`）；带修饰键的组合
  （如 `Ctrl+PrintScreen`）不受影响。要用裸 PrintScreen 请关掉该 Windows 设置，
  插件在注册裸 PrintScreen 成功时会打一条 WARN 说明这一点。
- 修饰键之间/末尾出现多余的 `+`、空 token、未知键位都按**非法**处理。
- 非法或**被其它程序占用**的热键只 warn 跳过该项，**其它热键照常注册**，插件启动不失败
  （占用时日志带系统错误码，`1409` = `ERROR_HOTKEY_ALREADY_REGISTERED`）。
- 语法允许**没有修饰键**（如 `F9`）；但裸字母/数字会拦截正常输入，建议至少带一个修饰键。
  这是用户配置责任，插件不擅自改用户的值。

解析实现是在 `hotkey_binding.cpp` 里的纯函数 `parse_binding()`，不依赖 Win32 API，
可以离线用假字符串验证。

## 5. 事件（只发事件，不直接调用任何插件）

| 动作 | 主题 | 负载 | 说明 |
| --- | --- | --- | --- |
| 截屏 | `capture.request` | 空 | 与托盘「截屏」、`--capture` 走同一条采集流水线 |
| 区域截屏 | `capture.request` | `{"mode":"region"}` | 只带模式字段，是"请开始框选"请求：流水线忽略它（不采集），由 region_selector 弹覆盖层，选完再发带 `region` 的请求（见 `document/CAPTURE.md` §3.1） |
| 打开设置 | `settings.open` | 空 | 与托盘「打开设置」相同 |

热键插件**不认识、也不直接调用**任何其它插件：按热键只 `publish` 事件，订阅方是谁、
有没有人订阅都不影响它。这样换掉采集插件/设置页、或同时装托盘与热键，都不需要改本插件。
发布失败（宿主没有事件总线、队列满）只 warn，不阻塞、不重试。

区域截屏的选区交互由采集侧/区域选择插件负责（见 `document/CAPTURE.md` 与
`plugins/region_selector/`）；热键只负责在 `capture.request` 上带 `{"mode":"region"}`
字段，不假设区域怎么产生、由谁完成选择。

订阅（回调都跑在宿主分发线程上，只置 pending + `PostMessage` 唤醒热键线程，不做解析、
不碰 Win32）：

| 主题 | 负载 | 处理 |
| --- | --- | --- |
| `config.applied` | 空 | 请求重注册：先 `UnregisterHotKey` 全部，再按当前配置逐项注册 |
| `hotkey.suspend` | 空 | 设置页开始录入组合键：撤销全部注册（暂停）；暂停期间 `set_config` / `config.applied` 只撤销、不注册 |
| `hotkey.resume` | 空 | 设置页结束录入：按**当前配置**恢复注册（不是复用旧注册表）；重复的 `resume` 是幂等的 |

`hotkey.suspend` / `hotkey.resume` 是**设置页**（`plugins/settings_ui`）在"按组合键录入"
期间发的：录入时用户按下的可能正是当前生效的组合键，不撤销的话会先触发真实动作（比如
立刻截一张图）。本插件只提供"撤/按配置恢复"这两个动作，不认识设置页的 UI 细节。

## 6. 生命周期与线程模型

| 时机 | 插件做什么 |
| --- | --- |
| `init(self, host)` | 订阅 `config.applied` → 起热键线程 → 建隐藏消息窗口 → 按当前配置注册热键 → 返回 `0` 时窗口/线程已就绪 |
| `WM_HOTKEY` | 按热键 id 找到动作，发布对应事件（热键线程上执行） |
| `config.applied` / `set_config` | 置"需要重注册"标记 + `PostMessage`；热键线程**先 `UnregisterHotKey` 全部、再逐项注册** |
| `hotkey.suspend` / `hotkey.resume` | 置"暂停/恢复"标记 + `PostMessage`；热键线程撤销全部注册 / 按当前配置重新注册（`RegisterHotKey` 仍然只在热键线程上） |
| `post_to_plugin`（`host.shutdown`） | 直接走 `stop()`（幂等；deinit 里的 stop 变成空操作） |
| `deinit(self)` | 拒绝新投递 → 线程撤销全部热键 → 退出消息循环 → `join` → 退订事件总线 |
| 宿主退出 / 热卸载 | 先投 `host.shutdown` 再 `deinit`，两条路径都收敛到同一个 `stop()` |

线程只有两个来源：宿主线程（`init` / `deinit` / `post_to_plugin` / 事件总线回调）与插件
自己的热键线程。**`RegisterHotKey` / `UnregisterHotKey` 全部在热键线程上执行**（撤销要求
同线程，也避免与消息分发竞争）；`stop()` 返回时线程已 `join`，不会留下后台线程。
窗口类在 `deinit` / 线程退出时**显式注销**（`UnregisterClassW`）：窗口类属于进程、不随
DLL 卸载消失，不注销的话禁用→启用会踩悬空 `WndProc`（`plugins/tray` 实测过
`0xC000041D`）。遇到进程里残留的同名类时先清掉再注册，清不掉就 `init` 失败，绝不沿用
未知窗口过程。

## 7. 平台限制

- **仅 Windows**：`RegisterHotKey` + 隐藏消息窗口；macOS（`RegisterEventHotKey`）与
  Linux（`XGrabKey` / Wayland portal）没有实现，非 Windows 平台整个工程被 CMake 跳过。
  移植点：`CMakeLists.txt` 的 TODO、`hotkey_binding.h` 里 VK 数值的平台映射说明。
- **全局热键是系统级资源**：同一个组合键在系统里只能被注册一次；被其它程序占用时本插件
  只能 warn 跳过，用户需要换一个组合键。
- 锁屏 / 安全桌面 / 更高权限的前台程序按系统策略决定是否投递热键，插件无法绕过。
- 无修饰键的字母/数字会拦截正常输入（语法允许，这是用户配置的责任）。

## 8. 降级行为

| 情况 | 行为 |
| --- | --- |
| 宿主没有事件总线（旧版宿主） | 记 WARN；热键仍按配置注册，但发布事件会失败（等于没有效果），重新加载后重试 |
| 某个热键字符串非法 | 记 WARN 说明原因并跳过该项，其它热键照常注册，插件启动不失败 |
| 某个热键被其它程序占用 | 记 WARN（含系统错误码与提示）并跳过该项，不影响其它键 |
| `enabled = "0"` | 撤销全部注册并记 INFO；线程与窗口保留，改成 `"1"` 应用后立即重新注册 |
| 设置页录入组合键（`hotkey.suspend` / `resume`） | 撤销全部注册 / 按当前配置恢复，两侧都记 INFO；暂停期间 `set_config` / `config.applied` 只撤销、不注册 |
| 设置页未加载 / 被禁用 | 没有 `hotkey.suspend` / `resume` 的发布者：热键照常注册；录入控件也不会出现（它属于设置页） |
| 注册失败导致一个热键都没有 | 插件仍正常运行（窗口/线程在），用户改配置应用即可恢复，不用重启宿主 |
| `apply_stored` 在启动时推送已存配置 | `set_config` 回调请求重注册（该阶段没有 `config.applied`，不能只等事件） |
| 窗口类注册/清理失败 | `init` 返回失败，宿主按"这个 UI 插件初始化失败"处理，其余功能不受影响 |
| `start` 时线程资源耗尽 | `catch (...)` 兜底，撤回订阅并返回失败，异常不越过 DLL 边界 |

## 9. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 直接调用采集/设置插件 | 跨 DLL 只能走宿主；只发事件，换插件不用改热键（`EVENT_BUS.md` §1） |
| 为热键新增 ABI 结构体/宿主服务 | 热键只是"配置字符串 + 事件"，属于数据形状，按 `document/LISTS.md` 的理念不加 ABI |
| 按住连发（自动重复） | 用 `MOD_NOREPEAT`，热键语义是"按一下执行一个动作"；长按连续截屏不是用户想要的 |
| 自动占用/替换冲突热键 | 冲突只 warn 跳过，不静默换键——用户必须知道自己的配置没生效 |
| 轮询键盘状态 | `RegisterHotKey` 由系统投递 `WM_HOTKEY`，轮询既费 CPU 又不可靠 |
| 自己写配置文件 | 配置声明交给 `shot.configurable`，落盘与启动回灌归宿主（`document/SETTINGS.md` §2） |
| 在插件里实现热键录制 UI | 录入控件属于设置页（跨 DLL 不能互调）；本插件只提供 `hotkey.suspend` / `hotkey.resume` 钩子，UI 与文本规范化都在设置页（`plugins/settings_ui/src/settings_hotkey.cpp`） |

## 10. 检查清单

- [ ] 新增动作只加"热键 → 事件"映射，没有直接调用其它插件？
- [ ] 配置键小写 + 下划线、默认值是字符串、不认识的 key 返回负值且不改变状态？
- [ ] 热键解析失败/被占用是 warn 跳过，不影响其它键、不让 `init` 失败？
- [ ] `RegisterHotKey` / `UnregisterHotKey` 都在热键线程上，`deinit` 返回前已撤销并退订？
- [ ] Win32 类型只在内部头与 `.cpp` 里，公共头不含 `<windows.h>`？
- [ ] 异常没有越过 `extern "C"` 与 `WndProc`？
- [ ] 事件主题与负载符合 `document/EVENT_BUS.md` §4，且不假设订阅方一定存在？
- [ ] 窗口类在线程退出前注销，禁用→启用可以反复加载？
- [ ] 没有把配置值自己写进文件、没有新增 ABI 依赖？
- [ ] `hotkey.suspend` / `hotkey.resume` 的订阅在 `start` 建立、`deinit` 退订，撤销/注册都在热键线程上？
- [ ] 暂停期间 `set_config` / `config.applied` 只撤销、不注册，`resume` 后按**当前配置**恢复？
