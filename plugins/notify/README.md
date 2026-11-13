# notify —— 截屏结果消息提示插件（`shot.configurable`）

无主窗口架构下的“截屏结果通知”：插件不采集、不保存、不弹自己的窗口，只订阅
`capture.done` / `capture.failed`，按平台显示系统通知，告诉用户保存路径或失败原因。
**截屏结果提示（成功 + 失败）统一由本插件负责**；托盘插件只管图标与菜单，不再订阅
截屏结果。

| 平台 | 通知后端 | 线程/窗口 |
| --- | --- | --- |
| Windows | 隐藏占位托盘图标 + `Shell_NotifyIconW(NIF_INFO)` 气泡（Win10/11 上是系统通知） | 插件自己的通知线程 + 隐藏消息窗口 |
| Linux | `libnotify`（`org.freedesktop.Notifications` 的 D-Bus 客户端） | 无窗口、无通知线程，在宿主 main 线程同步提交 |
| macOS | 未实现，CMake 跳过 | —— |

本插件只声明 `shot.configurable`（没有 `ui_provider`）：宿主调 `create(index, host)`
创建实例、`destroy` 卸载实例，中间没有 `init/deinit/post_to_plugin`。因此订阅与
通知后端都在 `create` 路径建立、在 `destroy` 路径收尾。

## 1. 目录

```
plugins/notify/
├── CMakeLists.txt                  # 平台源文件/库选择 + shot_add_plugin + nlohmann-json
├── metadata.json                   # 插件清单（id notify.main，接口 shot.configurable）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── notify_config.h             # NotifyConfig：4 个配置项声明与当前值（shot.configurable）
│   ├── notify_toast.h              # NotifyToast 公共接口（PIMPL，不含平台类型）
│   ├── notify_toast_impl.h         # 内部头：Windows 的隐藏窗口/图标/事件队列
│   └── notify_toast_linux_impl.h   # 内部头：Linux 的 libnotify 状态与事件分发
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + ABI 胶水 + 配置回调 + 语言表
    ├── notify_config.cpp           # 配置项声明表（label/tooltip 是 i18n key）/ 值读写
    ├── notify_toast.cpp            # Windows 生命周期：订阅 capture.* / app.quit，起停通知线程
    ├── notify_toast_window.cpp     # Windows 隐藏窗口 + 占位图标 + 按当前语言拼气泡文本并发送
    └── notify_toast_linux.cpp      # Linux libnotify：订阅 capture.done / capture.failed 并提交通知
```

公共头 `notify_toast.h` 不包含 `<windows.h>` 或 `<libnotify/notify.h>`：`HWND` /
`HICON` / `NotifyNotification` 等类型只出现在内部头与对应平台的 `.cpp` 里
（STYLE.md §6.8）。

## 2. 构建

宿主仓库内构建（`shot_add_plugin()` 由根工程提供）：

```bash
# Windows
bash tools/verify/build.sh --target notify
# Linux
cmake --build build/Linux --target notify
```

或直接走 CMake：

```bash
cmake --build build/Windows --target notify
```

离线 SDK 独立构建：

