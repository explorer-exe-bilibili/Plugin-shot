# 跨平台支持矩阵（Windows / Linux Wayland / Linux X11 / macOS）

这份文档回答一个问题：**这个仓库在某个平台上到底“能不能用”**，而不是“能不能编译”。
仓库是 Windows-first：**Windows 11、Ubuntu 24.04（X11）与 Ubuntu 26（Wayland）三台真机**本轮
都已跑通核心截图链路（采集 → 处理 → 保存 → 剪贴板/通知），设置页、热键（X11）/ D-Bus 触发
（Wayland）、托盘与语言包也已验证——**Linux 的结论只覆盖单显示器**。Linux 的**默认目标是
Wayland**（本轮实测环境：Ubuntu 26 / Wayland 会话），X11 保留为**第二优先**并已在
Ubuntu 24.04 / X11 会话验证。macOS 从未真机运行，也没有成功的构建结论。平台实现按目标平台在
各自 `CMakeLists.txt` 里选择：没有实现的平台直接跳过（`SHOT_PLUGIN_SKIPPED`），不产出伪实现。
运行期由各插件按 `WAYLAND_DISPLAY` / `XDG_SESSION_TYPE` 选择 Wayland 或 X11 分支。`hotkey` 的
纯 Wayland 路径（`XOpenDisplay` 失败）不是编译期 stub，而是运行期降级：只发布列表与配置项、
不注册热键并 WARN 指向 `session_control`；macOS 等其它平台仍编 `hotkey_window_stub.cpp`
（详见 §2）。

> **2026-11-14 真机回归说明**：Windows 11、Ubuntu 24.04（X11）、Ubuntu 26（Wayland）三台真机
> 完成了本轮回归。**已验证（已升 ✅）**：Windows 全链路复测通过，并新增确认区域截图悬停
> 窗口/任务栏/屏幕四角的吸附与截窗口尺寸、长截图完整交互（选窗口 → 滚动 → HUD → 保存）、
> 通知标题/正文与点击行为（默认打开图片、可配置打开所在文件夹）及应用名短名、官方 zh-TW
> 语言包加载与界面/托盘跟随；Ubuntu 24（X11）的整屏采集（Ctrl+Alt+S / D-Bus）、`XGrabKey`
> 热键（Ctrl+Alt+A / Ctrl+Alt+S / Ctrl+Alt+O）、XComposite 背景快照框选（不再灰遮罩、覆盖层
> 高于顶栏/dock、悬停窗口吸附、悬停顶栏/dock 与屏幕四角=全屏、单击截窗口、拖框、Esc 取消、
> 确认后保存+剪贴板）、libnotify 通知（标题/正文、头部应用名正确、点击打开）、剪贴板、设置页
> （页签/插件页/枚举多语言）、语言包、D-Bus 触发与 Linux 托盘（顶栏图标/菜单）；Ubuntu 26
> （Wayland）的 portal ScreenCast 整屏采集、门户定格图拖选（确认后覆盖层立即消失、无“应用无
> 响应”、保存+剪贴板）、通知、剪贴板、D-Bus 触发、设置页、语言包与托盘。
> **仍待验证（保持 🟡/🚫）**：X11 多显示器 / 多 X screen / 负坐标；Wayland 窗口悬停（协议
> 不允许，属平台限制）；macOS 全部（含构建）；CI 的 Linux / macOS job（等下一轮结果）。
> `long_capture` 仍为 Windows-only。
> **0.1.0 只发布 Windows 构建，未验证能力不随包分发**；表格只按上述证据升级，未列出的细节
> 不要读成已验证。

**改平台相关代码、CMake 跳过条件、根 `README*.md` 的平台段、或插件清单前先读这里，
并在同一个提交里更新矩阵。**

> **核对口径**：本表只承认**能 configure/build 的插件工程**与**真机上跑过的交互**；未提交的
> 并行改动不计为平台支持。✅ 只给本轮真机回归确认过的项（见上方清单）：Windows 全链路；
> Linux 的采集 / 框选 / 热键（X11）/ D-Bus 触发（Wayland）/ 剪贴板 / 通知 / 设置页 / 语言包 /
> 托盘，**均为单显示器环境**。**X11 多显示器 / 多 X screen / 负坐标、Wayland 窗口悬停（协议
> 限制）、macOS 与 CI 的 Linux / macOS job 仍不升 ✅**，详见 §3.1、§3.2、§3.4 与 §4。

## 1. 记号

| 记号 | 含义 | 判定标准 |
| --- | --- | --- |
| ✅ | 真机验证 | 在对应平台真机上跑过对应功能（当前有 Windows 11、Ubuntu 24.04 X11、Ubuntu 26 Wayland 三台真机的证据），有日志、落盘文件或截图证据 |
| 🟡 | 未真机验证 | 有代码、能编译，但没在目标平台真机上跑过。格子里要写清实现到哪一步（例如“已实现、编译通过，真机交互未验证”），不能读成已可用 |
| ⛔ | 未实现（构建跳过） | 该平台不构建这个插件（`if(NOT WIN32)` / `if(APPLE)` 等 → `SHOT_PLUGIN_SKIPPED`），或目标平台上根本没有对应实现 |
| 🚫 | 平台限制 | 平台机制不允许，或必须依赖非标准协议、用户授权、桌面扩展；不是“补一个函数”就能解决 |

