# session_control —— Linux 的 D-Bus 会话控制入口（`shot.configurable`，Generic 插件）

Wayland 下第三方应用**没有可移植的全局热键 API**（GNOME 46 还没有
`org.freedesktop.portal.GlobalShortcuts`），所以本插件不在进程里抢按键，而是把触发入口
交给桌面环境：在**用户会话总线**上暴露 `org.pluginshot.Control`，用户在 GNOME / KDE 的
「自定义快捷键」里绑定一条 `gdbus call ...` 命令，桌面负责按键、本插件负责把 D-Bus 方法
翻译成既有的事件总线主题。

**这是 `plugins/hotkey` 在 Wayland 下的替代方案**：hotkey 插件在 Windows 用
`RegisterHotKey`、在 Linux X11 用 `XGrabKey`，纯 Wayland 下没有可用的全局热键 API；
本插件不注册任何热键，只提供“桌面能调用的命令入口”，因此 X11 会话也能用。

## 1. 它做什么、不做什么

| 做 | 不做 |
| --- | --- |
| 在会话总线注册 `org.pluginshot.Control` | 不注册、不拦截任何键盘快捷键 |
| 把 `Capture` / `CaptureRegion` / `OpenSettings` / `CaptureLong` 翻译成事件 | 不直接调用任何插件，也不解析截图结果 |
| 名称被占用时 WARN 并让插件继续加载 | 不弹 UI、不开窗口、不写配置文件 |
| `enabled` 关掉时注销 D-Bus 名称 | 不在 Wayland 上尝试任何全局热键变通（如 libinput 抓键） |

**当前链路状态**：本插件只管“触发”。Linux 侧的采集 / 保存 / 区域选择已由
`screen_capture` / `image_saver` / `region_selector` 插件实现，并在 Ubuntu 24.04（X11）与
Ubuntu 26（Wayland）真机跑通：`Capture()` 发布事件后能落盘，`CaptureRegion()` 会弹出 Linux
覆盖层并由流水线完成“选区 → 抓屏 → 保存”。

## 2. D-Bus 接口

| 项 | 值 |
| --- | --- |
| 总线 | 会话总线（`--session`，不是系统总线） |
| bus name | `org.pluginshot.Control` |
| object path | `/org/pluginshot/Control` |
| interface | `org.pluginshot.Control` |

方法（全部无入参，统一返回 `(b ok, s message)`）：

| 方法 | 发布的事件 | 负载 | 说明 |
| --- | --- | --- | --- |
| `Capture()` | `capture.request` | 空 | 整屏截图请求（与托盘「截屏」、hotkey 的截屏项完全相同） |
| `CaptureRegion()` | `capture.request` | `{"mode":"region"}` | “请开始框选”：流水线忽略、不采集；由区域选择插件消费（Windows / Wayland / X11 均已实现；Linux 已在 Ubuntu 24.04 X11 / Ubuntu 26 Wayland 真机验证） |
| `OpenSettings()` | `settings.open` | 空 | 打开设置页（`plugins/settings_ui` 消费） |
| `CaptureLong()` | `long_capture.start` | 空 | 滚动长图能力的预留入口；当前没有订阅者，成功发布但无效果 |

返回值的含义：

- `ok = true`：事件**已入队**（`publish` 返回成功），不代表截图已经完成；
- `ok = false`：`message` 给出原因（事件总线不可用 / 队列满 / 参数错误等）；
- 调用方拿到的文本是中文，直接显示在终端里。

示例（终端调试）：

```bash
gdbus call --session --dest org.pluginshot.Control \
  --object-path /org/pluginshot/Control \
  --method org.pluginshot.Control.Capture
# (true, '已发布 capture.request（整屏截图）')

gdbus introspect --session --dest org.pluginshot.Control \
  --object-path /org/pluginshot/Control
busctl --user introspect org.pluginshot.Control /org/pluginshot/Control
```

## 3. GNOME 绑定步骤（Ubuntu 24.04 / GNOME 46）

1. 先启动宿主并确认插件已加载（设置页「插件」里能看到 **Session Control**，或日志里有
   `已获取 D-Bus 名称 org.pluginshot.Control`）。
2. 打开 **设置 → 键盘 → 查看及自定义快捷键 → 自定义快捷键**（Settings → Keyboard →
   Keyboard Shortcuts → View and Customize Shortcuts → Custom Shortcuts）。
