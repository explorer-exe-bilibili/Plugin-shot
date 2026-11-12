# 跨平台支持矩阵（Windows / Linux Wayland / Linux X11 / macOS）

这份文档回答一个问题：**这个仓库在某个平台上到底“能不能用”**，而不是“能不能编译”。
仓库是 Windows-first：Windows 是唯一在真机上跑过完整截图链路的平台。Linux 的**默认目标是
Wayland**（维护者实测环境：Ubuntu 24.04 / GNOME 46 / Wayland 会话），X11 保留但列为
**第二优先**；macOS 从未真机运行，也没有成功的构建结论。平台相关插件在非目标平台上由
CMake 显式跳过（`SHOT_PLUGIN_SKIPPED`），不产出“跑起来什么都不做”的伪实现。

> **2026-11-12 同步说明**：`screen_capture`（Wayland 门户）、`region_selector`（Wayland B 方案）、
> `clipboard`（X11）、`notify`（libnotify）、`session_control`、`long_capture` 的实现代码已入库，
> 但尚未在本机编译、也未在真机验证；下表部分单元格仍按“未实现 / 草稿”记录。
> **0.1.0 只发布 Windows 构建，未验证能力不随包分发**；表格会在逐项验证后更新。

**改平台相关代码、CMake 跳过条件、根 `README*.md` 的平台段、或插件清单前先读这里，
并在同一个提交里更新矩阵。**

> **核对口径**：本表只承认**已合入、能 configure/build 的插件工程**；工作区里未提交的并行
> 草稿不计为平台支持。当前有两个草稿：`plugins/session_control`（Linux，源码已齐）与
> `plugins/long_capture`（Windows，源文件未落齐），状态见 §3.3。草稿合入、编译并真机验证后，
> 必须在同一个提交里更新对应格子，**只有真机跑过才升 ✅**。

## 1. 记号

| 记号 | 含义 | 判定标准 |
| --- | --- | --- |
| ✅ | 真机验证 | 在对应平台真机上跑过对应功能（当前只有 Windows 11 有完整证据），有日志、落盘文件或截图证据 |
| 🟡 | 未真机验证 | 有代码与构建路径，但没在目标平台真机上跑过。格子里必须写清是**“能编译未验证”**还是**“草稿（连编译都没跑过）”**；后者不能被读成能编译 |
| ⛔ | 未实现（构建跳过） | 该平台不构建这个插件（`if(NOT WIN32)` / `if(APPLE)` 等 → `SHOT_PLUGIN_SKIPPED`），或目标平台上根本没有对应实现 |
| 🚫 | 平台限制 | 平台机制不允许，或必须依赖非标准协议、用户授权、桌面扩展；不是“补一个函数”就能解决 |

单元格里写的都是**理由**。**“CI 绿”不升级任何格**：CI 只跑 `--help`，见 §4。

## 2. 总矩阵

列的顺序就是优先级：Windows（唯一完整链路）、Linux Wayland（**默认目标**）、
Linux X11（**第二优先**）、macOS。

