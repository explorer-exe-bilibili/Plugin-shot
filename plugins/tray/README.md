# tray —— Windows 托盘插件（`shot.ui_provider`）

无主窗口架构下的托盘入口：图标、隐藏消息窗口和托盘线程全部由插件自己创建和管理，
宿主只调 `init` / `deinit` / `post_to_plugin`，既不知道插件开了几个线程，也拿不到 `HWND`。

**托盘是菜单的渲染器，不是命令的所有者**：右键菜单的内容完全来自宿主列表服务里的
`tray_command_list`——宿主预置 core 三项（截屏 / 打开设置 / 退出），插件运行期还可以用
`lists.add` 往这个列表里加命令；点击任何条目都只把条目声明的事件发到事件总线。
本插件不认识、也不直接调用任何其它插件（列表协议见 `document/LISTS.md`，
事件与线程模型见 `document/EVENT_BUS.md`、`document/SETTINGS.md`）。

## 1. 目录

```
plugins/tray/
├── CMakeLists.txt                  # shot_add_plugin + nlohmann-json / user32 / shell32 依赖
├── metadata.json                   # 插件清单（id tray.main，接口 shot.ui_provider + shot.configurable）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── tray_config.h               # TrayConfig 公共接口（两个开关配置）
│   ├── tray_icon.h                 # TrayIcon 公共接口（PIMPL，不含 Win32 类型）
│   ├── tray_icon_impl.h            # 内部头：TrayIcon::Impl、事件主题与托盘消息常量
│   └── tray_menu.h                 # TrayMenu / TrayCommandItem：tray_command_list 的数据模型
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + kEntries + ABI 胶水
    ├── tray_config.cpp             # shot.configurable：配置声明 / 值解析 / 线程安全读写
    ├── tray_icon.cpp               # 生命周期 / 线程握手 / 事件投递（start / stop / post）
    ├── tray_icon_window.cpp        # 窗口线程 / 消息循环 / 托盘图标 / 事件回调
    ├── tray_icon_menu.cpp          # 右键菜单：HMENU 重建 / TrackPopupMenu / 点击发布事件
    ├── tray_icon_notify.cpp        # 事件出队 / 气泡通知 / lists.changed 分发
    └── tray_menu.cpp               # tray_command_list JSON 解析 / 校验 / 排序 / 本地兜底
```

公共头一个类一个（`TrayIcon` / `TrayConfig` / `TrayMenu`），`tray_icon_impl.h` 是**内部头**：
只被本插件的 `.cpp` 包含，放 PIMPL 定义与 Win32 类型。`.cpp` 按关注点拆分：配置 /
生命周期 / 窗口与托盘图标 / 菜单渲染 / 通知 / 菜单数据，每个文件都能一句话说清职责
（`STYLE.md` §6.8）。菜单数据模型（`tray_menu.cpp`）不依赖 Win32，用假 JSON 就能离线
验证解析、排序与兜底。

产物：`<构建树>/bin/plugins/tray.dll`（仓库内整体构建时是
`build/Windows/bin/plugins/tray.dll`）。根 `CMakeLists.txt` 通过 `plugins/*/CMakeLists.txt`
的 glob 自动纳入本工程，不需要手改根文件。

## 2. 构建

宿主仓库内构建（`shot_add_plugin()` 由根工程提供）：

```bash
cmake -S . -B build/Windows -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows --target tray
```

离线 SDK 独立构建：