单元格里写的都是**理由**。**“CI 绿”不升级任何格**：CI 只跑 `--help`，见 §4。

## 2. 总矩阵

列的顺序就是优先级：Windows（主开发与验证平台）、Linux Wayland（**默认目标**）、
Linux X11（**第二优先**）、macOS。

| 能力 / 插件 | Windows | Linux Wayland（默认目标） | Linux X11（第二优先） | macOS |
| --- | --- | --- | --- | --- |
| 宿主核心（加载 / ABI / 事件总线 / 列表 / 配置 / CLI） | ✅ 本机全链路：插件加载与热卸载、采集/设置 e2e、`--capture`/`--settings`/`--help` 都跑过；本轮真机回归复测通过 | ✅ 与 X11 同一份二进制；插件加载/热卸载、采集/设置/通知/剪贴板/D-Bus 触发的宿主链路已在 Ubuntu 26 Wayland 真机跑通 | ✅ WSL2 构建通过，Ubuntu 24.04 X11 真机的采集/设置/热键/框选/通知/托盘链路都已跑通 | 🟡 代码有 `_NSGetExecutablePath` / `dlopen` 分支，但 macOS 从未成功 configure/build |
| 屏幕采集 `plugins/screen_capture` | ✅ `gdi_desktop_capture.cpp` 用 `GetDC`/`BitBlt`/`GetDIBits` 抓物理虚拟桌面，双屏 3584x1600 与光标合成已实测；本轮整屏采集复测通过 | ✅ 已实现并真机验证（Ubuntu 26 / Wayland / 单显示器）：`xdg-desktop-portal` ScreenCast + PipeWire 取帧（一次一个源：输出或窗口），全屏截图跑通，授权弹窗与取帧正常；多屏/HiDPI 拼接未实现（§3.1） | ✅ 已实现并真机验证（Ubuntu 24.04 / X11 / 单显示器 / 单 X screen）：`XShmGetImage` 优先、`XGetImage` 回退抓整个 X screen，XFixes 光标合成；Ctrl+Alt+S 与 D-Bus 触发的整屏截图均跑通；多显示器 / 多 X screen / 负坐标仍未验证（§3.2） | ⛔ 只有 GDI 实现；CMake 跳过，没有 ScreenCaptureKit / CGDisplayStream 分支 |
| 图像保存 `plugins/image_saver` | ✅ PNG/JPEG 落盘在 `--capture` 与多个 e2e 中验证 | ✅ 随 Wayland 真机采集链路跑通：截图与框选确认后的保存落盘正常（Ubuntu 26） | ✅ 随 X11 真机采集链路跑通：截图与框选确认后的保存落盘正常（Ubuntu 24.04） | 🟡 `output_path.cpp` 有 `$HOME/Pictures` 与 `localtime_r` 分支，但 macOS 未构建 |
| 图像处理 `plugins/example_processor` / `opencv_provider` | ✅ 两个 `image_processor` 参与采集流水线，`opencv_provider` 把 OpenCV 带进进程 | ✅ 随 Wayland 真机采集链路跑通：`.so` 被加载，真实采集帧进入处理链并落盘（Ubuntu 26） | ✅ 随 X11 真机采集链路跑通（Ubuntu 24.04） | 🟡 只用标准库/OpenCV，CMake 不跳过，但 macOS 未构建 |
| 设置页 `plugins/settings_ui` | ✅ ImGui + GLFW + OpenGL3，双屏 150%+100% DPI、跨屏拖动与几何恢复已 e2e | ✅ 已真机验证（Ubuntu 26 / Wayland）：设置页正常创建，页签 / 插件页 / 枚举多语言正确 | ✅ 已真机验证（Ubuntu 24.04 / X11）：设置页正常创建，页签 / 插件页 / 枚举多语言正确 | 🟡 Cocoa 要求窗口与事件循环在主线程，而插件用自建 UI 线程，是已知移植风险；未构建 |
| 托盘 `plugins/tray` | ✅ Win32 `Shell_NotifyIconW` + 数据驱动菜单，本机运行验证 | ✅ 已真机验证：StatusNotifierItem + `com.canonical.dbusmenu`（GDBus / 系统 `gio-2.0`），Ubuntu 顶栏图标与菜单正常；菜单来自 `tray_command_list`，`lists.changed` 后重建；需要 `org.kde.StatusNotifierWatcher`（Ubuntu GNOME 的 Ubuntu AppIndicators 扩展，Ubuntu 默认自带；纯净 GNOME 需手动安装），找不到时只 WARN 一次、不崩不重试（§3.1） | ✅ 同一份 SNI + dbusmenu 实现（D-Bus 协议与显示服务器无关），已在 Ubuntu 顶栏真机验证（§3.2） | ⛔ 没有 `NSStatusBar`/`NSStatusItem` 实现，CMake 跳过 |
| 全局热键 `plugins/hotkey` | ✅ `RegisterHotKey` + 隐藏消息窗口；`Ctrl+Alt+S` 等与 `e2e_hotkey_capture.ps1` 已验证 | ✅ 纯 Wayland 没有全局热键：运行期降级为只发布列表与配置项、不注册并 WARN 指向 `session_control`；热键替代链路（`session_control` 的 D-Bus 触发）已在 Ubuntu 26 真机验证（§3.1） | ✅ 已实现并真机验证（Ubuntu 24.04 / X11 / 单显示器）：`XGrabKey` 注册与触发跑通（Ctrl+Alt+A / Ctrl+Alt+S / Ctrl+Alt+O），锁修饰符变体、`BadAccess` 冲突 WARN、self-pipe 事件循环、suspend/resume 随链路运行；多 X screen 只抓默认 root 仍未验证。已知差异：X11 没有 `MOD_NOREPEAT`，按住会重复触发（§3.2） | ⛔ 没有 `RegisterEventHotKey`/`CGEventTap` 原生实现；macOS 编 `hotkey_window_stub.cpp`（不注册），且 macOS 从未成功构建 |
| 会话控制入口 `plugins/session_control` | ⛔ 仅 Linux 构建（GDBus 依赖桌面会话），Windows/macOS 在 `shot_add_plugin()` 之前跳过 | ✅ 已真机验证（Ubuntu 26 / Wayland）：随 Linux 构建产出 `.so`，D-Bus 触发跑通（§3.3） | ✅ 已真机验证（Ubuntu 24.04 / X11）：同一份实现，D-Bus 是会话级的，触发跑通（§3.3） | ⛔ 不构建 |
| 剪贴板 `plugins/clipboard` | ✅ `OpenClipboard` + `SetClipboardData` 写 `CF_DIB`（可选 `CF_HDROP`），`e2e_clipboard.ps1` 验证 | ✅ 已真机验证（Ubuntu 26 / Wayland）：Linux X11 selection 实现经 XWayland 桥接，截图后剪贴板有图；**纯 Wayland 且无 XWayland 时不可用**（平台限制） | ✅ 已实现并真机验证（Ubuntu 24.04 / X11）：`XSetSelectionOwner` + `SelectionRequest` 提供 `image/png` / `text/uri-list`，截图后剪贴板正常 | ⛔ 没有 `NSPasteboard` 实现，CMake 跳过 |
| 通知 `plugins/notify` | ✅ 隐藏占位图标 + `Shell_NotifyIconW(NIF_INFO)`；Windows 11 真机复测：标题/正文正常、点击默认打开图片、可配置打开所在文件夹、应用名为短名（中文系统「Plugin-shot 截图工具」） | ✅ 已真机验证（Ubuntu 26 / Wayland）：libnotify → `org.freedesktop.Notifications`，标题/正文、头部应用名与点击打开图片/文件夹正常 | ✅ 已真机验证（Ubuntu 24.04 / X11）：libnotify 标题/正文、头部应用名正确（不再是「Plugin-shot 设置」乱码）、点击打开图片/文件夹正常 | ⛔ 没有 `UNUserNotificationCenter` 实现，CMake 跳过 |
| 区域框选 `plugins/region_selector` | ✅ GLFW 覆盖层 + Win32 物理坐标；悬停窗口吸附、单击截窗、DWM 扩展边界、主题色折角已实测；本轮复测悬停窗口/任务栏/屏幕四角吸附与截窗口尺寸正确 | ✅ B 方案已实现并真机验证（Ubuntu 26 / Wayland）：门户抓一张定格图 → 自有窗口里拖选，确认后覆盖层立即消失、无“应用无响应”，保存+剪贴板正常；**没有窗口悬停吸附**（portal 不提供其它应用的窗口列表与几何，属平台限制，§3.1） | ✅ 已实现并真机验证（Ubuntu 24.04 / X11 / 单显示器 / 单 X screen）：GLFW 无边框覆盖层 + 背景快照（合成器下自动回退 XComposite 覆盖窗口，**不再灰遮罩**）+ `XQueryTree`/`_NET_WM_WINDOW_TYPE` 窗口命中；覆盖层高于顶栏/dock，悬停窗口吸附、悬停顶栏/dock（`_NET_WORKAREA` 占位）与屏幕四角=全屏、单击截窗口、拖框、Esc 取消、确认后保存+剪贴板全部跑通；与 `screen_capture` 的 X11 分支已联调。多显示器 / 多 X screen 仍未验证（§3.2） | ⛔ 没有 `NSScreen`/`CGWindowList` 覆盖层实现，CMake 跳过 |
| 外部语言包 / 翻译 `plugins/translator` | ✅ 扫描官方/用户目录、用户包覆盖官方包、坏包只 WARN，`smoke_i18n_translator.ps1` 已跑；本轮确认官方 zh-TW 语言包可加载、界面/托盘跟随 | ✅ 已真机验证（Ubuntu 26 / Wayland）：语言包加载与切换正常 | ✅ 已真机验证（Ubuntu 24.04 / X11）：语言包加载与切换正常 | 🟡 `pack_dirs.cpp` 有 `_NSGetExecutablePath` + `$HOME` 分支，但 macOS 未构建 |
| 长截图 `plugins/long_capture` | ✅ 已实现并真机验证（Windows 11）：`region.select.request` / `region.selected` 选目标窗口 → 自带 GDI 采集层连续抓帧 → 滚动 → HUD → 保存的完整交互跑通；只在保存时发一条 `capture.done`；作为实验特性随 0.1.0 分发、默认在插件列表里禁用（`plugins.disabled`），在设置页勾选启用后重启生效（§3.3） | ⛔ 未实现；计划走 ScreenCast 的**窗口流**持续取帧 + 复用 `long_capture` 的拼接核心（该核心已在 Windows 真机验证，§3.1） | ⛔ 未实现；计划 `XGetImage` 逐帧 + `XTest`/手动滚动 + 行指纹拼接（§3.2） | ⛔ 未实现；需要 `AXUIElement` 读滚动位置并模拟滚动 |