| 能力 / 插件 | Windows | Linux Wayland（默认目标） | Linux X11（第二优先） | macOS |
| --- | --- | --- | --- | --- |
| 宿主核心（加载 / ABI / 事件总线 / 列表 / 配置 / CLI） | ✅ 本机全链路：插件加载与热卸载、采集/设置 e2e、`--capture`/`--settings`/`--help` 都跑过 | 🟡 与 X11 同一份二进制；宿主不依赖显示服务器，但只做过 WSL2 编译与无头 `--help`，Wayland 会话未跑 | 🟡 WSL2 下 configure/build 通过、`--help` 退出 0；真实 X11 桌面未跑 | 🟡 代码有 `_NSGetExecutablePath` / `dlopen` 分支，但 macOS 从未成功 configure/build |
| 屏幕采集 `plugins/screen_capture` | ✅ `gdi_desktop_capture.cpp` 用 `GetDC`/`BitBlt`/`GetDIBits` 抓物理虚拟桌面，双屏 3584x1600 与光标合成已实测 | ⛔ 没有 portal/PipeWire 实现；Wayland 不提供全局抓屏，唯一通道是 `xdg-desktop-portal` 的 ScreenCast（§3.1） | ⛔ 没有 `XGetImage`/`XShmGetImage` 实现，构建期跳过（§3.2） | ⛔ 只有 GDI 实现；CMake 跳过，没有 ScreenCaptureKit / CGDisplayStream 分支 |
| 图像保存 `plugins/image_saver` | ✅ PNG/JPEG 落盘在 `--capture` 与多个 e2e 中验证 | 🟡 能编译未验证：纯 POSIX 代码、`.so` 编译通过；没有 Linux 采集链路，真实桌面没跑过保存 | 🟡 能编译未验证：与 Wayland 同一份 POSIX 分支 | 🟡 `output_path.cpp` 有 `$HOME/Pictures` 与 `localtime_r` 分支，但 macOS 未构建 |
| 图像处理 `plugins/example_processor` / `opencv_provider` | ✅ 两个 `image_processor` 参与采集流水线，`opencv_provider` 把 OpenCV 带进进程 | 🟡 能编译未验证：`.so` 编译并被 dlopen；没有真实采集帧进入处理链 | 🟡 能编译未验证：同上 | 🟡 只用标准库/OpenCV，CMake 不跳过，但 macOS 未构建 |
| 设置页 `plugins/settings_ui` | ✅ ImGui + GLFW + OpenGL3，双屏 150%+100% DPI、跨屏拖动与几何恢复已 e2e | 🟡 能编译未验证：WSLg 下建过 GL 上下文；GLFW Wayland 后端、NFD portal、中文渲染未在 Wayland 会话验证 | 🟡 能编译未验证：真实 X11 下的字体、文件对话框与 DPI 未跑 | 🟡 Cocoa 要求窗口与事件循环在主线程，而插件用自建 UI 线程，是已知移植风险；未构建 |
| 托盘 `plugins/tray` | ✅ Win32 `Shell_NotifyIconW` + 数据驱动菜单，本机运行验证 | ⛔ 没有 StatusNotifierItem/AppIndicator 实现，构建期跳过；Ubuntu GNOME 默认带 AppIndicator 扩展，技术上可用（§3.1） | ⛔ 同上：SNI 与显示服务器无关，缺的是实现 | ⛔ 没有 `NSStatusBar`/`NSStatusItem` 实现，CMake 跳过 |
| 全局热键 `plugins/hotkey` | ✅ `RegisterHotKey` + 隐藏消息窗口；`Ctrl+Alt+S` 等与 `e2e_hotkey_capture.ps1` 已验证 | 🚫 没有全局热键协议（GNOME 46 未提供 `GlobalShortcuts` portal）；改用 `plugins/session_control` + 桌面自定义快捷键 | ⛔ 没有 `XGrabKey` 实现（第二优先）；过渡期可先用 `session_control` 的 D-Bus 入口 | ⛔ 没有 `RegisterEventHotKey`/`CGEventTap` 实现，CMake 跳过 |
| 会话控制入口 `plugins/session_control` | ⛔ 仅 Linux 构建（GDBus 依赖桌面会话），Windows/macOS 在 `shot_add_plugin()` 之前跳过 | 🟡 **草稿（未编译）**：源码已齐、CMake 仅 Linux；没跑过编译，也没和 GNOME 自定义快捷键做过端到端（§3.3） | 🟡 **草稿（未编译）**：D-Bus 是会话级的，X11 会话同样能用，但同样未编译验证 | ⛔ 不构建 |
| 剪贴板 `plugins/clipboard` | ✅ `OpenClipboard` + `SetClipboardData` 写 `CF_DIB`（可选 `CF_HDROP`），`e2e_clipboard.ps1` 验证 | 🚫 普通客户端只能在自身窗口持有键盘焦点时写 selection；后台复制需要 wlr/ext-data-control 等非标准协议，X11 草稿不能套用 | ⛔ 工作区有 `XSetSelectionOwner` + SelectionRequest 草稿（`clipboard_x11*.cpp`，已改 CMake），但未提交、未编译，不计为支持 | ⛔ 没有 `NSPasteboard` 实现，CMake 跳过 |
| 通知 `plugins/notify` | ✅ 隐藏占位图标 + `Shell_NotifyIconW(NIF_INFO)`，Windows 11 真机显示成功 | ⛔ 没有实现；`org.freedesktop.Notifications` 在 Wayland 下可用，工作区有 libnotify 草稿（已改 CMake、未编译，§3.3） | ⛔ 同上 | ⛔ 没有 `UNUserNotificationCenter` 实现，CMake 跳过 |
| 区域框选 `plugins/region_selector` | ✅ GLFW 覆盖层 + Win32 物理坐标；悬停窗口吸附、单击截窗、DWM 扩展边界、主题色折角已实测 | ⛔ 没有实现；GNOME 不支持 layer-shell、也不允许读全局窗口列表，只能走“门户截图 → 自有窗口内拖选定格图”的 B 方案（§3.1） | ⛔ 没有 override-redirect 覆盖层 + `_NET_CLIENT_LIST`/`_NET_FRAME_EXTENTS` 枚举实现（§3.2） | ⛔ 没有 `NSScreen`/`CGWindowList` 覆盖层实现，CMake 跳过 |
| 外部语言包 / 翻译 `plugins/translator` | ✅ 扫描官方/用户目录、用户包覆盖官方包、坏包只 WARN，`smoke_i18n_translator.ps1` 已跑 | 🟡 能编译未验证：CMake 不跳过，POSIX 分支编译过；未在 Wayland 会话跑过 | 🟡 能编译未验证：同一份 POSIX 路径 | 🟡 `pack_dirs.cpp` 有 `_NSGetExecutablePath` + `$HOME` 分支，但 macOS 未构建 |
| 长截图 `plugins/long_capture` | ⛔ **草稿（未编译）**：`SOURCES` 里还没有 `plugin_entry.cpp`（没有 `shot_module_entry`），Windows 下 configure 会因找不到源文件失败（§3.3） | ⛔ 未实现；计划走 ScreenCast 的**窗口流**持续取帧 + 复用草稿里的拼接核心（§3.1） | ⛔ 未实现；计划 `XGetImage` 逐帧 + `XTest`/手动滚动 + 行指纹拼接（§3.2） | ⛔ 未实现；需要 `AXUIElement` 读滚动位置并模拟滚动 |

