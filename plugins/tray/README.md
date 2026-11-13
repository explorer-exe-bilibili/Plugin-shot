# tray —— 托盘插件（`shot.ui_provider`）

无主窗口架构下的托盘入口：图标、菜单和插件自己的线程全部由插件创建和管理，宿主只调
`init` / `deinit` / `post_to_plugin`——Windows 上宿主拿不到 `HWND`，Linux 上宿主也看不到
任何 GDBus 对象。

**两套后端，同一个公共接口（`include/tray_icon.h`）与同一份菜单数据：**

| 平台 | 实现 | 图标 | 主激活（左键） |
| --- | --- | --- | --- |
| Windows | Win32 托盘（隐藏消息窗口 + `Shell_NotifyIconW` + `HMENU`） | 宿主 exe 命名资源 `IDI_ICON1`；`icon` 配置可换成 `.ico` 文件 | 列表里 `"default":true` 的条目；没有则 `capture.request` |
| Linux | StatusNotifierItem（`org.kde.StatusNotifierItem`，GIO/GDBus）+ `com.canonical.dbusmenu` | 桌面图标主题里的 `camera-photo-symbolic`（`IconName`） | 列表里 `"default":true` 的条目；没有则 `settings.open` |
| macOS | 没有 `NSStatusBar` 实现，整个工程在 CMake 里跳过 | — | — |

**菜单是纯数据驱动的**：内容完全来自宿主列表服务里的 `tray_command_list`——宿主预置
core 四项（截屏 / 区域截屏 / 打开设置 / 退出），插件运行期还可以用 `lists.add` 往这个列表
里加命令；点击任何条目都只把条目声明的事件（和 `data` 负载）发到事件总线。本插件不认识、
也不直接调用任何其它插件（列表协议见 `document/LISTS.md`，事件与线程模型见
`document/EVENT_BUS.md`、`document/SETTINGS.md`）。

**通知全部移交独立插件**：托盘**不弹任何气泡**、也不订阅任何 `capture.*` 结果事件；
截屏成功/失败提示都由 `plugins/notify` 负责（避免同一件事被两个插件各弹一次）。

**多语言**：菜单 `label`、配置项 label/tooltip、兜底「退出」与图标悬停提示都跟随界面语言
（`shot::I18n`；`document/I18N.md`）。菜单条目里的 `label` 允许是"字面量或文本 key"，
托盘统一按 `display(owner,label) = i18n_list[(owner,label)] ?? label` 解析：老插件直接写
中文菜单名照常显示（零破坏），新插件写命令 id 作为 key 就能自动翻译。

## 1. 目录

```
plugins/tray/
├── CMakeLists.txt                  # 平台分支：Windows(Win32) / Linux(GIO) / macOS 跳过
├── metadata.json                   # 插件清单（id tray.main，接口 shot.ui_provider + shot.configurable）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── tray_config.h               # TrayConfig 公共接口（只有 icon 一个配置项）
│   ├── tray_icon.h                 # TrayIcon 公共接口（PIMPL，不含 Win32/GDBus 类型）
│   ├── tray_icon_impl.h            # 内部头（Windows）：TrayIcon::Impl、事件主题、托盘消息与图标状态
│   ├── tray_icon_linux_impl.h      # 内部头（Linux）：TrayIcon::Impl、SNI/dbusmenu 常量与 GDBus 回调
│   └── tray_menu.h                 # TrayMenu / TrayCommandItem：tray_command_list 的数据模型（两平台共用）
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + kEntries + ABI 胶水（两平台共用）
    ├── tray_config.cpp             # shot.configurable：icon 声明 / 值解析 / 线程安全读写（两平台共用）
    ├── tray_menu.cpp               # tray_command_list JSON 解析 / 校验 / 排序 / 本地兜底（两平台共用）
    │
    ├── tray_icon.cpp               # Windows：生命周期 / 线程握手 / 事件投递
    ├── tray_icon_window.cpp        # Windows：窗口线程 / 消息循环 / 托盘鼠标回调
    ├── tray_icon_icon.cpp          # Windows：托盘图标来源与刷新
    ├── tray_icon_menu.cpp          # Windows：HMENU 重建 / TrackPopupMenu / 点击发布事件
    ├── tray_icon_events.cpp        # Windows：事件出队 / UTF-8 转宽字符 / lists.changed 分发
    │
    ├── tray_icon_linux.cpp         # Linux：生命周期 / 服务线程 / 私有 GDBus 连接 / lists.changed 队列
    ├── tray_sni_linux.cpp          # Linux：org.kde.StatusNotifierItem 对象 / 属性 / Activate / watcher 注册
    └── tray_dbusmenu_linux.cpp     # Linux：com.canonical.dbusmenu 对象 / GetLayout / Event / 菜单重建
```