**结论**：Windows 与 Linux（Ubuntu 24.04 X11 / Ubuntu 26 Wayland，均为单显示器）的核心
“采集 → 处理 → 保存 → 消费”链路都已在真机跑通，并各自有日志/落盘/剪贴板/通知证据；
Windows 另有本轮新增的区域截图悬停吸附（窗口/任务栏/屏幕四角）与长截图完整交互证据。
Linux 宿主与全部可构建插件的 `.so`（含屏幕采集、框选、`session_control`、X11 剪贴板、libnotify
通知、StatusNotifierItem 托盘，以及带 X11 分支的 `hotkey`）均已在真机加载运行；Wayland 抓屏走
portal 授权 + ScreenCast、框选是门户定格图 B 方案、剪贴板经 XWayland 桥接，这些平台限制仍在，
但“能跑通”已由真机证据确认。**仍未验证的只有**：X11 多显示器 / 多 X screen / 负坐标；
Wayland 窗口悬停（协议不允许，属平台限制）；macOS（无构建、无真机）；CI 的 Linux / macOS job
（等下一轮结果，不随真机验证升级）。Linux 产物仍按目标发行版构建（glibc 约束见 §4.2）。

## 3. 缺口清单（要补齐需要做什么）

### 3.1 Linux Wayland（默认目标）