**结论**：唯一跑通过完整“采集 → 处理 → 保存 → 消费”链路的是 Windows。Linux 的“能编译”只覆盖
宿主与跨平台插件（`settings_ui` / `image_saver` / `example_processor` / `opencv_provider` /
`translator`）；采集、框选、热键、托盘、剪贴板、通知在 Linux 上都**不构建**。Wayland 是默认目标，
但三条关键能力分别依赖 portal（抓屏）、B 方案（框选）与 `session_control`（触发入口），目前
一条都还没写；X11 是第二优先，缺的是 `XGetImage` / `XGrabKey` / 覆盖层三件套。macOS 没有
构建结论。

## 3. 缺口清单（要补齐需要做什么）

### 3.1 Linux Wayland（默认目标）

| 能力 | 方案 | 现状 / 阻塞点 |
| --- | --- | --- |
| 抓屏 | `org.freedesktop.portal.ScreenCast`（`CreateSession` → `SelectSources` → `Start`）拿到 PipeWire 流，再从流里取帧 | 未实现。Wayland 不提供全局抓屏，只能走 portal：`Screenshot` portal 只给单帧、拿不到持续流，所以默认方案是 ScreenCast。依赖 `libportal-dev`（或 GDBus 直连 portal）+ `libpipewire-0.3-dev` + `libglib2.0-dev` |
| 授权弹窗 | 每次新建 ScreenCast 会话，合成器都会弹系统授权对话框（Ubuntu 24.04 / GNOME 46 的预期行为）；portal 的 `persist_mode`/`restore_token` 能否免弹窗取决于桌面实现与版本，**不要当既有能力** | 未实现、未实测。必须设计“用户拒绝授权”的失败路径（`capture.failed`），不能假装成功 |
| 全局热键 | 没有全局热键协议；改用 `plugins/session_control` 在会话总线暴露 `org.pluginshot.Control`，用户在 GNOME「自定义快捷键」里绑一条 `gdbus call`，桌面负责按键 | 插件是草稿、未编译；绑定步骤见 `plugins/session_control/README.md` |
| 区域框选（B 方案） | 先用门户抓一张整屏定格图 → 在自己的窗口里显示这张图 → 用户拖选 → 按选区裁像素 | 未实现。**没有窗口悬停吸附**：portal 不提供其它应用的窗口列表与几何。窗口位置与大小由合成器决定，做不成全屏遮罩，只能“先看图、再拖选” |
| 长截图 | ScreenCast 的 `SelectSources` 选**窗口**源持续取帧，在内存里按行指纹拼接；拼接核心可复用 `long_capture` 草稿里的 `stitch_canvas` / `scroll_stitcher` / `frame_loader` / `long_image_saver`（不含平台头） | 未实现；被复用的四个文件本身也是未编译草稿（§3.3） |
| 托盘 | StatusNotifierItem + `com.canonical.dbusmenu`（D-Bus）；GNOME 需要 **AppIndicator 扩展**（Ubuntu 默认随系统安装并启用），扩展缺失时图标不显示 | 未实现，构建期跳过。这是“能显示但依赖桌面扩展”，不是平台限制 |
| 剪贴板 | 见 §2 的 🚫：普通客户端只在自身窗口有键盘焦点时能写 selection | 不计划在 Wayland 上做后台写剪贴板；X11 草稿不能套用 |
| 通知 | `org.freedesktop.Notifications`（libnotify）与显示服务器无关，Wayland 下同样可用 | 工作区有 libnotify 草稿（已改 CMake），未提交、未编译 |
| 设置页 | GLFW 的 Wayland 后端（vcpkg 的 glfw3 同时编 X11/Wayland）或 XWayland；NFD 走 portal | 能编译，Wayland 会话未验证 |
| 多屏与 HiDPI | 采集坐标用 stream 的像素尺寸与 `spa_meta` 的缩放信息，**不要**用逻辑坐标 | 未实现 |

