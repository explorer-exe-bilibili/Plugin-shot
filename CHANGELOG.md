# 更新日志

## 0.1.0 — 2026-11-15（更新：插件开发链路）

"插件开发者版本"：把插件开发变成**不需要宿主源码**的离线流程，并让 AI 能"一句话写插件"。

### 新增

- **离线插件 SDK 拆分**：`export_sdk` 产出核心包（契约头、`shot_plugin_helpers`、构建模块、
  文档、模板、demo、技能，约 0.9 MB）+ 可选 OpenCV 叠加包（约 13 MB）；`package_sdk` 打成
  zip + sha256。SDK 自带 `third_party/include`（nlohmann-json 整树、stb），**不装 vcpkg、
  不 `find_package`** 即可编译；`ShotPlugin.cmake` 新增 SDK 模式，自动补第三方头与
  （叠加包存在时的）OpenCV 头/库路径，并按 Debug/Release 选择对应 lib。
- **插件离线自检** `Plugin-shot.exe --test-plugin <dll>`：走真实宿主加载/校验路径（`HostRuntime`），
  在隔离数据目录里检查加载状态、能力调用（处理器/保存器/采集器/lib provider）、配置回写回读、
  托盘/热键/i18n 列表贡献、事件派发，输出 PASS/FAIL/SKIP 与 `--json-out`；退出码 0/1/2。
  不构建整个宿主、不装进 `plugins/` 就能验收插件。文档：`document/PLUGIN_TEST.md`。
- **插件开发文档与模板**：`document/AUTHORING.md`（从零到安装的单一事实源）；
  `tools/plugin_sdk/templates/`（generic / image_processor / image_saver / lib_provider）+
  `tools/new_plugin.cmake` 脚手架；`tools/plugin_sdk/demos/` 两个"一句话需求"完整示例：
  **截图右下角时间水印**（image_processor）与**按日期归档**（capture.done 订阅者）。
- **DSH 技能 `plugin-shot-dev`**：`skills/plugin-shot-dev/`（SKILL.md + 6 份 references），
  并打包成可安装的 DSH 插件 bundle `dsh-plugin-shot-dev/`：下载 SDK → 生成工程 → 编译 →
  `--test-plugin` 自检 → 征得同意后安装。
- `PLUGIN_SHOT_DATA_DIR` 环境变量覆盖用户数据目录（自检/便携隔离用）；`Host::Logging::shutdown()`。
- 发布物料：`tools/package/make_host_package.ps1` 生成宿主便携包；发布资产：
  - `Plugin-shot-0.1.0-win64.zip`（6,803,044 B，sha256 `96485e014bf978849f0ad4ef736d4f6b29fb8a204bd84e762c77fe1fdc68aa91`）
  - `plugin-shot-sdk-0.1.0-win64.zip`（954,614 B，sha256 `31b1244e7e97277c5dd1b86663316c9bafe5a695f23d775363427c8bd7c1b138`）
  - `plugin-shot-sdk-0.1.0-win64-opencv.zip`（13,502,785 B，sha256 `5cce48d914073542933f6b2cf68f52712df1ab1cc3d480357322e2c6befab0c3`）
- **GitHub Actions 构建产物上传**：三平台 CI 构建完成后上传 artifact：
  `plugin-shot-<platform>-release`（`build/<platform>/bin` 的 zip，保留 7 天）；Windows 额外上传
  `plugin-shot-windows-dist-<版本>`（宿主便携包 `Plugin-shot-<版本>-win64.zip` +
  `plugin-shot-sdk-<版本>-win64{,-opencv}.zip` 及 `.sha256`，来自
  `tools/package/make_host_package.ps1` 与 CMake 目标 `package_sdk`）。Windows 的宿主便携包
  解压即可运行；Linux / macOS 的 artifact 只是编译产物打包，依赖 vcpkg 动态库且 RPATH 指向
  构建目录，**不保证**下载后能直接运行。SDK 打包步骤是 best-effort（`continue-on-error`）：
  若 CI 日志提示 SDK 打包失败，可手动本地跑 `package_sdk`。

### 变更