3. 点 **+**（添加），名称随意，例如 `截图`。
4. 命令填下面这一行（复制时保持单行）：

   ```bash
   gdbus call --session --dest org.pluginshot.Control --object-path /org/pluginshot/Control --method org.pluginshot.Control.Capture
   ```

5. 点「设置快捷键」，按一个组合键（例如 `Ctrl+Alt+S`），保存。

区域截屏 / 打开设置 / 长截图同理，只改最后的方法名：

```bash
gdbus call --session --dest org.pluginshot.Control --object-path /org/pluginshot/Control --method org.pluginshot.Control.CaptureRegion
gdbus call --session --dest org.pluginshot.Control --object-path /org/pluginshot/Control --method org.pluginshot.Control.OpenSettings
gdbus call --session --dest org.pluginshot.Control --object-path /org/pluginshot/Control --method org.pluginshot.Control.CaptureLong
```

> GNOME 的自定义快捷键会直接执行命令、不等待退出码；`gdbus call` 的输出（成功/失败）不会
> 显示在界面上。要确认是否真的发出去，看宿主日志，或先在终端里手动执行一次。
>
> 如果 `gdbus` 不存在：`sudo apt install libglib2.0-bin`（一般桌面版已预装）。

## 4. KDE / 其它桌面

| 桌面 | 等价做法 |
| --- | --- |
| KDE Plasma 5 | 系统设置 → 快捷键 → 自定义快捷键 → 编辑 → 新建 → 全局快捷键 → 命令/URL，命令同上 |
| KDE Plasma 6 | 系统设置 → 键盘 → 快捷键 → 添加 → 命令或脚本，命令同上 |
| XFCE | 设置 → 键盘 → 应用程序快捷键 → 添加，命令同上 |
| Cinnamon | 菜单 → 键盘 → 快捷键 → 自定义快捷键 → 添加，命令同上 |
| Sway / i3 / Hyprland | 在窗口管理器配置里绑定 `exec gdbus call --session ... --method org.pluginshot.Control.Capture` |
| 其它 X11 桌面 | 任何“执行命令”的快捷键入口都可以用；本插件与桌面环境无关，只要求同一用户会话总线可用 |

## 5. 配置项

设置页里属于本插件的唯一开关：

| 键 | 类型 | 默认 | 含义 |
| --- | --- | --- | --- |
| `enabled` | BOOL | `"1"` | 是否在会话总线注册 `org.pluginshot.Control`。关掉后名称立即注销（桌面快捷键再调用会得到 `ServiceUnknown`），插件本身继续加载、不影响其它插件；改回开时重新注册。 |

配置的启动回灌与运行期写值都走宿主既有的 `shot.configurable` 链路（`config_list` →
`lists.update` → `set_config`），不需要重启宿主。存盘键带插件 id 前缀：
`session_control.main/enabled`（规则见 `document/ABI_POLICY.md` §5）。`enabled=0` 也适合
“我只用别的方式截图、不想暴露 D-Bus 服务”的场景。

## 6. 生命周期与线程模型

```
SessionControlPlugin::start(host)
  └─ ControlService::start(host)
       ├─ 首次调用时创建私有 GMainContext，随后创建 GMainLoop
       └─ 新建专用线程：push 私有 GMainContext
            → g_bus_own_name(G_BUS_TYPE_SESSION, "org.pluginshot.Control", ...)   // 异步
            → g_main_loop_run()
                 ├─ bus_acquired  → register_object(/org/pluginshot/Control, 方法表)
                 ├─ name_acquired → INFO 日志
                 ├─ name_lost     → WARN 日志（名称被另一个宿主占用 / 会话总线断开）
                 └─ 方法调用      → EventBus::publish(...) → 回复 (b ok, s message)
```

- **为什么需要专用线程**：宿主没有 GLib 主循环，而 GDBus 的异步注册回调与方法派发都挂在
  调用线程的 thread-default `GMainContext` 上。只调用 `g_bus_own_name` 而不迭代上下文，
  名字永远拿不到、方法也永远不会被调用。
- `g_bus_own_name` 本身是非阻塞的；`start` 不等待名称获取完成。
- 事件发布可以在任意线程调用（`document/EVENT_BUS.md` §3），所以 D-Bus 方法回调里直接
  `shot::EventBus(host).publish(...)`，不需要再切线程。