```bash
cmake -S plugins/notify -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

产物：`<构建树>/bin/plugins/notify.dll` 或 `notify.so`。依赖：

| 平台 | 依赖 | 用途 / 系统包 |
| --- | --- | --- |
| 通用 | `nlohmann-json` | 解析 `capture.done` / `capture.failed` 的 JSON 负载；header-only |
| Windows | `user32` | 隐藏消息窗口 / 消息循环 |
| Windows | `shell32` | `Shell_NotifyIconW`（通知气泡） |
| Linux | `libnotify`（pkg-config 包名 `libnotify`） | 桌面通知客户端；Ubuntu 24.04：`sudo apt install libnotify-dev` |
| Linux | `pkg-config` | CMake 用 `find_package(PkgConfig REQUIRED)` + `pkg_check_modules` 探测上述库 |

Linux 用系统包而不是 vcpkg：libnotify 是桌面会话的一部分，必须和当前发行版的
glib / D-Bus 版本一致（`CMakeLists.txt` 里 `pkg_check_modules(LIBNOTIFY REQUIRED
libnotify)`，缺包时 configure 直接失败）。

## 3. 能力与事件

- **能力**：`shot.configurable`（v1，4 个 BOOL 配置项）。没有 `ui_provider`，所以宿主
  不会给本插件投 `post_to_plugin` 事件；`host.shutdown` 收不到。Windows 用 `app.quit`
  提前收通知线程，`destroy` 路径仍会 `stop()`；Linux 没有线程，不订阅 `app.quit`。
- **订阅**（事件总线，回调在宿主 main 线程）：

| 主题 | 负载 | 处理 |
| --- | --- | --- |
| `capture.done` | `{"path","width","height","processors"}` | 显示「截屏成功 / 已保存到 `<path>`」；Windows 长路径保留头尾、中间用 `…` 截断，Linux 交给桌面通知服务自行折行/截断 |
| `capture.failed` | `{"stage","reason"}` | 显示「截屏失败 / 截图失败：`<reason>`」 |
| `app.quit` | 空 | 仅 Windows：通知线程退出（等宿主卸载流程的 `destroy` 兜底） |

- **发布**：无。本插件只读事件，不发布任何事件，也不调用其它插件。
- 通知标题/正文按当前界面语言显示（见 §4.1）。`capture.done` 的 `path` 与
  `capture.failed` 的 `reason` 是宿主/保存插件的原文（`reason` 已由宿主本地化），
  本插件原样展示，不做二次翻译。

### 3.1 为什么 Windows 需要隐藏的占位托盘图标（实测依据）

`Shell_NotifyIconW` 的通知（`NIF_INFO`）必须挂在某个已添加的托盘图标上。本机
（Windows 11 26H2）用临时探针实测两种写法：

| 写法 | `NIM_MODIFY(NIF_INFO)` 返回值 | 用户可见通知 |
| --- | --- | --- |
| 不做 `NIM_ADD`，直接 `NIM_MODIFY` | **失败**（`0`，E_FAIL） | 无 |
| `NIM_ADD` 普通图标 | 成功 | 有（但通知区域多一个图标） |
| `NIM_ADD` + `NIF_STATE`/`NIS_HIDDEN` | 成功 | **有，且通知区域不显示图标**（本插件采用） |

所以 Windows 后端在通知区域添加一个 `NIS_HIDDEN` 的隐藏占位图标作为气泡挂载点；它
对用户不可见，也不会变成第二个托盘图标。`explorer.exe` 重启后 Shell 会丢掉图标，
通知线程监听 `TaskbarCreated` 广播消息并重新添加。

**Linux 不需要图标或窗口**：libnotify 直接通过 D-Bus 调桌面通知守护进程
（GNOME Shell / KDE Plasma / dunst 等），没有“先挂一个图标”的限制，也没有
`TaskbarCreated` 之类的 Shell 重启问题。

## 4. 配置项

| key | kind | 默认 | 说明 |
| --- | --- | --- | --- |
| `enabled` | BOOL | `1` | 消息提示总开关；关掉后成功/失败都不弹 |
| `show_on_success` | BOOL | `1` | 成功时提示「已保存到 `<path>`」 |
| `show_on_error` | BOOL | `1` | 失败时提示「截图失败：`<reason>`」 |
| `sound` | BOOL | `1` | 提示音：Windows 用 `NIIF_NOSOUND` 静音；Linux 只是给桌面通知服务的 hint，见 §4.2 |

所有值一律字符串（`"1"` / `"0"`），存盘键是 `notify.main/<key>`；本插件只负责
get/set，落盘与回灌归宿主（`document/SETTINGS.md` §2）。只把 `"0"` / `"false"` 当关，
其它写法按开处理（与 tray / clipboard 的约定一致）。

配置项的 `label` / `tooltip` 不是最终显示文本，而是 i18n key（`config.<配置键>.label` /
`config.<配置键>.tooltip`）：宿主把 key 原样放进 `config_list`，设置页用宿主生成的
`i18n_list` 解析成当前语言，查不到时显示 key 本身。文案表见 §4.1。

**与托盘的分工**：截屏结果提示（`capture.done` / `capture.failed`）**全部由本插件
负责**——成功、失败都在这里弹系统通知；托盘插件只管托盘图标与右键菜单，不再订阅
截屏结果、也不弹“截屏成功/失败”气泡。

### 4.1 多语言（i18n）

> 语言表同时包含插件自身的 `plugin.name` / `plugin.description`：设置页的插件分区标题按
> `display(插件 id, "plugin.name")` 解析（查不到时回退成 key 本身）。

用户可见文案全部走 `shot::I18n`（`include/shot_plugin_helpers/shot_i18n.h`），
代码里只保留中文 fallback，不再把中文当作唯一显示文本：

- **语言表**：`src/plugin_entry.cpp` 的 `kLangTables`，目前 `zh-CN` / `en-US` 两张
  JSON，每张表用 `@meta` 标语言；两表 key 集必须一致（每个 key 两种语言齐全）。
- **生命周期**：`create(index, host)` 是唯一能拿到宿主服务表的时机，在这里
  `std::make_unique<shot::I18n>(host, "notify.main")` 并 `loadTables(...)`；通知后端
  启动前加载完，`destroy` 里先 `stop()` 再 `i18n.reset()`（析构会退订 `lists.changed`）。
- **取值**：通知在提交时调 `i18n.tr(key, 中文原文)`。没有语言表（旧宿主 / 初始化
  失败）时返回 fallback，界面保持中文；语言切换后，下一条通知按新语言显示。
- **不翻译**：`shot::Logger` 日志固定中文（e2e 按中文日志断言）；事件主题、配置键、
  `NIF_INFO` / `NIIF_NOSOUND` 等技术值保持原样；`capture.done` 的 `path` 与
  `capture.failed` 的 `reason` 原样展示（`reason` 已由宿主本地化），只有 `reason`
  整个缺失时才用 `toast.reason_unknown` 补一句。

语言表 key 一览（共 17 个，`zh-CN` / `en-US` 一一对应）：

| key | zh-CN | en-US |
| --- | --- | --- |
| `config.enabled.label` | 消息提示 | Notifications |
| `config.enabled.tooltip` | 截屏成功后弹出系统通知显示保存路径；关掉后本插件的所有提示都不弹 | Show the saved path in a system notification; off disables all notifications |
| `config.show_on_success.label` | 截屏成功时提示 | Notify on success |
| `config.show_on_success.tooltip` | 成功时显示"已保存到 <文件路径>"（路径过长会截断中间部分） | Show "Saved to <path>" on success (long paths are truncated) |
| `config.show_on_error.label` | 截屏失败时提示 | Notify on failure |
| `config.show_on_error.tooltip` | 失败时显示"截图失败：<原因>"；截屏结果提示统一由本插件负责（托盘不弹截屏气泡） | Show "Capture failed: <reason>" (the tray does not show balloons) |
| `config.sound.label` | 提示音 | Sound |
| `config.sound.tooltip` | 通知气泡是否播放系统提示音；关掉后使用 NIIF_NOSOUND 静音 | Play the system sound for the notification; off uses NIIF_NOSOUND |
| `toast.success_title` | 截屏成功 | Capture saved |
| `toast.failed_title` | 截屏失败 | Capture failed |
| `toast.saved_prefix` | `"已保存到 "`（末尾空格） | `"Saved to "` |
| `toast.failed_prefix` | `"截图失败："` | `"Capture failed: "` |
| `toast.no_path` | 已保存（保存插件未回报路径） | Saved (the saver did not report a path) |
| `toast.path_unavailable` | （路径无法显示） | (path unavailable) |
| `toast.reason_unknown` | 原因未知 | Unknown reason |
| `toast.window_title` | Plugin-shot 消息提示 | Plugin-shot notifications |
| `toast.icon_tooltip` | Plugin-shot 消息提示（隐藏占位） | Plugin-shot notifications (hidden placeholder) |

`toast.window_title` / `toast.icon_tooltip` 只用于 Windows 隐藏消息窗口和隐藏占位
图标的排障，用户正常情况下看不到；Linux 不使用这两个 key，但仍保留在语言表里，
避免按平台裁剪 key 集导致两表不一致。

### 4.2 Linux 上的 `sound`：无效 / 只是 hint

libnotify **没有播放声音的接口**，本插件也**不 spawn** `paplay` / `canberra-gtk-play`
等外部播放器。Linux 上：

- `sound=1`：附加 `sound-name` hint（成功 `message-new-instant`、失败
  `dialog-warning`）；
- `sound=0`：附加 `suppress-sound` hint；
- 两个 hint 都只是 freedesktop 通知规范里的**建议**，是否播放、播什么由桌面通知
  守护进程决定（GNOME/KDE 可能播放，dunst 等可能直接忽略）。所以 **Linux 上不保证
  有声，也不保证静音**；README 与设置页 tooltip 里的 `NIIF_NOSOUND` 是 Windows 说法。

## 5. 生命周期与线程

| 时机 | Windows | Linux |
| --- | --- | --- |
| `create(index, host)` | 分配状态、加载语言表、订阅 `capture.done` / `capture.failed` / `app.quit`、起通知线程；线程就绪前到达的事件先入队 | 分配状态、加载语言表、`notify_init`、订阅 `capture.done` / `capture.failed`；无后台线程 |
| 事件到达 | 总线回调只「拷负载 + `PostMessage` 唤醒」；通知线程出队后按配置弹气泡 | 总线回调在 main 线程直接解析并 `notify_notification_show`；没有跨线程队列 |
| `capture.done` / `capture.failed` | 通知线程出队，检查 `enabled` / `show_*`，拼文本后 `Shell_NotifyIconW` | main 线程检查 `enabled` / `show_*`，拼文本后提交 D-Bus 通知 |
| `app.quit` | 通知线程退出消息循环、删隐藏图标、销毁窗口、注销窗口类 | 不订阅（没有线程可收） |
| `destroy` | 先退订事件总线，再让通知线程退出并 `join`，然后 `reset` 语言表、释放状态；重复调用是空操作 | 先退订事件总线，再 `notify_uninit`，然后 `reset` 语言表、释放状态；重复调用是空操作 |

Windows 线程边界：事件总线回调运行在宿主分发线程，只做拷贝与唤醒；`Shell_NotifyIconW`、
窗口创建与消息循环全部在插件自己的通知线程上执行（Shell 通知不是线程安全的）。
窗口类属于进程、不随 DLL 卸载消失，退出前显式 `UnregisterClassW`，避免下次
（禁用→启用）重新加载时窗口过程指针悬空（tray 插件踩过的 `0xC000041D` 教训）。

Linux 线程边界：libnotify 的 `notify_notification_show` 是一次短 D-Bus 调用，不依赖
窗口类或线程亲和性，所以不需要线程；所有 libnotify 对象都在同一次回调里创建、
提交、`g_object_unref`，`destroy` 之后没有指向本 DLL 的回调或全局对象。

## 6. 降级行为

| 情况 | 行为 |
| --- | --- |
| 宿主没有事件总线（老宿主） | 记一条 WARN，不初始化后端、不订阅；插件照常加载，消息提示不可用 |
| 负载不是合法 JSON / 缺字段 | `capture.done` 缺 `path` 显示通用「已保存（保存插件未回报路径）」；`capture.failed` 缺 `reason` 显示「截图失败：原因未知」；解析失败只记 WARN |
| Windows：隐藏占位图标添加失败（Shell 忙等） | `create` 路径按初始化失败处理（本插件不可用，宿主继续运行）；`TaskbarCreated` 重加失败只 WARN，等下一次 Shell 重启再试 |
| Windows：`Shell_NotifyIconW(NIM_MODIFY)` 失败 | 记一条 WARN，不中断宿主/下一次通知 |
| Windows：用户开了系统“专注助手” | `NIIF_RESPECT_QUIET_TIME` 让系统决定不打扰；Shell 仍返回成功 |
| Windows：系统通知被用户关闭 / 任务栏浮层在前台 | Shell 仍返回成功但用户可能看不到（实测：任务栏溢出浮层在前台时 toast 不显示）；插件只记日志，不改系统设置 |
| Linux：没有 D-Bus 会话 / `notify_init` 失败 | WARN 一次，插件保留但通知不可用；截屏与其它插件不受影响 |
| Linux：没有通知守护进程 / 用户关了通知 / `notify_notification_show` 失败 | 记 WARN（带 `GError` 文案），释放 `GError` 与通知对象；不影响下一次截图 |
| Linux：桌面忽略 `sound-name` / `suppress-sound` | 通知照常显示，只是声音行为由桌面决定（§4.2） |
| 配置关闭（`enabled` / `show_*`） | 跳过对应通知，只记 debug 日志 |
| 宿主没提供 `shot.configurable` 的渲染/回灌 | 配置用默认值（全开），插件功能不受影响 |
| 旧宿主 / 没有 `i18n_list` | `shot::I18n` 安静降级：`tr` 用第一张表（zh-CN），界面保持中文，通知不受影响 |
| 语言表加载失败 / 非法 JSON | 只记 WARN，`tr` 返回中文 fallback，通知照常显示 |
| 当前语言缺某个 key | 按回退链取另一种语言，最终返回 fallback（中文原文）；两表 key 数不一致只 WARN |

## 7. 平台支持

| 平台 | 状态 |
| --- | --- |
| Windows（MSVC x64） | ✅ 已验证：构建通过（/W4 无警告）；隐藏占位图标 + `NIF_INFO` 气泡在 Windows 11 26H2 真机显示成功（探针 + 假宿主 harness 截图证据在本地 `build/verify/notify_*`） |
| Linux（Wayland / X11 会话） | 🟡 代码已接入 CMake（`pkg_check_modules(LIBNOTIFY REQUIRED libnotify)`），使用 libnotify / `org.freedesktop.Notifications`；**未在本机 Ubuntu 24.04 桌面真机验证**，需要安装 `libnotify-dev` 并跑一次通知 e2e |
| macOS | **未实现**：CMake 在 APPLE 上跳过整个插件；候选方案记在 `CMakeLists.txt` 的 `# TODO:`（`UNUserNotificationCenter`），当前本插件没有 macOS 实现 |