```bash
cmake -S plugins/tray -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

依赖（都由 vcpkg 清单模式提供，插件不链接 spdlog）：

| 依赖 | 用途 | 说明 |
| --- | --- | --- |
| `nlohmann-json` | 解析 `tray_command_list` 与 `capture.done` / `capture.failed` 的 JSON 负载 | header-only，不引入运行库 |
| `user32` | 窗口类、消息循环、右键菜单 | 系统库 |
| `shell32` | `Shell_NotifyIconW` / `Shell_NotifyIconGetRect` | 系统库 |

**仅 Windows**：非 Windows 平台本工程直接跳过（`SHOT_PLUGIN_SKIPPED`，根工程打印
"当前平台不支持"），因为托盘层是 Win32 实现。

## 3. 交互

| 用户动作 | 插件行为 |
| --- | --- |
| 左键单击图标 | 列表里有 `"default":true` 条目就发布它的事件；没有则发布 `capture.request`（core 预置项都没有 `default`，所以默认仍是截屏） |
| 右键单击图标 | 弹出数据驱动菜单：`tray_command_list` 快照按 `order` 升序渲染，`separator:true` 渲染成分隔线 |
| 点击菜单项 | 发布该条目声明的 `event`（负载 = `data`），不直接调用任何插件 |
| 气泡通知（`capture.done`） | 显示「已保存到 `<path>`」，受 `notify` 控制 |
| 气泡通知（`capture.failed`） | 显示「截屏失败：`<reason>`」，受 `notify_on_error` 控制 |

菜单用 `TrackPopupMenu(TPM_RETURNCMD | TPM_NONOTIFY)` 直接拿返回值，不经过 `WM_COMMAND`；
弹菜单前先 `SetForegroundWindow`、关掉后补一条 `WM_NULL`，避免"点菜单外面"的那次鼠标
抬起被当成左键点击、又执行一次默认命令（托盘菜单的经典坑，见 `tray_icon_menu.cpp` 注释）。

## 4. 菜单数据（`tray_command_list`）

菜单没有一行硬编码命令：托盘用 `shot::PluginLists(host).json("tray_command_list")` 取
宿主聚合好的只读 JSON 快照（两段式读取由辅助头封装），解析成 `TrayCommandItem` 后交给
`HMENU` 渲染。core 三项（截屏 / 打开设置 / 退出）也由宿主预置在同一张列表里，托盘不
需要知道它们是什么。

条目 schema（宿主 / 插件约定，数组按 `order` 升序渲染）：

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `id` | string | 可选 | 稳定标识：宿主按 `(owner, id)` upsert；托盘只用它写日志 |
| `label` | string | 命令必填 | 菜单文字（UTF-8）；为空时退回 `id`，两者都空则跳过该条目 |
| `event` | string | 命令必填 | 点击时发布的事件主题；`separator` 条目可空 |
| `data` | string | 可选 | 事件负载；省略 / `null` = 空负载，非字符串（对象/数组/数字）按 JSON 文本发布 |
| `order` | number | 可选 | 升序排序键，缺省 `1000`（排在所有显式 order 之后）；同 order 保持宿主顺序 |
| `separator` | bool | 可选 | `true` = 渲染成分隔线（不可点击） |
| `default` | bool | 可选 | `true` = 左键单击托盘图标时执行该条目；多个时取排序后第一个 |

宿主预置的 core 三项形状示例（`id` / `order` 由宿主决定，托盘不依赖具体值；当前宿主实现见
`src/hostListCatalog.cpp`）：

```json
[
  {"id":"capture.request","label":"截屏","event":"capture.request","data":"","order":10,"separator":false},
  {"id":"settings.open","label":"打开设置","event":"settings.open","data":"","order":20,"separator":false},
  {"id":"app.quit","label":"退出","event":"app.quit","data":"","order":100,"separator":false}
]
```

**刷新**：托盘订阅 `lists.changed`。事件总线回调跑在宿主分发线程上，只做深拷贝入队 +
`PostMessage` 唤醒托盘线程；托盘线程重新 `list_json("tray_command_list")` 并用
`DestroyMenu` 重建菜单。菜单打开期间（`TrackPopupMenu` 模态循环中）不销毁正在使用的
句柄，只记 pending，菜单关闭后立刻补重建。`init` 时先构建一次；插件启动即贡献的命令、
插件启停导致的增删都会在下一轮 `lists.changed` 后出现在菜单里。

**兜底**：列表服务不可用（旧宿主 / 列表还没注册）、列表为空、JSON 非法时，菜单只保留
本地兜底的一项「退出」（发布 `app.quit`），并记一条 WARN。用户永远有办法从托盘退出
宿主，不会因为列表数据坏了就没有退出入口。

## 5. 事件

发布（本插件是发送方，不知道谁在听）：

| 主题 | 负载 | 说明 |
| --- | --- | --- |
| `capture.request` | 空 | 左键单击的兜底动作（列表里没有 `default` 条目时） |
| 菜单条目的 `event` | 条目的 `data` | 点击菜单项：core 预置 `capture.request` / `settings.open` / `app.quit`；插件贡献的命令发布它们自己的主题 |

订阅（本插件是接收方）：

| 主题 | 负载 | 说明 |
| --- | --- | --- |
| `capture.done` | JSON `{"path":"…","width":N,"height":N,"processors":N}` | 保存成功，气泡显示 `path` |
| `capture.failed` | JSON `{"stage":"…","reason":"…"}` | 某个阶段失败，气泡显示 `reason` |
| `lists.changed` | JSON `{"list":"<list id>"}` | 列表变化，只用于唤醒托盘线程重建右键菜单 |

订阅回调跑在**宿主分发线程**上，只做两件事：把主题与负载深拷贝进队列、`PostMessage`
唤醒托盘线程；解析 JSON 与 `Shell_NotifyIconW` 全部在托盘线程上做（`EVENT_BUS.md` §3）。
负载不是约定 JSON 时安静降级成"路径未知 / 原因未知"的气泡并记一条 WARN，不会吞掉通知。
`lists.changed` 只在托盘线程上解释：`list` 不是 `tray_command_list` 时忽略；空负载按
"可能是任意列表"处理，宁可多重建一次也不漏刷新。

## 6. 配置项（`shot.configurable`）

| key | kind | 默认 | 含义 |
| --- | --- | --- | --- |
| `notify` | `BOOL` | `"1"` | 保存成功后是否弹气泡 |
| `notify_on_error` | `BOOL` | `"1"` | 采集/处理/保存失败时是否弹气泡 |

值一律用字符串 `"1"` / `"0"` 表达，存盘键是 `tray.main/<key>`（宿主负责落盘与启动回灌，
见 `document/SETTINGS.md`）。`set_config` 可能在宿主任意线程上被调用，当前值用
`std::atomic<bool>`，托盘线程读到的永远是完整值；不认识的 key 返回负值且不改变状态。

## 7. 生命周期与线程模型

| 时机 | 插件做什么 |
| --- | --- |
| `init(self, host)` | 订阅 `capture.done` / `capture.failed` / `lists.changed`；起托盘线程，线程里建隐藏消息窗口 + `Shell_NotifyIconW(NIM_ADD)` + 从 `tray_command_list` 构建一次右键菜单；返回 `0` 时图标已由 Shell 接受、线程在跑 |
| `post_to_plugin` | 收到 `host.shutdown` → 直接 `stop()`；其它事件深拷贝后入队 + `PostMessage` 唤醒，不阻塞宿主 |
| `deinit(self)` | 退订事件总线 → 拒绝新投递 → 让消息循环退出 → `NIM_DELETE` → `DestroyMenu` → `join` 托盘线程；返回时图标、菜单、线程、窗口类都已不存在 |
| 宿主退出 / 热卸载 | 宿主先投 `host.shutdown` 再 `deinit`，两条路径都收敛到同一个 `stop()`（幂等） |
| explorer.exe 重启 | 收到 Shell 广播的 `TaskbarCreated` 注册消息后重新 `NIM_ADD`，用户不用重启宿主 |
| `lists.changed` | 事件总线回调只入队 + `PostMessage`；托盘线程重新读列表并重建 HMENU（菜单打开中先记 pending，关闭后重建） |

线程只有两个来源：宿主线程（`init` / `deinit` / `post_to_plugin` / 事件总线回调）与插件
自己的托盘线程。事件总线回调不碰 Win32 对象，所有托盘/窗口/菜单操作都串行在托盘线程上；
`stop()` 返回时线程已 `join`，不会留下后台线程或图标。**窗口类在 deinit / 线程退出时
显式注销**（`UnregisterClassW`）：窗口类属于进程、不随 DLL 卸载消失，不注销的话禁用托盘
会让类里的 `lpfnWndProc` 指向被 `FreeLibrary` 掉的代码，再启用时 `CreateWindowExW`
分发消息就会踩悬空指针崩溃（实测 `0xC000041D`）。注销之后**禁用→启用可以反复重新加载**；
如果进程里仍有同名遗留类（例如上次异常退出），`ensureWindowClass()` 会先清掉它再重新注册，
清不掉就返回失败，绝不沿用未知窗口过程。

## 8. 平台限制

- **仅 Windows**：托盘层用 `RegisterClassExW` + `Shell_NotifyIconW`；macOS（`NSStatusBar`）
  与 Linux（StatusNotifier / AppIndicator）没有实现，非 Windows 平台整个工程被 CMake 跳过。
- 图标目前用系统共享的应用图标（`LoadIconW(nullptr, IDI_APPLICATION)`）兜底：插件还没有
  自己的 `.rc` 图标资源，换成自带资源的位置在 `tray_icon_window.cpp` 里标好了。
- 气泡通知依赖系统"通知"设置；用户在系统里关掉通知时 `Shell_NotifyIconW` 仍然成功，
  只是看不到气泡——插件无法、也不应该绕过这个用户选择。

## 9. 为什么这么设计

| 决定 | 理由 |
| --- | --- |
| 托盘线程 + 隐藏消息窗口（PIMPL） | `Shell_NotifyIconW` 要一个 `hWnd` 收回调；Win32 类型不可能顺着契约漏给宿主，宿主连 `<windows.h>` 都不需要包含 |
| 右键菜单完全数据驱动（托盘只渲染） | 加命令 = 插件发 `lists.add`，不用改/重编托盘；core 命令也在同一张列表里，托盘没有 `kMenuCapture` 之类硬编码 |
| 菜单数据模型与 Win32 渲染分两个文件 | 解析逻辑不依赖 Win32，可以用假 JSON 离线验证排序 / 分隔线 / 兜底；窗口代码只处理 `HMENU`，改菜单不用碰线程握手 |
| `lists.changed` 回调只唤醒、重建在托盘线程 | 回调跑在宿主分发线程；`HMENU` 不是线程安全的，而且 `TrackPopupMenu` 打开期间不能 `DestroyMenu`，所以用 pending 标记延后 |
| 本地兜底「退出」 | 列表服务不可用时托盘至少还能退出宿主，不会变成"只能看不能点"；这一项是兜底，core 正常预置的「退出」仍然来自列表 |
| 左键的默认动作可被 `default` 覆盖 | "单击图标干什么"是用户/数据决定的事，不是托盘写死的；没有 `default` 时保持历史行为 `capture.request` |
| 所有跨插件动作只发事件 | 插件之间不能直接互调（跨 DLL 只能走宿主）；托盘不知道自己之外还有谁，宿主换掉设置页/采集插件都不用改托盘（`EVENT_BUS.md` §1） |
| `notify` / `notify_on_error` 做成配置项 | 弹不弹气泡是用户偏好，改成硬编码就得让用户改配置文件；描述符驱动后设置页自动出现这一节 |
| 关掉通知时只记 DEBUG 不记 WARN | 用户主动关的东西不该在日志里反复告警 |
| `host.shutdown` 直接 `stop()` 而不是排进队列 | 卸载路径要在 `deinit` 返回前收干净；直接走同一套 `stop()` 路径最简单，也不会和"线程没起好"的窗口期打架 |

## 10. 降级行为

| 情况 | 行为 |
| --- | --- |
| 宿主没有事件总线（旧版宿主） | 记一条 WARN；托盘图标照常出现，但发不出事件、也收不到截屏结果 |
| 宿主没有列表服务 / 列表为空 / JSON 非法 | 菜单只保留本地兜底「退出」（`app.quit`）并记 WARN；其余功能不受影响 |
| 条目缺 `event`，或 `label` 与 `id` 都为空 | 跳过该条目并记 WARN，其余条目照常渲染 |
| 条目的 `data` 不是字符串 | 序列化成 JSON 文本发布，事件消费方仍按 JSON 解析 |
| `lists.changed` 在菜单打开期间到达 | 记 pending，菜单关闭后重建，不动正在使用的 `HMENU` |
| 菜单重建失败（`CreatePopupMenu` 失败等） | 保留旧菜单并记 WARN；托盘图标与气泡不受影响 |
| 事件队列满 / 发布失败 | 记一条 WARN 并丢弃该次点击，不阻塞、不重试（托盘不是关键路径） |
| `capture.done` / `capture.failed` 负载不是约定 JSON | 气泡退化成"路径未知 / 原因未知"的通用提示，并记 WARN |
| 气泡接口失败（`NIM_MODIFY` 返回 FALSE） | 记一条 WARN，不影响托盘图标与菜单 |
| `LoadIconW` 失败 / `NIM_ADD` 失败 | `init` 返回失败，宿主按"这个 UI 插件初始化失败"处理，其余功能不受影响 |
| 配置项被写成不认识的值 | 按默认值（开）处理，不因一个坏配置让托盘失去通知 |

## 11. 检查清单

- [ ] 菜单里没有硬编码命令（没有 `kMenuCapture` 之类）：加命令只改列表数据 / 发 `lists.add`？
- [ ] 点击菜单项只 `publish` 条目声明的 `event`，没有直接 `query_interface` 别的插件？
- [ ] `lists.changed` 回调里只做入队与 `PostMessage`，重建 `HMENU` 只在托盘线程上？
- [ ] 菜单打开期间（`TrackPopupMenu`）不重建 / 不销毁 `HMENU`，关闭后补重建？
- [ ] 列表不可用时菜单仍有兜底「退出」（`app.quit`）？
- [ ] 事件总线回调里只做入队与 `PostMessage`，没有直接调 `Shell_NotifyIconW`？
- [ ] `deinit` / `host.shutdown` 返回前退订了事件总线、停了托盘线程、删掉了图标、销毁了菜单、**注销了窗口类**（否则禁用→启用会踩悬空 WndProc）？
- [ ] 异常没有越过 `extern "C"` 边界（`create` / `init` / `post` / `deinit` 都有 catch 兜底）？
- [ ] Win32 类型只出现在内部头 `tray_icon_impl.h` 与窗口相关 `.cpp` 里（公共头不含 `<windows.h>`）？
- [ ] 新加的配置键是小写下划线、默认值是字符串、不认识的 key 返回负值？
- [ ] `metadata.json` 的 `interfaces` 与实际 `query_interface` 回答一致（`shot.ui_provider` + `shot.configurable`）？