- 同一用户开两个宿主时，后注册的那个 `name_lost` 打一条 WARN（D-Bus 会把它排在队列里），
  插件继续加载；先注册的宿主退出后，后注册的会自动拿到名称并记一条 `已获取 D-Bus 名称` 的
  INFO。这条 WARN 只会出现一次，不会刷屏。
- **销毁顺序**：`destroy` → `SessionControlPlugin::stop()` → `ControlService::stop()` 把
  shutdown 动作投递到服务线程 → 服务线程 `unregister_object` → `g_bus_unown_name` →
  `g_main_loop_quit` → 调用方 `join`。`stop()` 返回后本插件不再持有任何 GLib 回调，DLL 可以
  安全卸载（热卸载/重新加载也走这条路径）。
- **私有 `GMainContext` 不在 `stop()` 里释放**：`g_bus_own_name` 内部走 `g_bus_get` 的会话连接
  singleton —— 它是进程级唯一的连接，GDBus 把它的消息派发固定挂在创建时的 thread-default
  上下文上。上下文保留到 `ControlService` 析构（即插件卸载），`enabled` 关掉再打开时复用同一个
  上下文，新主循环才收得到方法调用；主循环本身每次 `start` 重建、线程随之重建。
- 成功调用只记 DEBUG 日志，失败/名称冲突记 WARN：桌面快捷键连按时不会刷屏。

## 7. 构建

本工程只在 **Linux** 构建；Windows / macOS 上根 CMake 打印“当前平台跳过”，不产生目标
（`SHOT_PLUGIN_SKIPPED`）。系统依赖：

| 依赖 | 用途 | Ubuntu 24.04 包 |
| --- | --- | --- |
| GIO / GLib（`pkg-config gio-2.0`） | GDBus、GMainContext、GMainLoop | `libglib2.0-dev` |
| pkg-config | 找 GLib 的 include/lib | `pkg-config` |
| nlohmann-json | 只有 i18n 文案表解析用到 | 由仓库的 vcpkg 清单提供 |

```bash
sudo apt install libglib2.0-dev pkg-config
# 宿主仓库内整体构建（Linux）：
cmake --preset release -DVCPKG_TARGET_TRIPLET=x64-linux
cmake --build --preset release --target session_control
```

产物：`build/Linux/bin/plugins/session_control.so`。

## 8. 目录

```text
plugins/session_control/
├── CMakeLists.txt                  # 仅 Linux；pkg-config gio-2.0 + nlohmann-json
├── metadata.json                   # session_control.main，接口 shot.configurable
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── session_control_config.h    # enabled 配置项（public）
│   ├── session_control_plugin.h    # 插件主体：配置回调 + 启停编排（public）
│   ├── control_service.h           # D-Bus 服务门面（public；不暴露 GLib 类型）
│   └── control_service_impl.h      # 内部头：专用线程 / GMainContext / GDBus 句柄
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 + kEntries + ABI 胶水
    ├── session_control_config.cpp  # enabled 的声明 / 解析 / 线程安全读写
    ├── session_control_plugin.cpp  # 文案表 + enabled 变化时启停服务
    ├── control_service.cpp         # 线程生命周期 / g_bus_own_name / 关闭顺序
    └── control_service_dispatch.cpp# introspection XML + 方法 → 事件映射 + 回复
```

## 9. 安全与已知限制

- 会话总线默认只允许**同一用户**的进程访问；本服务没有额外鉴权。任何以该用户身份运行的
  程序都可以调用这些方法（与桌面快捷键等价）。不需要时在设置页关掉 `enabled`。
- 不提供“截图已完成/失败”的同步结果：方法返回只表示事件已入队，结果走
  `capture.done` / `capture.failed` 事件（由其它插件消费）。
- 会话总线断开（dbus 重启、注销）后：本插件只打 WARN 并停止提供入口，不会结束宿主进程
  （连接上的 `exit-on-close` 已显式关掉）；进程缓存的会话连接可能已经失效，恢复办法是重启宿主。
- Wayland / X11 下 `CaptureRegion` 依赖 Linux 区域选择覆盖层（已在 Ubuntu 24.04 X11 /
  Ubuntu 26 Wayland 真机验证）；覆盖层不可用时该调用仍只会发布“请开始框选”并立即返回成功。
- 本插件不解决“应用没有权限读屏幕”的问题：Wayland 下截图仍受合成器协议/portal 约束，
  取决于 Linux 采集插件的实现。