公共头一个类一个（`TrayIcon` / `TrayConfig` / `TrayMenu`）；两个 `*_impl.h` 是**内部头**：
只被本插件对应平台的 `.cpp` 包含。`.cpp` 按关注点拆分：配置 / 菜单数据两平台共用，
生命周期 / 平台渲染按后端分开，每个文件都能一句话说清职责（`STYLE.md` §6.8）。

产物：Windows 是 `<构建树>/bin/plugins/tray.dll`，Linux 是 `tray.so`；根 `CMakeLists.txt`
通过 `plugins/*/CMakeLists.txt` 的 glob 自动纳入本工程，不需要手改根文件。

## 2. 构建

宿主仓库内构建（`shot_add_plugin()` 由根工程提供）：

```bash
# Windows
cmake -S . -B build/Windows -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows --target tray

# Linux（Ubuntu 24.04）
cmake --preset release
cmake --build build/Linux --target tray
```

离线 SDK 独立构建：

```bash
cmake -S plugins/tray -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

依赖：

| 平台 | 依赖 | 用途 | 说明 |
| --- | --- | --- | --- |
| 两平台 | `nlohmann-json` | 解析 `tray_command_list`（菜单数据）与 `lists.changed` 负载 | header-only，vcpkg 清单已有 |
| Windows | `user32` / `shell32` | 窗口类、消息循环、右键菜单、`Shell_NotifyIconW` | 系统库 |
| Linux | `gio-2.0`（`libglib2.0-dev`） | GDBus 会话总线、SNI/dbusmenu、`GMainContext` 服务线程 | **系统包**，跟桌面会话同一份 GLib；不是 vcpkg 依赖，`vcpkg.json` 不需要改 |
| Linux | `Threads` | 插件服务线程 | 系统库 |

Linux 缺 GIO 开发包时 **configure 阶段直接失败**并提示安装 `libglib2.0-dev`
（`pkg_check_modules(GIO ...)`，见 `CMakeLists.txt`），不会编到一半才发现 `<gio/gio.h>` 不存在。

## 3. 交互

### 3.1 Windows

| 用户动作 | 插件行为 |
| --- | --- |
| 左键单击图标 | 列表里有 `"default":true` 条目就发布它的事件；没有则发布 `capture.request`（core 预置项都没有 `default`，所以默认仍是截屏） |
| 右键单击图标 | 弹出数据驱动菜单：`tray_command_list` 快照按 `order` 升序渲染，`separator:true` 渲染成分隔线 |
| 点击菜单项 | 发布该条目声明的 `event`（负载 = 条目的 `data`，原样透传），不直接调用任何插件 |

菜单用 `TrackPopupMenu(TPM_RETURNCMD | TPM_NONOTIFY)` 直接拿返回值，不经过 `WM_COMMAND`；
弹菜单前先 `SetForegroundWindow`、关掉后补一条 `WM_NULL`，避免"点菜单外面"的那次鼠标
抬起被当成左键点击、又执行一次默认命令（托盘菜单的经典坑，见 `tray_icon_menu.cpp` 注释）。

### 3.2 Linux（StatusNotifierItem）

Linux 上图标常驻在桌面顶栏的角度是"SNI item"：插件在会话总线上拥有
`org.kde.StatusNotifierItem-<pid>-1`，导出 `/StatusNotifierItem` 与
`/StatusNotifierItem/Menu` 两个对象。Ubuntu 24.04 GNOME 默认带 **Ubuntu AppIndicators**
扩展（实现了 `org.kde.StatusNotifierWatcher` 与 SNI 宿主），Wayland / X11 都适用——SNI 是
D-Bus 协议，与显示服务器无关。

| 用户动作 | 插件行为 |
| --- | --- |
| 图标悬停 | 宿主读取 SNI `Title`（本插件的文案 key 是 `tray.tooltip`） |
| 左键 Activate | 列表里有 `"default":true` 条目就发布它的事件；没有（core 预置项都没有）则发布 `settings.open`。与 Windows「左键执行 default 命令」语义对齐，但 Linux 没有"双击托盘打开主窗口"的约定，所以默认落到非破坏性的设置页 |
| `SecondaryActivate` | 与 `Activate` 相同（中键/次要激活也当作"打开入口"） |
| 点击图标弹菜单 | 由 SNI 宿主直接按 `Menu` 属性（`/StatusNotifierItem/Menu`）调 `com.canonical.dbusmenu` 的 `GetLayout`，插件不参与弹窗 |
| 点击菜单项 | 宿主调 `dbusmenu.Event(id, "clicked")` → 插件发布该条目声明的 `event`（负载 = `data`） |
| `ContextMenu` | 规范要求的方法；菜单由宿主通过 dbusmenu 打开，插件只需回空回复（无操作） |

SNI 属性：`Category="ApplicationStatus"`、`Id="plugin-shot"`、`Title`（i18n）、
`Status="Active"`、`IconName="camera-photo-symbolic"`、`Menu="/StatusNotifierItem/Menu"`、
`ItemIsMenu=true`。方法：`Activate` / `SecondaryActivate` / `ContextMenu`。
`ItemIsMenu=true` 表示"图标只负责菜单"——GNOME/KDE 的 SNI 宿主点击图标时直接展开菜单，
这也是 AppIndicator 的常规交互。

托盘**不订阅、不处理** `capture.done` / `capture.failed`：截屏结果提示由独立的
`plugins/notify` 负责（见上）。

## 4. 菜单数据（`tray_command_list`）

菜单没有一行硬编码命令：托盘用 `shot::PluginLists(host).json("tray_command_list")` 取
宿主聚合好的只读 JSON 快照（两段式读取由辅助头封装），解析成 `TrayCommandItem` 后交给
平台渲染（Windows `HMENU` / Linux dbusmenu）。core 四项（截屏 / 区域截屏 / 打开设置 /
退出）也由宿主预置在同一张列表里，托盘不需要知道它们是什么。

条目 schema（宿主 / 插件约定，数组按 `order` 升序渲染）：

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `id` | string | 可选 | 稳定标识：宿主按 `(owner, id)` upsert；托盘只用它写日志 |
| `owner` | string | 宿主注入 | 贡献者 id（`core` 或插件 id）；托盘用 `resolve(owner,label)` 解析文本 key，条目里不需要自己写 |
| `label` | string | 命令必填 | 菜单文字：**字面量或文本 key**（UTF-8），托盘按 `display(owner,label)` 用 `i18n_list` 解析；为空时退回 `id`，两者都空则跳过该条目 |
| `event` | string | 命令必填 | 点击时发布的事件主题；`separator` 条目可空 |
| `data` | 任意 JSON | 可选 | 事件负载：字符串按原字节发布；对象/数组/数字等按 JSON 文本原样发布 |
| `order` | number | 可选 | 升序排序键，缺省 `1000`（排在所有显式 order 之后）；同 order 保持宿主顺序 |
| `separator` | bool | 可选 | `true` = 渲染成分隔线（不可点击） |
| `default` | bool | 可选 | `true` = 主激活（Windows 左键 / Linux SNI Activate）时执行该条目；多个时取排序后第一个 |

`data` 是**原样透传**的事件负载，托盘不解析它：例如 core 的「区域截屏」条目带
`"data": {"mode":"region"}`，点击后订阅者收到的负载就是 JSON 文本 `{"mode":"region"}`；
字符串负载则按原字节发出（可以直接是 JSON 字符串，消费方自己解析）。

宿主预置的 core 四项（`src/hostListCatalog.cpp`；`id` / `order` 由宿主决定，托盘不依赖
具体值）：

```json
[
  {"id":"capture.request","label":"capture.request","event":"capture.request","data":"","order":10,"separator":false},
  {"id":"region.capture","label":"region.capture","event":"capture.request","data":{"mode":"region"},"order":15,"separator":false},
  {"id":"settings.open","label":"settings.open","event":"settings.open","data":"","order":20,"separator":false},
  {"id":"app.quit","label":"app.quit","event":"app.quit","data":"","order":100,"separator":false}
]
```

「区域截屏」的 `{"mode":"region"}` 是"请开始框选"的约定：`plugins/region_selector` 收到
这条请求后弹覆盖层，用户确认后再发一条**带 `region`** 的 `capture.request`，由宿主流水线
裁剪（见 `document/CAPTURE.md` §3.1）。这条触发请求本身会被流水线**忽略**（不采集、不落盘、
不发 `capture.*`），不会额外存一张全屏图。

**刷新（两平台同一语义）**：托盘订阅 `lists.changed`。事件总线回调跑在宿主分发线程上，
只做深拷贝入队 + 唤醒插件线程；插件线程重新 `list_json("tray_command_list")` 并重建菜单。
`init` 时先构建一次；插件启动即贡献的命令、插件启停导致的增删都会在下一轮
`lists.changed` 后出现在菜单里。

- Windows：托盘线程重建 `HMENU`；菜单打开期间（`TrackPopupMenu` 模态循环中）不销毁正在
  使用的句柄，只记 pending，菜单关闭后立刻补重建。
- Linux：服务线程重建 `TrayMenu` 模型（dbusmenu 的布局是"现取现答"，没有需要销毁的客户端
  句柄），并广播 `com.canonical.dbusmenu.LayoutUpdated(revision, 0)` 通知已打开菜单的宿主
  重新 `GetLayout`。布局 item id 从 1 开始（root 是 0），与 `TrayMenu` 条目下标一一对应。

**兜底**：列表服务不可用（旧宿主 / 列表还没注册）、列表为空、JSON 非法时，菜单只保留
本地兜底的一项「退出」（发布 `app.quit`），并记一条 WARN。用户永远有办法从托盘退出
宿主，不会因为列表数据坏了就没有退出入口。兜底项文字与图标悬停提示同样按当前语言生成
（`shot::I18n::tr`，老宿主退回中文）。

**语言刷新**：`i18n_list` 变化同样走 `lists.changed`（负载 `{"list":"i18n_list"}`）。
托盘的列表过滤同时接受 `tray_command_list` 与 `i18n_list`：前者是菜单条目变化，
后者是语言变化；两路都会触发重建菜单（`TrayMenu::load` 用
`shot::I18n::resolve` 取新语言文本）。Windows 还会用 `NIF_TIP + NIM_MODIFY` 更新图标
悬停提示；Linux 的 `Title` 是每次被查询时按当前语言现算的，不需要额外刷新。

## 5. 图标

### 5.1 Windows

托盘默认显示**宿主应用图标**：宿主 exe 的资源里是 `IDI_ICON1 ICON "Plugin-shot.ico"`
（`app.rc.in` 里它是**命名资源**），插件在宿主进程里用
`LoadIconW(GetModuleHandleW(nullptr), L"IDI_ICON1")` 取到——`GetModuleHandleW(nullptr)`
返回的是宿主 exe 模块，不是本插件 DLL，所以拿到的正是"整个应用"的图标。
（正确做法始终是命名资源；只有宿主 rc 被改成遗留写法 `1 ICON "..."` 时，本插件才额外按
`MAKEINTRESOURCEW(1)` 做一次兼容尝试——文件图标 ID 不是 `1`。）

`icon` 配置非空时改用该 `.ico` 文件：配置值是 UTF-8 路径，先转 UTF-16 再
`LoadImageW(nullptr, path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE)`。
来源优先级与回退（每一步都记日志）：

1. `icon` 配置的文件（加载失败记 WARN，继续下一步）；
2. 宿主应用图标 `IDI_ICON1`（拿不到记 WARN，继续下一步）；
3. 系统默认应用图标 `IDI_APPLICATION`。

**句柄归属**：宿主/系统图标是 `LoadIconW` 返回的共享资源，**不能** `DestroyIcon`；
从文件 `LoadImageW` 出来的图标归本插件所有，换图标/删除图标时必须 `DestroyIcon`
（`tray_icon_icon.cpp` 用 `iconOwned` 区分这两种情况）。

**运行期更换**：设置页应用 `icon` 后，宿主把值推给插件并发布 `config.applied`；托盘订阅
该事件，在托盘线程上重读配置并用 `NIM_MODIFY` 换图标。宿主启动时 `apply_stored` 会直接
`set_config`（那时还没有 `config.applied`），所以 `set_config` 回调里也会唤醒托盘线程；
两条路径都只做"重读配置"，路径没变就不碰 Shell。

### 5.2 Linux

图标用 **SNI `IconName`（主题图标名）**：`camera-photo-symbolic`。GNOME/KDE 的 SNI 宿主
按当前图标主题取图，图标会跟随桌面深浅色与主题变化，也不需要插件内嵌位图。
这是二选一里选的方案（另一个方案是内置 `IconPixmap` ARGB 位图）；没有主题图标时宿主会
显示占位图标，属于桌面环境行为。

**`icon` 配置在 Linux 上被忽略**：它是 Windows 的 `.ico` 文件路径，SNI 无法直接消费；
`requestIconRefresh()` 在 Linux 上是空操作。设置页该项在 Linux 上仍然显示（配置声明是
两平台共用的），插件启动时如果发现配了值会记一条 WARN 说明原因。

## 6. 事件

发布（本插件是发送方，不知道谁在听）：

| 主题 | 负载 | 说明 |
| --- | --- | --- |
| `capture.request` | 空 | Windows 左键单击的兜底动作（列表里没有 `default` 条目时） |
| `settings.open` | 空 | Linux SNI `Activate` 的兜底动作（列表里没有 `default` 条目时）；有 `default` 时发布 default 条目 |
| 菜单条目的 `event` | 条目的 `data`（原样） | 点击菜单项：core 预置 `capture.request`（整屏）/ `capture.request`（`{"mode":"region"}`）/ `settings.open` / `app.quit`；插件贡献的命令发布它们自己的主题 |

订阅（本插件是接收方）：

| 主题 | 负载 | 说明 |
| --- | --- | --- |
| `lists.changed` | JSON `{"list":"<list id>"}` | 列表变化，唤醒插件线程重建菜单 |
| `config.applied` | 空 | Windows：批量配置已落盘，唤醒托盘线程重读 `icon` 配置、必要时换图标 |

订阅回调跑在**宿主分发线程**上，只做两件事：把主题与负载深拷贝进队列、唤醒插件线程；
平台操作全部在插件线程上做（`EVENT_BUS.md` §3）。托盘**不订阅任何 `capture.*` 结果
事件**：成功/失败提示由 `plugins/notify` 负责。`lists.changed` 只在插件线程上解释：
`list` 不是 `tray_command_list` / `i18n_list` 时忽略；空负载按"可能是任意列表"处理，
宁可多重建一次也不漏刷新。

## 7. 配置项（`shot.configurable`）

| key | kind | 默认 | 含义 |
| --- | --- | --- | --- |
| `icon` | `PATH`（`options="file"`） | `""` | 自定义托盘图标 `.ico` 文件（**仅 Windows**）；空 = 用宿主应用图标 |

值一律用字符串表达，存盘键是 `tray.main/icon`（宿主负责落盘与启动回灌，见
`document/SETTINGS.md`）。`icon` 的 `options="file"` 让设置页弹"选择文件"对话框而不是
"选择目录"。`set_config` 可能在宿主任意线程上被调用：图标路径用 mutex 保护并返回副本。
不认识的 key 返回负值且不改变状态。

> 截屏结果通知的开关已经全部移到 `plugins/notify`（`enabled` / `show_on_success` /
> `show_on_error` / `sound`）；托盘不再有任何气泡配置。

## 8. 生命周期与线程模型

### 8.1 Windows

| 时机 | 插件做什么 |
| --- | --- |
| `init(self, host)` | 订阅 `lists.changed` / `config.applied`；起托盘线程，线程里建窗口 + 按 `icon`/宿主图标 `NIM_ADD` + 构建一次右键菜单；返回 `0` 时图标已被 Shell 接受 |
| `post_to_plugin` | 收到 `host.shutdown` → 直接 `stop()`；其它事件深拷贝后入队 + `PostMessage` 唤醒，不阻塞宿主 |
| `set_config` | 更新配置值；成功后唤醒托盘线程重读 `icon` 配置（apply_stored 阶段没有 `config.applied`，不能只依赖事件） |
| `deinit(self)` | 退订事件总线 → 拒绝新投递 → 让消息循环退出 → `NIM_DELETE` → `DestroyMenu` → 释放自有图标句柄 → `join` 托盘线程；返回时图标、菜单、线程、窗口类都已不存在 |
| 宿主退出 / 热卸载 | 宿主先投 `host.shutdown` 再 `deinit`，两条路径都收敛到同一个 `stop()`（幂等） |
| explorer.exe 重启 | 收到 Shell 广播的 `TaskbarCreated` 注册消息后重新 `NIM_ADD`，用户不用重启宿主 |
| `lists.changed` | 事件总线回调只入队 + `PostMessage`；托盘线程重新读列表并重建 HMENU（菜单打开中先记 pending，关闭后重建） |
| `config.applied` | 事件总线回调只入队 + `PostMessage`；托盘线程重读 `icon` 配置、路径变了才换图标 |

线程只有两个来源：宿主线程（`init` / `deinit` / `post_to_plugin` / `set_config` / 事件总线
回调）与插件自己的托盘线程。事件总线回调不碰 Win32 对象，所有托盘/窗口/菜单/图标操作都
串行在托盘线程上；`stop()` 返回时线程已 `join`，不会留下后台线程或图标。**窗口类在
deinit / 线程退出时显式注销**（`UnregisterClassW`）：窗口类属于进程、不随 DLL 卸载消失，
不注销的话禁用托盘会让类里的 `lpfnWndProc` 指向被 `FreeLibrary` 掉的代码，再启用时
`CreateWindowExW` 分发消息就会踩悬空指针崩溃（实测 `0xC000041D`）。注销之后**禁用→启用
可以反复重新加载**；如果进程里仍有同名遗留类（例如上次异常退出），`ensureWindowClass()`
会先清掉它再重新注册，清不掉就返回失败，绝不沿用未知窗口过程。

### 8.2 Linux

| 时机 | 插件做什么 |
| --- | --- |
| `init(self, host)` | 订阅 `lists.changed`；创建私有 `GMainContext`（第一次）与 `GMainLoop`，起服务线程；服务线程用**私有** `GDBusConnection` 连接会话总线（不是 `g_bus_get` singleton，避免和 `screen_capture` 的 libportal 抢派发上下文），注册 `/StatusNotifierItem` + `/StatusNotifierItem/Menu`，构建一次菜单，`g_bus_own_name_on_connection("org.kde.StatusNotifierItem-<pid>-1")`；`init` 返回 `0` 时线程已拉起，SNI 注册是异步的（不阻塞宿主） |
| `post_to_plugin` | 收到 `host.shutdown` → 直接 `stop()`；`lists.changed` 走与总线相同的队列；其它主题静默接受（Linux 托盘不消费） |
| `set_config` | 更新配置值；Linux 上图标不依赖 `icon`，只记 WARN 说明被忽略，不触碰 D-Bus |
| `deinit(self)` | 退订事件总线 → 拒绝新投递 → 取消可能卡在连接阶段的线程 → 把关闭动作投到服务线程（注销两个对象、`g_bus_unown_name`、`g_object_unref` 连接、`g_main_loop_quit`）→ `join`；返回时 SNI 已注销、公共名字已释放、D-Bus 连接已销毁 |
| 宿主退出 / 热卸载 | 宿主先投 `host.shutdown` 再 `deinit`，两条路径都收敛到同一个 `stop()`（幂等） |
| `lists.changed` | 事件总线回调只入队 + `g_main_context_invoke_full` 唤醒；服务线程重新读列表、重建模型并广播 `LayoutUpdated` |
| 没有 `org.kde.StatusNotifierWatcher` | 向 watcher 的注册调用异步失败，**只记一条 WARN**（指向 GNOME AppIndicator 扩展），插件与宿主都不受影响；不重试、不刷屏 |
| 没有 D-Bus 会话总线 | 服务线程记一条 WARN 后空闲等待 `stop()`；插件其余功能不受影响，宿主退出不阻塞 |

Linux 的线程也只有两个来源：宿主线程与插件服务线程。事件总线回调不碰 GDBus 对象；
菜单模型、DBus 方法派发与信号发送都串行在服务线程上，所以不需要额外加锁。服务线程
用的 `GMainContext` **故意活到 `TrayIcon` 析构才释放**：`stop → start`（禁用→启用）时
可以继续复用它，异步回调不会被投递到已销毁的上下文。

注销顺序：先取消在途的 watcher 注册（回调按 CANCELLED 安静返回）→ 注销两个对象 →
释放接口描述 → `g_bus_unown_name` 释放公共名 → unref 私有连接 → `g_main_loop_quit`。
公共名释放后 watcher 会自动移除图标，即使连接随后关闭也不会留下"幽灵托盘项"。

## 9. 平台限制与真机验证状态

- **Windows**：Win32 托盘，宿主仓库的主验证平台，已有真机运行验证。
- **Linux**：StatusNotifierItem + dbusmenu 后端已随 Linux 全量构建通过（WSL Ubuntu 24.04，
  gio 2.80）。协议层用一次性假宿主探针在 `dbus-run-session` 里冒烟过（SNI 属性、
  dbusmenu `GetLayout` / `Event` / `LayoutUpdated`、无 watcher 与无会话总线降级、注销后
  服务名消失），但**没有在真实 GNOME Wayland/X11 桌面上做交互验证**：图标是否出现依赖
  `org.kde.StatusNotifierWatcher`（Ubuntu 默认的 Ubuntu AppIndicators 扩展）、菜单弹出与
  点击路径依赖真实 SNI 宿主；这些都需要真机人工确认（清单见下方）。
- **macOS**：没有 `NSStatusBar` 实现，CMake 跳过整个工程。
- 托盘不弹任何通知，也不受系统"通知"设置影响（气泡在 `plugins/notify`）。

### 真机验证清单（Linux）

- [ ] Ubuntu 24.04 GNOME（Wayland）顶栏出现 `camera-photo-symbolic` 图标（或在扩展面板里
      看到 `plugin-shot` / `Plugin-shot 托盘`）。
- [ ] 点图标弹出菜单，四项 core 命令（截屏 / 区域截屏 / 打开设置 / 退出）顺序正确、可点击。
- [ ] 点「打开设置」/「退出」分别发布 `settings.open` / `app.quit`（宿主日志有 `[tray] 发布 ...`）。
- [ ] 插件启停或 `lists.add` 后菜单增删在下一轮 `lists.changed` 后生效。
- [ ] 禁用 Ubuntu AppIndicators 扩展时：日志只有一条 WARN，宿主不崩、不卡。
- [ ] 禁用→启用托盘插件（热卸载）后图标能重新出现，无残留、无崩溃。
- [ ] X11 会话同样可用（SNI 与显示服务器无关）。

## 10. 为什么这么设计

| 决定 | 理由 |
| --- | --- |
| 托盘默认用宿主应用图标（Windows） | 托盘是"整个应用"的入口，不是某个插件能力的入口；宿主 exe 的 `IDI_ICON1` 就是用户认识的那个图标，插件也不必再带一份 `.rc` |
| 图标可配置成 `.ico` 文件（Windows） | 换图标是用户偏好，不该逼用户换宿主 exe；`icon` 配置走既有 `shot.configurable`，设置页自动出现该行并带"选择文件"对话框 |
| Linux 用 `IconName` 主题图标而不是内置位图 | SNI 宿主按系统图标主题取图，深浅色/主题切换自然；`camera-photo-symbolic` 是 GNOME 主题稳定存在的相机图标；也避免在插件里带一份 PNG 解码依赖 |
| 文件图标与共享图标分开管理（`iconOwned`） | `DestroyIcon` 共享图标会破坏其它窗口；不销毁文件图标会泄漏——两者混在一起是 Win32 图标最常见的坑 |
| 托盘线程 + 隐藏消息窗口（Windows，PIMPL） | `Shell_NotifyIconW` 要一个 `hWnd` 收回调；Win32 类型不可能顺着契约漏给宿主，宿主连 `<windows.h>` 都不需要包含 |
| 私有 `GMainContext` + 私有 `GDBusConnection`（Linux） | 宿主没有 GLib 主循环；`g_bus_get` 的进程级 singleton 会把消息派发固定在第一次使用时的上下文上，和 `screen_capture` 的 libportal 互相干扰。私有连接让托盘的 D-Bus 生命周期完全自洽 |
| 右键菜单完全数据驱动（托盘只渲染） | 加命令 = 插件发 `lists.add`，不用改/重编托盘；core 命令也在同一张列表里，托盘没有 `kMenuCapture` 之类硬编码 |
| 菜单数据模型与平台渲染分文件 | `tray_menu.cpp` 不依赖 Win32/GIO，两平台共用；解析逻辑可以用假 JSON 离线验证排序 / 分隔线 / 兜底 |
| `lists.changed` 回调只唤醒、重建在插件线程 | 回调跑在宿主分发线程；`HMENU` 不是线程安全的（Windows），GDBus 对象也属于服务线程上下文（Linux），所以都只入队 |
| Linux 主激活默认 `settings.open` | 与 Windows「左键 = default 命令」对齐；core 预置项没有 `default`，而 Linux 没有"托盘双击打开主窗口"，设置页是最安全的非破坏性主入口（截屏始终在菜单里） |
| 本地兜底「退出」 | 列表服务不可用时托盘至少还能退出宿主，不会变成"只能看不能点"；这一项是兜底，core 正常预置的「退出」仍然来自列表 |
| 所有跨插件动作只发事件 | 插件之间不能直接互调（跨 DLL 只能走宿主）；托盘不知道自己之外还有谁，宿主换掉设置页/采集插件都不用改托盘（`EVENT_BUS.md` §1） |
| 通知气泡全部交给 `plugins/notify` | 成功/失败提示有自己的开关与展示策略；托盘只做图标+菜单，避免同一件事被两个插件各弹一次 |
| `host.shutdown` 直接 `stop()` 而不是排进队列 | 卸载路径要在 `deinit` 返回前收干净；直接走同一套 `stop()` 路径最简单，也不会和"线程没起好"的窗口期打架 |

## 11. 降级行为

| 情况 | 行为 |
| --- | --- |
| `icon` 配置的文件不存在 / 不是 `.ico`（Windows） | 记 WARN，回退宿主应用图标；不影响菜单 |
| `icon` 配置非空（Linux） | 启动时记一条 WARN 说明 Linux 用主题图标；配置值本身不破坏任何东西 |
| 宿主 exe 没有 `IDI_ICON1` 资源（Windows） | 记 WARN，回退系统默认应用图标 |
| `NIM_MODIFY` 换图标失败（Windows） | 记 WARN，保留当前图标，不影响其余功能 |
| 宿主没有事件总线（旧版宿主） | 记一条 WARN；托盘图标照常出现，但发不出事件、也收不到列表通知 |
| 宿主没有列表服务 / 列表为空 / JSON 非法 | 菜单只保留本地兜底「退出」（`app.quit`）并记 WARN；其余功能不受影响 |
| 条目缺 `event`，或 `label` 与 `id` 都为空 | 跳过该条目并记 WARN，其余条目照常渲染 |
| 条目的 `data` 不是字符串 | 序列化成 JSON 文本原样发布，事件消费方按 JSON 解析 |
| `lists.changed` 在菜单打开期间到达（Windows） | 记 pending，菜单关闭后重建，不动正在使用的 `HMENU` |
| 菜单重建失败（`CreatePopupMenu` 失败等，Windows） | 保留旧菜单并记 WARN；托盘图标不受影响 |
| 事件队列满 / 发布失败 | 记一条 WARN 并丢弃该次点击，不阻塞、不重试（托盘不是关键路径） |
| `config.applied` 在图标工具就绪前到达（Windows） | 入队，托盘线程启动后补处理；重复刷新幂等 |
| `LoadIconW` / `NIM_ADD` 失败（Windows） | `init` 返回失败，宿主按"这个 UI 插件初始化失败"处理，其余功能不受影响 |
| 没有 `org.kde.StatusNotifierWatcher`（Linux） | 只记一条 WARN（提示启用 GNOME AppIndicator 扩展）；不重试、不阻塞，插件其余功能不受影响 |
| watcher 在插件启动之后才出现（Linux） | 不会自动补注册（按"不反复重试"的约定只尝试一次）；重启宿主或禁用→启用托盘插件即可重新注册 |
| 没有 D-Bus 会话总线（Linux） | 记一条 WARN，服务线程空闲等待 `stop()`；宿主正常启动/退出，不崩、不卡 |
| SNI 服务名被占用 / 连接断开（Linux） | 记 WARN 并停止提供图标，不影响插件其余功能；连接断开不会 raise SIGTERM（`exit_on_close=FALSE`） |
| `GetLayout` 的 parentId 不存在（Linux） | 回 D-Bus `InvalidArgs` 错误；正常宿主只查 parentId=0 |

## 12. 检查清单

- [ ] 菜单里没有硬编码命令（没有 `kMenuCapture` 之类）：加命令只改列表数据 / 发 `lists.add`？
- [ ] 点击菜单项只 `publish` 条目声明的 `event`（`data` 原样作为负载），没有直接 `query_interface` 别的插件？
- [ ] Windows：托盘图标默认取宿主 exe 的**命名资源** `IDI_ICON1`，不是插件 DLL 的图标？
- [ ] Windows：从文件 `LoadImageW` 加载的图标用 `DestroyIcon` 释放、共享图标不释放（`iconOwned`）？
- [ ] 事件总线回调里只做入队与唤醒，重建菜单 / 换图标 / D-Bus 操作只在插件线程上？
- [ ] Windows：菜单打开期间（`TrackPopupMenu`）不重建 / 不销毁 `HMENU`，关闭后补重建？
- [ ] Linux：SNI 属性齐全（`Category` / `Id` / `Title` / `Status=Active` / `IconName` / `Menu` / `ItemIsMenu`）？
- [ ] Linux：dbusmenu 布局 item id 从 1 开始、布局类型是规范的 `(ia{sv}av)`、`LayoutUpdated` 是 `(u revision, i parentId)`？
- [ ] Linux：`lists.changed` / `host.shutdown` 都在服务线程上重建/注销，`deinit` 返回前 SNI 与 D-Bus 连接已销毁、线程已 join？
- [ ] Linux：没有 watcher / 没有会话总线时只 WARN、不崩、不阻塞、不反复重试？
- [ ] 列表不可用时菜单仍有兜底「退出」（`app.quit`）？
- [ ] 托盘不订阅任何 `capture.*` 结果事件、不弹气泡（通知全部交给 `plugins/notify`）？
- [ ] 异常没有越过 `extern "C"` 边界或 GDBus C 回调帧（`create` / `init` / `post` / `deinit` / 所有 GDBus 回调都有 catch 兜底）？
- [ ] 平台类型只出现在对应的内部头（`tray_icon_impl.h` / `tray_icon_linux_impl.h`）与平台 `.cpp` 里（公共头不含 `<windows.h>` / `<gio/gio.h>`）？
- [ ] 配置键是小写下划线、默认值是字符串、不认识的 key 返回负值？
- [ ] `metadata.json` 的 `interfaces` 与实际 `query_interface` 回答一致（`shot.ui_provider` + `shot.configurable`）？
