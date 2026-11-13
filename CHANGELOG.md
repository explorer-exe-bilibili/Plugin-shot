# 更新日志

## 0.1.0 — 2026-11-12

首个公开版本：Windows 10/11 x64 便携版。

### 新增

- 插件宿主：扫描、校验、依赖排序、加载、运行期启停与热卸载。
- 单入口 `shot_module_entry` + 自描述模块表，ABI 2.0（只追加）。
- 整屏截图与区域截图（物理虚拟桌面像素，混合 DPI 多屏）。
- 区域框选覆盖层：悬停窗口吸附、单击截窗、拖动选区，主题色折角。
- PNG / JPEG 保存，目录、格式、质量、文件名模板可配置。
- 截图后写入剪贴板（CF_DIB / 可选 CF_HDROP）与系统通知。
- 托盘入口、全局热键（可在设置页按组合键录入）、设置页。
- 设置页按插件声明自动生成表单；插件可启用/禁用，禁用即热卸载。
- 6 套主题色、27 个语言标签、外部语言包协议与翻译插件。
- 命令行 `--capture` / `--settings` / `--help`。
- 离线插件 SDK（`export_sdk` 目标）。
- 真机验证脚本：采集、光标合成、设置页、热键、剪贴板、区域框选、多语言。

### Linux（已实现、已编译，真机未验证）

0.1.0 仍只提供 Windows 构建；以下为 Linux 侧代码状态，尚未在真机验证。

- 托盘插件新增 Linux 后端：StatusNotifierItem + `com.canonical.dbusmenu`（GDBus / 系统
  `gio-2.0`，Ubuntu 需 `libglib2.0-dev`，不新增 vcpkg 依赖）；Windows 后端不变，macOS 仍跳过。
  Ubuntu GNOME 需要 Ubuntu AppIndicators 扩展（Ubuntu 默认自带；纯净 GNOME 需手动安装）；
  找不到 `org.kde.StatusNotifierWatcher` 时只 WARN 一次、不崩不重试；菜单数据仍来自
  `tray_command_list`，`lists.changed` 后重建。
- 区域框选修复 Wayland 隐藏/编码时序：隐藏覆盖层后先 `glfwPollEvents` 再编码，PNG 编码/写盘
  移入工作线程；portal 超时 120s → 30s 并每 5s 打印进度；失败分级日志（用户取消静默、其它
  失败记 WARN）；显示后校验 `GLFW_VISIBLE`。Windows / X11 行为不变（X11 也补了隐藏后 poll）。
- Linux X11 三插件分支：`screen_capture`（XShm/XGetImage + XFixes）、`region_selector`
  （GLFW 覆盖层 + `XGetImage` 快照）、`hotkey`（`XGrabKey`）；随 Linux 全量构建通过，真机未验证。

### 变更

- 项目品牌统一为 **Plugin-shot**；配置与日志目录改为 `%LOCALAPPDATA%\plugin-shot`。
- 示例处理器（反色 / 灰度）默认禁用，避免首次运行截图被示例改写。
- 长截图（`long_capture`，Windows）作为实验特性随包分发，默认在插件列表里禁用（`plugins.disabled`），在设置页勾选启用后重启生效；已有配置在启动时自动迁移到 `config.schema=2`。
- 许可证确定为 **Apache-2.0**。

### 已知限制

- 仅提供 Windows x64 构建；Linux / macOS 未在本版本提供可运行构建。
- 长截图（`long_capture`，Windows）为实验特性，真机交互待验证；默认禁用，启用方式见上。
- 录屏、标注、OCR、上传尚未实现。
- 设置页文件名模板等少量文案仅提供中文/英文，其它语言回退到英文。
