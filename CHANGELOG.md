# 更新日志

## 0.1.0 — 2026-11-14

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