### 3.2 Linux X11（第二优先）

| 能力 | 要补齐什么 | 现状 |
| --- | --- | --- |
| 采集 | `XGetImage`/`XShmGetImage` 抓 root window；RandR/Xinerama 拼多屏；XFixes 取光标并合成 | 没有代码；`screen_capture` 在非 Windows 直接跳过 |
| 热键 | 独立 X11 连接 + `XGrabKey`/`XUngrabKey`，处理 NumLock/CapsLock 组合，自己跑事件循环 | `hotkey` 的 CMake 里已留 TODO；非 Windows 跳过。过渡期可先用 `session_control` 的 D-Bus 入口 |
| 区域框选 | override-redirect 全屏窗口 + XComposite/XShape；`_NET_CLIENT_LIST`/`_NET_FRAME_EXTENTS` 枚举窗口；多屏用 Xinerama | `region_selector` 只有 Win32 物理坐标版本，非 Windows 跳过 |
| 剪贴板 | `XSetSelectionOwner` 持有 `image/png`，响应 `SelectionRequest`，可选 `text/uri-list` | 工作区有草稿（`clipboard_x11*.cpp`，已进 CMake），未提交、未编译 |
| 通知 | libnotify 或直接调 `org.freedesktop.Notifications` | 工作区有草稿（已改 CMake），未提交、未编译 |
| 托盘 | StatusNotifierItem + DBusMenu | 未实现 |
| 长截图 | AT-SPI2 读可滚动区域，XTest 发滚轮，按重叠区拼接 | 未实现 |
| 设置页 | GLFW 的 X11 后端（vcpkg 已编） | 代码可编译；真实 X11 桌面的字体、NFD 对话框与 DPI 未验证 |

### 3.3 两个新插件（工作区草稿，不计入平台支持）

| 插件 | 目标平台 | 代码状态（当前工作区） | 依赖 | 未做的事 |
| --- | --- | --- | --- | --- |
| `plugins/session_control` | Linux（Wayland / X11 会话都能用） | 13 个文件，5 个源文件与头文件齐全；**未编译、未提交** | 系统 `libglib2.0-dev`（`pkg-config gio-2.0`）+ `pkg-config`；不引入新的 vcpkg 依赖 | 没跑过编译；没在 GNOME 自定义快捷键上做端到端；`Capture()` 发出的 `capture.request` 目前没有 Linux 采集者，不会产生文件 |
| `plugins/long_capture` | Windows（P1） | 源文件还在落齐中：平台层（`window_target_windows` / `hud_windows` / `global_keys_windows` / `tray_hint_windows`）已就位，**编排层 `plugin_entry` / `long_capture` / `long_capture_session` 仍缺**（截至核对时）；没有 `README.md` | C++ 核心只要 `stb` + `nlohmann-json`；Win32 层要 `user32`/`gdi32`/`shell32` | 缺 `shot_module_entry`；**Windows 下 `add_subdirectory` 会因为找不到源文件让整仓 configure 失败**；没有编译/运行记录 |