| 能力 | 方案 | 现状 / 阻塞点 |
| --- | --- | --- |
| 抓屏 | `org.freedesktop.portal.ScreenCast`（`CreateSession` → `SelectSources` → `Start`）拿到 PipeWire 流，再从流里取帧 | 已实现并真机验证（Ubuntu 26 / Wayland / 单显示器）：`screen_capture` 的 Wayland 路径（portal 会话 + PipeWire 接收 + BGRA8 帧缓冲），全屏截图跑通，授权弹窗与取帧正常。依赖 `libportal-dev` + `libpipewire-0.3-dev` + `libspa-0.2-dev` + `libglib2.0-dev` |
| 授权弹窗 | 每次新建 ScreenCast 会话，合成器都会弹系统授权对话框（Ubuntu / GNOME 的预期行为）；portal 的 `persist_mode`/`restore_token` 能否免弹窗取决于桌面实现与版本，**不要当既有能力** | 已实现授权等待与失败路径；Ubuntu 26 真机的正常授权链路已跑通。拒绝授权必须走 `capture.failed`，不能假装成功 |
| 全局热键 | 没有全局热键协议；改用 `plugins/session_control` 在会话总线暴露 `org.pluginshot.Control`，用户在 GNOME「自定义快捷键」里绑一条 `gdbus call`，桌面负责按键 | `hotkey` 的 Linux 实现是 X11 `XGrabKey`；纯 Wayland（`XOpenDisplay` 失败）运行期降级为发布列表与配置项、不注册，启动 WARN 指向本入口；`session_control` 已实现，D-Bus 触发已在 Ubuntu 26 真机验证；绑定步骤见 `plugins/session_control/README.md` |
| 区域框选（B 方案） | 先用门户抓一张整屏定格图 → 在自己的窗口里显示这张图 → 用户拖选 → 按选区裁像素 | 已实现并真机验证（Ubuntu 26 / Wayland）：门户 Screenshot + 自有覆盖层拖选，确认后覆盖层立即消失、无“应用无响应”，保存+剪贴板正常；确认后先 `glfwPollEvents` 再编码，裁剪/PNG 编码/写盘在工作线程；portal 等待最多 30s、每 5s 一条进度日志；失败分级（用户取消静默、其它失败记 WARN）；显示后校验 `GLFW_VISIBLE`。**没有窗口悬停吸附**（🚫）：portal 不提供其它应用的窗口列表与几何。窗口位置与大小由合成器决定，做不成全屏遮罩，只能“先看图、再拖选” |
| 长截图 | ScreenCast 的 `SelectSources` 选**窗口**源持续取帧，在内存里按行指纹拼接；拼接核心可复用 `long_capture` 的 `stitch_canvas` / `scroll_stitcher`（不含平台头） | 未实现；被复用的拼接核心已实现并已在 Windows 真机验证（§3.3），Wayland 的取帧与滚动编排尚未开始 |
| 托盘 | StatusNotifierItem + `com.canonical.dbusmenu`（D-Bus）；GNOME 需要 **Ubuntu AppIndicators 扩展**（Ubuntu 默认随系统安装并启用；纯净 GNOME 需手动安装），扩展缺失时图标不显示 | 已实现并真机验证：GDBus / 系统 `gio-2.0`（Ubuntu `libglib2.0-dev`，不新增 vcpkg 依赖），Ubuntu 顶栏图标与菜单正常；菜单数据仍来自 `tray_command_list`，`lists.changed` 后重建；找不到 `org.kde.StatusNotifierWatcher` 时只 WARN 一次、不崩不重试。这是“能显示但依赖桌面扩展”，不是平台限制 |
| 剪贴板 | 见 §2 的 🚫：普通客户端只在自身窗口有键盘焦点时能写 selection | Wayland 下仍不计划后台写剪贴板；Linux 的 X11 selection 实现（`clipboard_x11*.cpp`，经 XWayland 桥接）已在 Ubuntu 26 真机验证（截图后剪贴板有图）；纯 Wayland 且无 XWayland 时不可用 |
| 通知 | `org.freedesktop.Notifications`（libnotify）与显示服务器无关，Wayland 下同样可用 | 已实现并真机验证（Ubuntu 26 / Wayland）：libnotify → `org.freedesktop.Notifications`，标题/正文、头部应用名、点击打开图片/文件夹正常 |
| 设置页 | GLFW 的 Wayland 后端（vcpkg 的 glfw3 同时编 X11/Wayland）或 XWayland；NFD 走 portal | 已真机验证（Ubuntu 26 / Wayland）：设置页正常创建，页签/插件页/枚举多语言正确 |
| 多屏与 HiDPI | 采集坐标用 stream 的像素尺寸与 `spa_meta` 的缩放信息，**不要**用逻辑坐标 | 未实现 |

