# notify —— 截屏结果消息提示插件（`shot.configurable`）

无主窗口架构下的"截屏结果通知"：插件不采集、不保存、不弹自己的窗口，只订阅
`capture.done` / `capture.failed`，用 Shell 通知气泡（Windows 10/11 上是系统通知）
告诉用户保存路径或失败原因。**截屏结果提示（成功 + 失败）统一由本插件负责**；
托盘插件只管图标与菜单，不再订阅截屏结果。

本插件只声明 `shot.configurable`（没有 `ui_provider`）：宿主调 `create(index, host)`
创建实例、`destroy` 卸载实例，中间没有 `init/deinit/post_to_plugin`。因此订阅与
通知线程都在 `create` 路径建立、在 `destroy` 路径收尾；`app.quit` 事件用于提前
退出，`destroy` 是最后的兜底（线程模型见 `document/EVENT_BUS.md`）。

## 1. 目录

```
plugins/notify/
├── CMakeLists.txt                  # shot_add_plugin + nlohmann-json / user32 / shell32
├── metadata.json                   # 插件清单（id notify.main，接口 shot.configurable）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── notify_config.h             # NotifyConfig：4 个配置项声明与当前值（shot.configurable）
│   ├── notify_toast.h              # NotifyToast 公共接口（PIMPL，不含 Win32 类型）
│   └── notify_toast_impl.h         # 内部头：NotifyToast::Impl、隐藏窗口/图标/事件队列
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + ABI 胶水 + 配置回调
    ├── notify_config.cpp           # 配置项声明表 / 字符串值解析 / 当前值读写
    ├── notify_toast.cpp            # 生命周期：订阅 capture.* / app.quit，起停通知线程
    └── notify_toast_window.cpp     # 隐藏消息窗口 + 隐藏占位托盘图标 + 气泡文本与 Shell 通知
```

公共头 `notify_toast.h` 不包含 `<windows.h>`：`HWND` / `HICON` 等 Win32 类型只出现在
内部头与窗口相关 `.cpp` 里（STYLE.md §6.8）。

## 2. 构建

宿主仓库内构建（`shot_add_plugin()` 由根工程提供）：

```bash
bash build/verify/locked_build.sh --target notify
```

或直接走 CMake：

```bash
cmake -S . -B build/Windows -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows --target notify
```

离线 SDK 独立构建：