> 关键：两个插件都是“跳过”写在 `shot_add_plugin()` 之前。`session_control` 在 Windows/macOS 上
> 直接跳过、不受未编译影响；`long_capture` 在 Linux/macOS 上也直接跳过，但它**自己的目标平台是
> Windows**——所以在 Windows 上反而会 configure 失败。合入前必须先补齐源文件，再按 §5 升格状态。
>
> 另有并行 agent 正在做的 Linux 草稿：`clipboard` 的 X11 selection、`notify` 的 libnotify、
> `region_selector` 的 portal 覆盖层。接入程度随时在变（已改 CMake 的、只落了文件的都有），
> 共同点是**未提交、未编译**，因此一律不计入矩阵，等合入后按 §5 重新核对。

### 3.4 macOS

| 能力 | 要补齐什么 | 现状 / 备注 |
| --- | --- | --- |
| 构建 | 先让 vcpkg `arm64-osx` + CMake preset 通过 configure/build；统一本地默认 `x64-osx` 与 CI 的 `arm64-osx`；处理 Mach-O 贴尾对 ad-hoc 签名的影响 | CI 的 macOS job 从未进入编译，历史上在 configure 阶段失败 |
| 屏幕采集 | ScreenCaptureKit（macOS 12.3+）或 `CGDisplayStream`；申请“屏幕录制”TCC 权限；多显示器坐标与光标 | 当前只有 GDI 实现，macOS 构建期跳过 |
| 全局热键 | `RegisterEventHotKey`（Carbon/HIToolbox，不需要辅助功能授权）或 `CGEventTap`（需要“辅助功能”授权） | 没有实现；`hotkey` 的 CMake 里已留 TODO |
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
| Windows | ✅ 本机与 Windows job（草稿加入之前的树） | ✅ | ✅ 完整采集/设置/热键/剪贴板/通知/框选 e2e | 唯一完整可用平台 |
| Linux（X11 / Wayland 共用） | 🟡 本地 WSL2 通过；最近的 CI run 在 vcpkg baseline 修复前于 configure 失败，修复后没有复跑结论 | 🟡 同左 | 🟡 仅 `--help` 与无头 `--capture`，真实桌面未跑 | 只证明“能编译” |
| macOS | ❌ 从未通过 | ❌ 无产物 | ❌ 无真机 | 只有代码审计 |

- **Windows job**：最近一次可见的 CI run 中通过；本机真机验证（`tools/verify/`）是主要证据。
- **Linux job**：WSL2 Ubuntu 24.04 + GCC 13.3 全量构建通过（**草稿加入之前**的树），产出宿主与
  `settings_ui` / `image_saver` / `example_processor` / `opencv_provider` 四个 `.so`；
  用 `git archive` 在 ext4 上按 workflow 的 preset 流程重跑 configure/build/`--help` 也通过。
  **不要把“本地 WSL 能编译”写成“CI 已绿”。**
- **macOS job**：从未成功进入编译。历史上失败在 configure 阶段（vcpkg/工具链），本机没有 Mac。
- **草稿合入会让 CI 先红**：`session_control` 的 CMake 里是
  `pkg_check_modules(GIO REQUIRED gio-2.0)`，workflow 没有装 `libglib2.0-dev`；
  `long_capture` 还缺 `plugin_entry.cpp` 等源文件，会让 Windows job 的 configure 失败；
  `clipboard` / `notify` 的 Linux 草稿合入后还要 `libx11-dev`（已装）与 `libnotify-dev`（未装）。
  补 workflow 前不要合入。
- **CI 绿 ≠ 平台可用**：
  1. `--help` 在日志、插件加载和 GUI 初始化之前就返回，连插件都没加载；
  2. Linux 构建产物里没有 `screen_capture`，即使 job 全绿也截不了图；
  3. macOS runner 没有真实桌面会话，也给不了屏幕录制/辅助功能 TCC 授权；
  4. Wayland 的 portal/PipeWire 在无头 runner 上通常不可用，CI 通过不代表桌面会话可用。