- 根 `CMakeLists.txt` 尊重 `VCPKG_INSTALLED_DIR`（多构建目录可复用同一份 vcpkg 依赖；
  copy_dlls / 剪枝 / RPATH 不再写死 `build/<dir>/vcpkg_installed`）。
- `document/README_SDK.md` 全面重写（两包、manifest/ABI、SDK 模式构建、自检、Debug/Release、
  故障排查）；新增 `document/SDK_DOCS.md`（文档映射与防漂移）。
- 根 README / README.en / AGENTS 补充 SDK、`--test-plugin`、模板/demo/技能与发布流程。

### 修复

- 自检模式 `--test-plugin --help` 不再把 `--help` 当 DLL 路径（打印用法并退出 0）。
- 插件侧 `shot_i18n.h` 注释过时（SDK 已内置 nlohmann-json，无需 `find_package`）。

### 修复（2026-11-15 续）

- **Release 构建改为 Windows 窗口子系统**：`CMakeLists.txt` 新增 `if(WIN32 AND BUILD_RELEASE)` 段
  （`WIN32_EXECUTABLE TRUE` + `/ENTRY:mainCRTStartup` + `SHOT_GUI_SUBSYSTEM`），`Release` 宿主双击
  启动常驻、不弹控制台窗口；`Debug` 仍为控制台子系统，方便在终端看日志与断点调试。`Release` 从
  终端带参数启动（`--capture` / `--test-plugin` / `--help` / `--settings`）时，`src/main.cpp` 顶部
  的 `AttachConsole(ATTACH_PARENT_PROCESS)` 会附着父控制台，命令行输出仍可见；双击无参数常驻则
  完全无窗口。
- **配置落盘失败不再每秒刷屏**：`HostRuntime::tick` 对配置保存失败改为 **30 秒退避重试**；第一次
  失败打印一条完整 WARN（提示"受限环境/沙箱/权限"可能导致日志、截图保存、剪贴板一起失败，建议
  改用普通用户从资源管理器或系统终端启动），后续每次重试只打印"第 N 次失败"。根因是进程没有
  普通用户桌面与文件权限——典型症状：`File logging disabled`、`Failed to open config file for
  writing`、`Shell_NotifyIconW NIM_ADD` 失败、截图保存失败、剪贴板为空；**不是缺 OpenCV DLL，
  也不是发布包损坏**。普通用户从资源管理器或系统终端启动正常（本机已实测）。
- **插件元数据版本警告消除**：`plugins/long_capture/src/plugin_entry.cpp` 的 `.version` 由
  `"0.1.0"` 改为 `"0.2.1"`，与 `plugins/long_capture/metadata.json` 一致，"元数据与 get_info
  不一致"的警告不再出现。

### 已知限制

- 预编译包仍只提供 Windows x64；Linux SDK / Linux 预编译包未发布（Linux 宿主源码构建已验证）。
- `--test-plugin` 与宿主同进程：插件访问违例会带走自检进程（C++ 异常已兜底，SEH 未做）；
  GUI 类插件只做加载检查；事件试运行可能产生真实副作用（剪贴板/通知）。
- 自检必须与插件同构建配置（Debug 插件用 Debug 宿主）。
- 录屏、标注、OCR、上传仍未实现（见 `document/AUTHORING.md` §13 能力边界）。

## 0.1.0 — 2026-11-14（首发）

首个公开版本：Windows 10/11 x64 便携版；Linux（Ubuntu 24.04 X11 / Ubuntu 26 Wayland，单显示器）源码构建已验证。

### 新增