```bash
cmake -S plugins/notify -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

产物：`<构建树>/bin/plugins/notify.dll`（仓库内整体构建时是
`build/Windows/bin/plugins/notify.dll`）。依赖（都由 vcpkg 清单模式提供，插件不链接
spdlog）：

| 依赖 | 用途 | 说明 |
| --- | --- | --- |
| `nlohmann-json` | 解析 `capture.done` / `capture.failed` 的 JSON 负载 | header-only，不引入运行库 |
| `user32` | 隐藏消息窗口 / 消息循环 | 系统库 |
| `shell32` | `Shell_NotifyIconW`（通知气泡） | 系统库 |

## 3. 能力与事件

- **能力**：`shot.configurable`（v1，4 个 BOOL 配置项）。没有 `ui_provider`，所以宿主
  不会给本插件投 `post_to_plugin` 事件；`host.shutdown` 收不到，用 `app.quit` 事件
  提前收线程，`destroy` 路径仍会 `stop()`。
- **订阅**（事件总线，回调在宿主 main 线程）：

| 主题 | 负载 | 处理 |
| --- | --- | --- |
| `capture.done` | `{"path","width","height","processors"}` | 显示「截屏成功 / 已保存到 `<path>`」，长路径保留头尾、中间用 `…` 截断 |
| `capture.failed` | `{"stage","reason"}` | 显示「截屏失败 / 截图失败：`<reason>`」 |
| `app.quit` | 空 | 通知线程退出（等宿主卸载流程的 `destroy` 兜底） |

- **发布**：无。本插件只读事件，不发布任何事件，也不调用其它插件。

### 3.1 为什么需要隐藏的占位托盘图标（实测依据）

`Shell_NotifyIconW` 的通知（`NIF_INFO`）必须挂在某个已添加的托盘图标上。本机
（Windows 11 26H2）用临时探针实测两种写法：

| 写法 | `NIM_MODIFY(NIF_INFO)` 返回值 | 用户可见通知 |
| --- | --- | --- |
| 不做 `NIM_ADD`，直接 `NIM_MODIFY` | **失败**（`0`，E_FAIL） | 无 |
| `NIM_ADD` 普通图标 | 成功 | 有（但通知区域多一个图标） |
| `NIM_ADD` + `NIF_STATE`/`NIS_HIDDEN` | 成功 | **有，且通知区域不显示图标**（本插件采用） |

所以本插件在通知区域添加一个 `NIS_HIDDEN` 的隐藏占位图标作为气泡挂载点；它对用户
不可见，也不会变成第二个托盘图标。`explorer.exe` 重启后 Shell 会丢掉图标，通知线程
监听 `TaskbarCreated` 广播消息并重新添加。

## 4. 配置项

| key | kind | 默认 | 说明 |
| --- | --- | --- | --- |
| `enabled` | BOOL | `1` | 消息提示总开关；关掉后成功/失败都不弹 |
| `show_on_success` | BOOL | `1` | 成功时提示「已保存到 `<path>`」 |
| `show_on_error` | BOOL | `1` | 失败时提示「截图失败：`<reason>`」 |
| `sound` | BOOL | `1` | 气泡是否播放系统提示音（关掉时用 `NIIF_NOSOUND` 静音） |

所有值一律字符串（`"1"` / `"0"`），存盘键是 `notify.main/<key>`；本插件只负责
get/set，落盘与回灌归宿主（`document/SETTINGS.md` §2）。只把 `"0"` / `"false"` 当关，
其它写法按开处理（与 tray / clipboard 的约定一致）。

**与托盘的分工**：截屏结果提示（`capture.done` / `capture.failed`）**全部由本插件
负责**——成功、失败都在这里弹系统通知；托盘插件只管托盘图标与右键菜单，不再订阅
截屏结果、也不弹"截屏成功/失败"气泡。托盘如果保留自己的错误提示，只用于托盘自身
的错误（例如图标加载失败），不与本插件的截屏结果提示重复。

## 5. 生命周期与线程

| 时机 | 插件做什么 |
| --- | --- |
| `create(index, host)` | 分配状态、订阅 `capture.done` / `capture.failed` / `app.quit`、起通知线程；线程就绪前到达的事件先入队 |
| 通知线程启动 | 注册并创建**隐藏消息窗口**（`PluginShotNotifyWnd`）→ 添加隐藏占位托盘图标 → 握手 → 消息循环 |
| `capture.done` / `capture.failed` | 总线回调只「拷负载 + `PostMessage` 唤醒」；通知线程出队后按配置弹气泡 |
| explorer 重启（`TaskbarCreated`） | 重新添加隐藏占位图标 |
| `app.quit` | 通知线程退出消息循环、删隐藏图标、销毁窗口、注销窗口类 |
| `destroy` | 先退订事件总线，再让通知线程退出并 `join`，最后释放状态；重复调用是空操作 |

线程边界：事件总线回调运行在宿主分发线程，只做拷贝与唤醒；`Shell_NotifyIconW`、
窗口创建与消息循环全部在插件自己的通知线程上执行（Shell 通知不是线程安全的）。
窗口类属于进程、不随 DLL 卸载消失，退出前显式 `UnregisterClassW`，避免下次
（禁用→启用）重新加载时窗口过程指针悬空（tray 插件踩过的 `0xC000041D` 教训）。

## 6. 降级行为

| 情况 | 行为 |
| --- | --- |
| 宿主没有事件总线（老宿主） | 记一条 WARN，不通知线程、不订阅；插件照常加载，消息提示不可用 |
| 负载不是合法 JSON / 缺字段 | `capture.done` 缺 `path` 显示通用「已保存（保存插件未回报路径）」；`capture.failed` 缺 `reason` 显示「截图失败：原因未知」；解析失败只记 WARN |
| 隐藏占位图标添加失败（Shell 忙等） | `create` 路径按初始化失败处理（本插件不可用，宿主继续运行）；`TaskbarCreated` 重加失败只 WARN，等下一次 Shell 重启再试 |
| `Shell_NotifyIconW(NIM_MODIFY)` 失败 | 记一条 WARN，不中断宿主/下一次通知 |
| 配置关闭（`enabled` / `show_*`） | 跳过对应气泡，只记 debug 日志 |
| 用户开了系统"专注助手" | `NIIF_RESPECT_QUIET_TIME` 让系统决定不打扰；Shell 仍返回成功 |
| 系统通知被用户关闭 / 任务栏浮层在前台 | Shell 仍返回成功但用户可能看不到（实测：任务栏溢出浮层在前台时 toast 不显示）；插件只记日志，不改系统设置 |
| 宿主没提供 `shot.configurable` 的渲染/回灌 | 配置用默认值（全开），插件功能不受影响 |

## 7. 平台支持

| 平台 | 状态 |
| --- | --- |
| Windows（MSVC x64） | ✅ 已验证：构建通过（/W4 无警告）；隐藏占位图标 + `NIF_INFO` 气泡在 Windows 11 26H2 真机显示成功（探针 + 假宿主 harness 截图证据在本地 `build/verify/notify_*`） |
| Linux / macOS | **未实现**：CMake 在非 Windows 平台跳过整个插件；各平台的候选方案记在 `CMakeLists.txt` 的 `# TODO:` 注释里（macOS 用本地通知框架、Linux 走桌面通知 DBus），当前本插件只有 Windows 实现 |

## 8. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 自己采集/保存/处理图像 | 那是 `screen_capture` / `image_saver` / image_processor 的职责；本插件只读结果事件 |
| 使用 Windows Toast（WinRT） | 需要 COM/WinRT 初始化与打包身份（AppUserModelID），复杂度远高于 `NIF_INFO`；实测隐藏占位图标方案在本机可用 |
| 在通知里放缩略图/按钮 | 像素不进事件总线（`document/CAPTURE.md` §3）；`NIF_INFO` 也只支持标题+正文 |
| 常驻可见托盘图标 | 用户已经有托盘插件；本插件只需要一个不可见的挂载点 |
| 队列/去重/节流 | 一次截屏只发一条 `capture.done` 或 `capture.failed`；连发多次时的系统通知叠层由 Windows 自己处理 |
| 本插件读写宿主全局配置 | 配置项走 `shot.configurable`，由宿主统一存盘与回灌 |

## 9. 检查清单

- [ ] 只声明了 `shot.configurable`，没有偷偷调其它插件/宿主私有接口？
- [ ] 配置值一律用字符串、键名不与其它插件冲突？
- [ ] 事件总线回调里只做入队与唤醒，没有直接碰 `Shell_NotifyIconW` / Win32？
- [ ] `destroy` 返回前退订、停线程、删隐藏图标、注销窗口类？
- [ ] 负载缺字段 / 不是合法 JSON 时安静降级，不崩？
- [ ] 平台类型仍然只出现在内部头与窗口相关 `.cpp` 里（公共头不含 `<windows.h>`）？
- [ ] 气泡文案超长时中间截断，不溢出 `NOTIFYICONDATAW` 的定长缓冲？
- [ ] 改了事件主题/负载？→ 同步 `document/CAPTURE.md` §3 与 `document/EVENT_BUS.md` §4