### 3.2 Linux X11（第二优先：Ubuntu 24.04 真机已验证，单显示器）

X11 的采集 / 热键 / 覆盖层三件套已在 Ubuntu 24.04 / X11 / 单显示器（单 X screen）真机跑通。
下表记录**已验证项与剩余未验证项**；多显示器 / 多 X screen / 负坐标仍未验证。

| 能力 | 方案 | 现状 / 已验证与未验证项 |
| --- | --- | --- |
| 采集 | `XShmGetImage`（MIT-SHM）优先、`XGetImage` 回退抓默认 root window；XFixes 取光标并合成；每次调用独立 `XOpenDisplay` | 已实现并真机验证（Ubuntu 24.04 / X11 / 单显示器 / 单 X screen）：Ctrl+Alt+S 与 D-Bus 触发的整屏截图落盘正常。**仍未验证**：多显示器 / 多 X screen / 负坐标。注意 X11 会话只能看到 X 客户端窗口，Wayland 原生窗口不会进入 root pixmap（Wayland 会话应走 portal 分支） |
| 热键 | 独立 X11 连接 + `XGrabKey`/`XUngrabKey`，CapsLock/NumLock/ScrollLock 锁修饰符变体，`BadAccess` 冲突 WARN，self-pipe 事件循环，`hotkey.suspend`/`resume` | 已实现并真机验证（Ubuntu 24.04 / X11）：`XGrabKey` 注册与触发跑通（Ctrl+Alt+A / Ctrl+Alt+S / Ctrl+Alt+O）；纯 Wayland（`XOpenDisplay` 失败）运行期降级为只发布列表与配置项并 WARN 指向 `session_control`。**仍未验证**：多 X screen（只抓默认 root）。已知差异：没有 `MOD_NOREPEAT`，按住会重复触发 |
| 区域框选 | GLFW 无边框覆盖层 + 背景快照（合成器下自动回退 XComposite 覆盖窗口）+ `XQueryTree`/`_NET_WM_WINDOW_TYPE` 窗口命中；普通框选发 `capture.request`（长截图目的走 `region.selected`） | 已实现并真机验证（Ubuntu 24.04 / 单显示器 / 单 X screen）：XComposite 背景快照正常（**不再灰遮罩**）、覆盖层高于顶栏/dock、悬停窗口吸附、悬停顶栏/dock（`_NET_WORKAREA` 占位）与屏幕四角=全屏、单击截窗口、拖框、Esc 取消、确认后保存+剪贴板全部跑通；与 `screen_capture` 的 X11 分支已联调。隐藏后已补 `glfwPollEvents`。**仍未验证**：多显示器 / 多 X screen（reparenting frame 的 Z 序只在单屏环境验证） |
| 剪贴板 | `XSetSelectionOwner` 持有 `image/png`，响应 `SelectionRequest`，可选 `text/uri-list` | 已实现并真机验证（Ubuntu 24.04 / X11）：截图后剪贴板正常 |
| 通知 | libnotify 或直接调 `org.freedesktop.Notifications` | 已实现并真机验证（Ubuntu 24.04 / X11）：标题/正文正常、头部应用名正确（不再是「Plugin-shot 设置」乱码）、点击打开图片/文件夹正常 |
| 托盘 | StatusNotifierItem + DBusMenu | 已实现并真机验证：Ubuntu 顶栏图标与菜单正常（与 Wayland 同一份实现，D-Bus 与显示服务器无关） |
| 长截图 | AT-SPI2 读可滚动区域，XTest 发滚轮，按重叠区拼接 | 未实现 |
| 设置页 | GLFW 的 X11 后端（vcpkg 已编） | 已真机验证（Ubuntu 24.04 / X11）：设置页正常创建，页签/插件页/枚举多语言正确 |

### 3.3 两个新插件（已实现、真机验证）