- 插件宿主：扫描、校验、依赖排序、加载、运行期启停与热卸载。
- 单入口 `shot_module_entry` + 自描述模块表，ABI 2.0（只追加）。
- 整屏截图与区域截图（物理虚拟桌面像素，混合 DPI 多屏）。
- 区域框选覆盖层：悬停窗口吸附、单击截窗、拖动选区，主题色折角。
- PNG / JPEG 保存，目录、格式、质量、文件名模板可配置。
- 截图后写入剪贴板（CF_DIB / 可选 CF_HDROP）与系统通知。
- 托盘入口、全局热键（可在设置页按组合键录入）、设置页。
- 设置页按插件声明自动生成表单；插件可启用/禁用，禁用即热卸载。
- 设置页新增「常规 / 插件」页签、插件名称与描述、任务栏闪烁提示；枚举值按 i18n key 显示，插件页文案随语言刷新。
- 默认保存目录改用系统图片库（Windows KnownFolder / Linux XDG user-dirs），不再硬编码 `Pictures`。
- 6 套主题色、27 个语言标签、外部语言包协议与翻译插件。
- 官方 zh-TW 语言包（`files/lang/zh-TW.json`，繁體中文，覆盖 core 与全部内置插件，启用 translator 后生效）；截图通知可点击打开图片 / 所在文件夹，并显示应用名短名。
- 长截图为实验特性：hash / correlate / phase 三种位移估计算法、缺口回滚恢复、50ms 送帧、高度上限 120000，默认禁用。
- 命令行 `--capture` / `--settings` / `--help`。
- 离线插件 SDK（`export_sdk` 目标）。
- 真机验证脚本：采集、光标合成、设置页、热键、剪贴板、区域框选、多语言。

### Linux（已实现，真机已验证）

0.1.0 的**预编译包**仍只提供 Windows 构建；Linux 已在上面的 Ubuntu 24.04（X11）与
Ubuntu 26（Wayland，单显示器）真机验证（采集 / 框选 / 保存 / 剪贴板 / 通知 / 热键（X11）/
D-Bus 触发（Wayland）/ 设置页 / 语言包 / 托盘），可按源码构建。

- 托盘插件新增 Linux 后端：StatusNotifierItem + `com.canonical.dbusmenu`（GDBus / 系统
  `gio-2.0`，Ubuntu 需 `libglib2.0-dev`，不新增 vcpkg 依赖）；Windows 后端不变，macOS 仍跳过。
  Ubuntu GNOME 需要 Ubuntu AppIndicators 扩展（Ubuntu 默认自带；纯净 GNOME 需手动安装）；
  找不到 `org.kde.StatusNotifierWatcher` 时只 WARN 一次、不崩不重试；菜单数据仍来自
  `tray_command_list`，`lists.changed` 后重建。
- 区域框选修复 Wayland 隐藏/编码时序：隐藏覆盖层后先 `glfwPollEvents` 再编码，PNG 编码/写盘
  移入工作线程；portal 超时 120s → 30s 并每 5s 打印进度；失败分级日志（用户取消静默、其它
  失败记 WARN）；显示后校验 `GLFW_VISIBLE`。已在 Ubuntu 26（Wayland）真机验证：拖选、确认后
  覆盖层立即消失、保存 + 剪贴板；Windows / X11 行为不变（X11 也补了隐藏后 poll）。
- Linux X11 三插件分支：`screen_capture`（XShm/XGetImage + XFixes）、`region_selector`
  （GLFW 覆盖层 + `XGetImage` / XComposite 背景快照）、`hotkey`（`XGrabKey`）；已在
  Ubuntu 24.04（X11，单显示器）真机验证（全屏采集、区域框选、热键）；多显示器 / 多 X screen /
  负坐标未验证。

### 变更

- 项目品牌统一为 **Plugin-shot**；配置与日志目录改为 `%LOCALAPPDATA%\plugin-shot`。
- 示例处理器（反色 / 灰度）默认禁用，避免首次运行截图被示例改写。
- 长截图（`long_capture`，Windows）作为实验特性随包分发，默认在插件列表里禁用（`plugins.disabled`），在设置页勾选启用后重启生效；已有配置在启动时自动迁移到 `config.schema=2`。
- 许可证确定为 **Apache-2.0**。

### 已知限制

- **预编译包仅提供 Windows x64**；Linux 已真机验证（单显示器）但本版本不提供预编译包，需按源码构建；macOS 未验证。
- 长截图（`long_capture`，Windows）为实验特性，真机完整交互已验证（选窗口 → 滚动 → HUD → 保存）；默认禁用，启用方式见上。
- 录屏、标注、OCR、上传尚未实现。
- 设置页文件名模板等少量文案仅提供中文/英文，其它语言回退到英文。