### 4.1 新增 Linux 依赖包清单（供以后补 workflow）

`ubuntu-latest` 的 apt 步骤里，有的是已经装的（`libx11-dev` / `pkg-config`），有的还没装。
列在这里是为了让“补 workflow”是一张确定的清单，而不是到了 configure 才逐条猜：

| 包 | 用途 | 现状 |
| --- | --- | --- |
| `libglib2.0-dev` | `session_control` 的 GDBus/GMainLoop；portal 客户端也依赖 glib | **已需要但 workflow 未装**，`session_control` 合入即失败 |
| `libportal-dev` | Wayland ScreenCast 的客户端封装（`XdpPortal`/`XdpSession`）；也可以 GDBus 直连，但 portal 类型与信号定义来自它 | 未装、代码未使用 |
| `libpipewire-0.3-dev` | 消费 ScreenCast 的 PipeWire 帧（`pw_stream`） | 未装、代码未使用 |
| `libnotify-dev` | Linux 通知（`org.freedesktop.Notifications` 的客户端封装） | 未装；notify 的 Linux 草稿已改 CMake，合入后必需 |
| `libx11-dev` | X11 剪贴板 / 热键 / 框选的 Xlib | workflow 已装（glfw3 端口也需要） |
| `pkg-config` | `pkg_check_modules` 找 gio / libportal / libpipewire | workflow 已装 |

> 版本随发行版走：CMake 里一律用 `pkg_check_modules(... REQUIRED)` 探测，**不要写死版本号**，
> 也不要在 README 里写“Ubuntu 24.04 就是某版本”。

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
| 在根 README 写“全平台支持” | Windows 之外没有完整链路；Linux 只验证编译，macOS 没有构建结论 |
| 给非 Windows 平台留空实现的 `screen_capture`/`tray`/`hotkey` | 空实现会让宿主误判能力可用，失败点被推迟到运行时，比构建期跳过更难查 |
| 把 `--help` 冒烟当作平台验证 | 它在插件加载与 GUI 初始化之前返回，几乎不覆盖任何平台代码 |
| 用 X11 API 冒充 Linux 支持 | Wayland 会话下 `XGetImage`/`XGrabKey`/全局覆盖层都不可用，而 Wayland 是默认目标 |
| 在 Wayland 上用 libinput/evdev 直接抓键盘 | 需要 root/input 组权限，会绕过桌面环境的快捷键分发；`session_control` 明确不做这条 |
| 在 Wayland 上假装“区域框选 = 全屏遮罩” | GNOME 不允许客户端定位全屏窗口、也读不到别的应用的窗口几何；B 方案的体验差异必须写进文档 |
| 把 portal 授权弹窗当一次性的 | 每个 ScreenCast 会话都可能重新弹窗；拒绝授权是正常路径，必须走 `capture.failed` |
| 在 macOS 上直接照搬 Win32 的窗口/热键/托盘方案 | Cocoa 主线程、TCC 授权、`NSStatusBar`/`NSPasteboard` 都是另一套模型 |

## 7. 检查清单

- [ ] 我改动的平台代码是否只在 `*_windows.cpp` / `*_macos.mm` / `*_linux.cpp` 或等价平台层里？
- [ ] 公共头是否仍然不包含 `<windows.h>`、Cocoa、X11、GLib 等平台头？
- [ ] 未实现的平台是否在 `CMakeLists.txt` 里 `SHOT_PLUGIN_SKIPPED`，而不是留空实现？
- [ ] 新增插件是否在 `shot_add_plugin()` **之前**判断平台（否则依赖与源文件会先被解析）？
- [ ] Wayland 与 X11 是否分别给了状态，没有用一句“Linux 支持”糊过去？
- [ ] 本文件的矩阵是否已按新证据更新（✅/🟡/⛔/🚫 没有升级错，草稿仍标“草稿”）？
- [ ] 根 `README.md` / `README.en.md`、插件 `README.md`、`STYLE.md` §9.1 是否同提交更新？
- [ ] 是否把“能编译”误写成了“已验证”？