## 8. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 自己采集/保存/处理图像 | 那是 `screen_capture` / `image_saver` / image_processor 的职责；本插件只读结果事件 |
| 使用 Windows Toast（WinRT） | 需要 COM/WinRT 初始化与打包身份（AppUserModelID），复杂度远高于 `NIF_INFO`；实测隐藏占位图标方案在本机可用 |
| 在通知里放缩略图/按钮 | 像素不进事件总线（`document/CAPTURE.md` §3）；Windows `NIF_INFO` 只支持标题+正文，Linux 的 actions 也没有跨桌面的一致 UI |
| 常驻可见托盘图标 | 用户已经有托盘插件；Windows 只需要一个不可见的挂载点，Linux 完全不需要 |
| 队列/去重/节流 | 一次截屏只发一条 `capture.done` 或 `capture.failed`；连发多次时的系统通知叠层由平台自己处理 |
| Linux 自己 spawn 声音播放器 | 桌面通知服务负责声音；插件只传 hint，避免多一个外部进程依赖与生命周期问题 |
| 本插件读写宿主全局配置 | 配置项走 `shot.configurable`，由宿主统一存盘与回灌 |

## 9. 检查清单

- [ ] 只声明了 `shot.configurable`，没有偷偷调其它插件/宿主私有接口？
- [ ] 配置值一律用字符串、键名不与其它插件冲突？
- [ ] Windows 事件总线回调里只做入队与唤醒，没有直接碰 `Shell_NotifyIconW` / Win32？
- [ ] Windows `destroy` 返回前退订、停线程、删隐藏图标、注销窗口类？
- [ ] Linux 订阅了 `capture.done` 和 `capture.failed` 两条，而不是只订上一条？
- [ ] Linux 每个 `NotifyNotification*` 都 `g_object_unref`、每个 `GError*` 都 `g_error_free`？
- [ ] Linux `destroy` 返回前退订事件总线并 `notify_uninit`，没有留下 D-Bus 回调？
- [ ] 负载缺字段 / 不是合法 JSON 时安静降级，不崩？
- [ ] 平台类型仍然只出现在内部头与对应平台 `.cpp` 里（公共头不含 `<windows.h>` / `<libnotify/notify.h>`）？
- [ ] Windows 气泡文案超长时中间截断，不溢出 `NOTIFYICONDATAW` 的定长缓冲？
- [ ] Linux 的 `sound` 只作为 hint 处理，README 写清“不保证有声/静音”？
- [ ] 新增/修改的界面文案在 `zh-CN` / `en-US` 两张语言表里都补齐，且 `tr` 的 fallback 是中文原文？
- [ ] 日志、事件主题、配置键没有跟着翻译？
- [ ] 改了事件主题/负载？→ 同步 `document/CAPTURE.md` §3 与 `document/EVENT_BUS.md` §4