| 插件 | 目标平台 | 代码状态 | 依赖 | 未做的事 |
| --- | --- | --- | --- | --- |
| `plugins/session_control` | Linux（Wayland / X11 会话都能用） | 已实现；随 Linux 构建产出 `.so`；D-Bus 触发已在 Ubuntu 24.04（X11）/ Ubuntu 26（Wayland）真机验证 | 系统 `libglib2.0-dev`（`pkg-config gio-2.0`）+ `pkg-config`；不引入新的 vcpkg 依赖 | 无（本轮已完成 D-Bus 触发端到端验证；`Capture()` 发出的 `capture.request` 链路同样跑通） |
| `plugins/long_capture` | Windows（P1） | 已实现；随 Windows 全量构建 exit 0，产出 DLL；完整交互已在 Windows 11 真机验证（选窗口 → 滚动 → HUD → 保存） | C++ 核心只要 `stb` + `nlohmann-json`；Win32 层要 `user32`/`gdi32`/`shell32` | 无（本轮完整会话已真机跑通） |

> 两个插件在非目标平台上都在 `shot_add_plugin()` 之前跳过：`session_control` 仅 Linux 构建，
> `long_capture` 仅 Windows 构建。二者本轮都已在各自目标平台完成真机验证，状态升为 ✅。
>
> Linux 的其它实现（`clipboard` 的 X11 selection、`notify` 的 libnotify、`region_selector` 的
> 门户定格图 B 方案与 X11 覆盖层、`tray` 的 StatusNotifierItem 后端，以及 `screen_capture` /
> `hotkey` 的 X11 分支）同样已在本轮 Ubuntu 24（X11）/ Ubuntu 26（Wayland）真机验证；仍未验证的
> 只剩 X11 多显示器 / 多 X screen / 负坐标、Wayland 窗口悬停（协议限制）与 macOS。

### 3.4 macOS

| 能力 | 要补齐什么 | 现状 / 备注 |
| --- | --- | --- |
| 构建 | 先让 vcpkg `arm64-osx` + CMake preset 通过 configure/build；统一本地默认 `x64-osx` 与 CI 的 `arm64-osx`；处理 Mach-O 贴尾对 ad-hoc 签名的影响 | CI 的 macOS job 现已通过 configure/build（2026-11-14 run）；仍无真机运行 |
| 屏幕采集 | ScreenCaptureKit（macOS 12.3+）或 `CGDisplayStream`；申请“屏幕录制”TCC 权限；多显示器坐标与光标 | 当前只有 GDI 实现，macOS 构建期跳过 |
| 全局热键 | `RegisterEventHotKey`（Carbon/HIToolbox，不需要辅助功能授权）或 `CGEventTap`（需要“辅助功能”授权） | 没有原生实现；macOS 编 `hotkey_window_stub.cpp`（发布列表与配置项、不注册），且 macOS 从未成功构建 |
| 剪贴板 | `NSPasteboard` 写 `NSImage`/PNG | 没有实现；`clipboard` 的 CMake 里已留 TODO |
| 托盘 | `NSStatusBar`/`NSStatusItem` + 菜单 | 没有实现；`tray` 的 CMake 里已留 TODO |
| 通知 | `UNUserNotificationCenter`（需要应用身份/签名） | 没有实现；`notify` 的 CMake 里已留 TODO |
| 区域框选 | `NSScreen` + `CGWindowListCopyWindowInfo` 取窗口；无边框高位窗口做覆盖层；截窗口内容还需要屏幕录制权限 | 没有实现；`region_selector` 非 Windows 跳过 |
| 长截图 | `AXUIElement` 读滚动位置并模拟滚动，需要“辅助功能”授权 | 没有实现 |
| 设置页 | GLFW（Cocoa）要求窗口与事件循环在主线程，当前插件用自建 UI 线程；NFD（`NSOpenPanel`）也应在主线程调用 | 需要改 UI 线程模型，不能只补一个平台分支 |

## 4. CI 现状

`.github/workflows/cmake.yml` 的矩阵是 Linux / macOS / Windows 三 job，统一走
`cmake --preset release` + `cmake --build --preset release`，最后只做一次
`Plugin-shot --help` 冒烟。它证明“构建脚本没有在 runner 上立刻炸”，不证明平台可用。

| 平台 | configure | build | 真机运行 | 结论 |
| --- | --- | --- | --- | --- |
| Windows | ✅ 本机与 Windows job | ✅ | ✅ 完整采集/设置/热键/剪贴板/通知/框选 e2e（本轮 8 个脚本全绿） | 唯一完整可用平台 |
| Linux（X11 / Wayland 共用） | ✅ 本机 WSL2 与 Linux job（ubuntu-24.04，2026-11-14 run） | ✅ 12 个 .so | 🟡 仅 `--help`，真实桌面未跑（真机见 §2/§3） | CI 只证明“能编译” |
| macOS | ✅ macOS job（2026-11-14 run） | ✅ CI 产物 | ❌ 无真机 | CI 只证明能编译，无真机结论 |

- **Windows job**：最近一次可见的 CI run 中通过；本机真机验证（`tools/verify/`）是主要证据。
- **Linux job**：WSL2 Ubuntu 24.04 + GCC 13.3 全量构建通过，产出宿主与 12 个 `.so`（13 个插件
  工程减去仅 Windows 的 `long_capture`；含 `screen_capture` / `region_selector` /
  `session_control` / `clipboard` / `notify` / `tray` 的 Linux 实现，以及带 X11 分支的 `hotkey`）；
  `--help` 退出 0。2026-11-14 的 CI run（ubuntu-24.04）中 Linux job 通过；CI 只跑 `--help`，
  真实桌面仍以真机验证为准。
- **macOS job**：2026-11-14 的 CI run 中通过 configure/build；本机没有 Mac，仍无真机运行。
- **CI 的 Linux job 已绿**：2026-11-14 的 run 在 `ubuntu-24.04` 上通过（此前 22.04 因 libportal /
  pipewire 版本太旧失败）；workflow 已安装 `libglib2.0-dev` / `libportal-dev` /
  `libpipewire-0.3-dev` / `libspa-0.2-dev` / `libnotify-dev` / X11 开发包。CI 只跑 `--help`，
  runner 没有真实 Wayland 会话，不能据此判断桌面可用。
- **CI 绿 ≠ 平台可用**：
  1. `--help` 在日志、插件加载和 GUI 初始化之前就返回，连插件都没加载；
  2. Linux 构建产物里已经有 `screen_capture`，但 runner 没有真实 Wayland 会话，portal/PipeWire
     通常不可用，job 全绿也截不了图；
  3. macOS runner 没有真实桌面会话，也给不了屏幕录制/辅助功能 TCC 授权；
  4. Wayland 的 portal/PipeWire 在无头 runner 上通常不可用，CI 通过不代表桌面会话可用。

### 4.1 新增 Linux 依赖包清单（供以后补 workflow）

`ubuntu-latest` 的 apt 步骤已装这些包（当前 workflow 的 apt 段，X11 组为
`libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxcomposite-dev`；更早一轮见 `fc2d69e`）；本机 WSL
（Ubuntu 24.04）也已装齐，Linux 构建通过。下表保留逐项用途与状态：

| 包 | 用途 | 现状 |
| --- | --- | --- |
| `libglib2.0-dev` | `session_control` 的 GDBus/GMainLoop、`tray` 的 Linux SNI/dbusmenu；portal 客户端也依赖 glib | workflow 已装（`fc2d69e`）；本机 WSL 已装，`session_control` 与 `tray` 编译通过 |
| `libportal-dev` | Wayland ScreenCast 的客户端封装（`XdpPortal`/`XdpSession`）；也可以 GDBus 直连，但 portal 类型与信号定义来自它 | workflow 已装（`fc2d69e`）；本机 WSL 已装，`screen_capture` / `region_selector` 的 Wayland 实现编译通过 |
| `libpipewire-0.3-dev` | 消费 ScreenCast 的 PipeWire 帧（`pw_stream`） | workflow 已装（`fc2d69e`）；本机 WSL 已装，`screen_capture` 的 PipeWire 路径编译通过 |
| `libspa-0.2-dev` | PipeWire 帧元数据（`spa_meta`）与缓冲区布局头文件 | workflow 已装（`fc2d69e`）；本机 WSL 已装，随 `libpipewire-0.3-dev` 一起使用 |
| `libnotify-dev` | Linux 通知（`org.freedesktop.Notifications` 的客户端封装） | workflow 已装（`fc2d69e`）；本机 WSL 已装，`notify` 编译通过 |
| `libx11-dev` | X11 采集 / 热键 / 框选 / 剪贴板的 Xlib（`XOpenDisplay` / `XGetImage` / `XGrabKey` / `XSetSelectionOwner`） | workflow 与 WSL 均已装；`screen_capture` / `hotkey` / `region_selector` / `clipboard` 的 X11 分支随 Linux 全量构建通过 |
| `libxext-dev` | MIT-SHM（`XShm.h` 与链接 `-lXext`）：`screen_capture` 的 `XShmGetImage` 首选路径 | workflow 与 WSL 均已装；随 Linux 全量构建通过 |
| `libxfixes-dev` | XFixes：`screen_capture` 的 `XFixesGetCursorImage` 光标合成 | workflow 与 WSL 均已装；随 Linux 全量构建通过 |
| `libxrandr-dev` | RandR：`screen_capture` 只在 debug 日志里列显示器布局（RandR 1.5+）；GLFW 自身也用它取输出布局 | workflow 与 WSL 均已装；随 Linux 全量构建通过 |
| `libxcomposite-dev` | XComposite：`region_selector` 的 X11 覆盖层在合成器下抓不到 root 时，改从 Composite Overlay Window 取背景 | **可选**：缺了只 WARN（灰遮罩），构建不失败；workflow 与 WSL 均已装 |
| `pkg-config` | `pkg_check_modules` 找 gio / libportal / libpipewire | workflow 已装 |

> 版本随发行版走：CMake 里一律用 `pkg_check_modules(... REQUIRED)` 探测，**不要写死版本号**，
> 也不要在 README 里写“Ubuntu 24.04 就是某版本”。

### 4.2 构建产物与目标发行版 glibc

Linux 的 `.so` 直接链接**构建机**上的 glibc 与系统库，不保证跨发行版可移植：本仓库
`build/Linux` 当前由 WSL Ubuntu 24.04（glibc 2.39）构建，产物最高只要求 `GLIBC_2.38`，
可在 Ubuntu 24 / 26 上运行；反过来，在 Ubuntu 26 构建的产物会要求 `GLIBC_2.43`，拷到
Ubuntu 24 上 `dlopen` 会失败。

规则：**Linux 产物按目标发行版构建**，不要跨发行版共用同一个 `build/Linux` 目录，也不要
把新发行版构建的 `.so` 拷回旧发行版；需要兼容旧发行版时，在旧发行版（或对应容器 / WSL）
里构建。

## 5. 新增功能的平台约定

1. **算法与核心保持平台无关。** 像素处理、编码、路径模板、配置模型不碰平台 API；
   平台差异只出现在少量平台层文件里。现有例子：`plugins/image_saver` 只用 stb，
   `plugins/translator/src/pack_dirs.cpp` 是唯一做三平台路径分支的文件。
2. **平台差异收敛到少量文件。** 新代码按 `*_windows.cpp` / `*_macos.mm` / `*_linux.cpp`
   拆分平台层，公共代码不写大段 `#ifdef`；仓库里现有命名是
   `plugins/screen_capture/src/gdi_desktop_capture.cpp` 这类“职责 + 平台”形式，可以沿用。
   Objective-C++（`.mm`）需要 CMake 显式启用，目前仓库还没有 macOS 平台层。
3. **未实现的平台不构建插件，而不是跑起来什么都不做。** 在插件 `CMakeLists.txt` 开头用
   `if(NOT WIN32)`（或更细的平台判断）→ `set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)` +
   `return()`；根工程会打印“跳过”而不是产出 stub。**禁止**用空实现冒充能力：宿主会按
   能力 id 选中它，最后在采集/保存阶段才失败。
4. **✅ 只能在真机上给。** 编译通过、CI 绿、`--help` 退出 0 都只是 🟡；要升级为 ✅，
   必须在目标平台跑一次对应 e2e，并把证据写进插件 README 或本矩阵。
5. **X11 与 Wayland 分开判断，Wayland 优先。** 不要写“Linux 支持”然后默认 X11；如果一份
   二进制同时支持两者，平台层必须在运行时识别会话（`XDG_SESSION_TYPE` / `WAYLAND_DISPLAY`），
   并分别标记能力。**宿主目前不做这项识别**（`src/`、`include/` 里没有任何 Wayland/X11 判断），
   要加就加在平台层，不要散在核心。
6. **平台支持合入时同提交更新**：本文件矩阵与缺口、根 `README.md`/`README.en.md` 的平台段、
   插件自己的 `README.md`、`STYLE.md` §9.1 的同步矩阵；新增平台层还要补 `tools/verify/`
   的冒烟脚本。

## 6. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 在根 README 写“全平台支持” | Windows 之外 macOS 没有构建结论、Linux 的多显示器 / 多 X screen 仍未验证；Linux 单显示器核心链路已验证也不等于全平台支持 |
| 给未实现的平台留空实现（例如 macOS 的 `tray`、`clipboard`） | 空实现会让宿主误判能力可用，失败点被推迟到运行时，比构建期跳过更难查（`hotkey` 例外：Linux 用 X11 `XGrabKey`，纯 Wayland 运行期降级为只提供列表与配置项并 WARN；macOS 编不注册的 stub） |
| 把 `--help` 冒烟当作平台验证 | 它在插件加载与 GUI 初始化之前返回，几乎不覆盖任何平台代码 |
| 用 X11 API 冒充 Linux 支持 | Wayland 会话下 `XGetImage`/`XGrabKey`/全局覆盖层都不可用，而 Wayland 是默认目标 |
| 在 Wayland 上用 libinput/evdev 直接抓键盘 | 需要 root/input 组权限，会绕过桌面环境的快捷键分发；`session_control` 明确不做这条 |
| 在 Wayland 上假装“区域框选 = 全屏遮罩” | GNOME 不允许客户端定位全屏窗口、也读不到别的应用的窗口几何；B 方案的体验差异必须写进文档 |
| 把 portal 授权弹窗当一次性的 | 每个 ScreenCast 会话都可能重新弹窗；拒绝授权是正常路径，必须走 `capture.failed` |
| 在 macOS 上直接照搬 Win32 的窗口/热键/托盘方案 | Cocoa 主线程、TCC 授权、`NSStatusBar`/`NSPasteboard` 都是另一套模型 |

## 7. 检查清单

- [ ] 我改动的平台代码是否只在 `*_windows.cpp` / `*_macos.mm` / `*_linux.cpp` 或等价平台层里？
- [ ] 公共头是否仍然不包含 `<windows.h>`、Cocoa、X11、GLib 等平台头？
- [ ] 未实现的平台是否在 `CMakeLists.txt` 里 `SHOT_PLUGIN_SKIPPED`，而不是留空实现（`hotkey` 的 macOS stub 与纯 Wayland 运行期降级是明确不注册的例外）？
- [ ] 新增插件是否在 `shot_add_plugin()` **之前**判断平台（否则依赖与源文件会先被解析）？
- [ ] Wayland 与 X11 是否分别给了状态，没有用一句“Linux 支持”糊过去？
- [ ] 本文件的矩阵是否已按新证据更新（✅/🟡/⛔/🚫 没有升级错，未真机验证的仍标 🟡 并写清“能编译未验证”）？
- [ ] 根 `README.md` / `README.en.md`、插件 `README.md`、`STYLE.md` §9.1 是否同提交更新？
- [ ] 是否把“能编译”误写成了“已验证”？
