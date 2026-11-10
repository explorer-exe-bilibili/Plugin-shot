# tools/verify —— 真机端到端验证脚本

这些脚本是**给人在 Windows 桌面上跑**的验收工具，不是 CI 单元测试：它们会启动
`Plugin-shot.exe`、注入键鼠、读写剪贴板、接管前台窗口。

> 为什么放在仓库里：以前这些脚本散落在 `build/verify/`（gitignored），换机器/清工作区就丢，
> 后来接手的人只能靠猜。现在把**最常用的几个**固化在这里，其余一次性探针仍留在
> `build/verify/`（不入库）。

## 前置

1. 已构建：`bash tools/verify/build.sh`（或 `tools/verify/build.bat`）；
2. 有**交互式桌面**（不是 SSH/服务会话），否则截图、热键、剪贴板都不工作；
3. 运行期间**不要并行跑其它 e2e**（多个宿主会互相抢 `config.json` 和前台焦点）。

## 脚本

| 脚本 | 验证内容 | 依赖 |
| --- | --- | --- |
| `build.sh` / `build.bat` | 加构建锁后构建宿主 + 全部插件（`build.sh --target <t>` 可只构建一个） | 无 |
| `smoke_capture.ps1` | `--capture` 一次性截屏：exit 0、日志有 `Capture saved:`、落盘尺寸 = 物理虚拟桌面 | 采集/保存插件 |
| `smoke_settings.ps1` | `--settings`：设置窗口出现、可见、能截到画面、日志无 error | settings_ui |
| `e2e_hotkey_capture.ps1` | 常驻 + `Ctrl+Alt+S` → 采集落盘（hotkey + 流水线 + 保存 串联） | hotkey/screen_capture/image_saver |
| `e2e_clipboard.ps1` | 截图后系统剪贴板里有一张图（CF_DIB） | clipboard |

统一约定：

- 所有脚本都会 **先 `Stop-Host`**（杀掉已有的 `Plugin-shot.exe`）并**备份/恢复**
  `%LOCALAPPDATA%\screen-shot\config.json`；异常中断时手动跑一次 `Restore-Config`
  或把 `build\verify\config.backup.json` 拷回去。
- 产物（截图、`config.backup.json`）统一落 `build\verify\`（gitignored）。
- 脚本用 `Check <名字> <条件>` 打 `[PASS]/[FAIL]`，结尾打印 `结果：N/M 通过`，
  有失败时 exit 1。
- PowerShell 5.1 兼容：`.ps1` 是 **UTF-8 带 BOM**，不要用编辑器存成无 BOM。

## 新增一个验证脚本

1. 在脚本开头 `. (Join-Path $PSScriptRoot "common.ps1")`，并调用 `Initialize-PmAwareness`；
2. 用 `Stop-Host` / `Backup-Config` 做前置，用 `Start-Host` 启动宿主；
3. 断言用 `Check`，结束前 `Stop-Host` + `Restore-Config` + `Complete-Verification`；
4. 需要新的公共能力（找窗口、等日志、注入键鼠、读配置）就加到 `common.ps1`，不要复制粘贴；
5. 更新本表，并把脚本登记到仓库根 `AGENTS.md` §6.2。

## 这些脚本不覆盖什么

- 多显示器混合 DPI 的逐点坐标断言（曾用一次性探针测过，结论写在
  `plugins/region_selector/README.md` / `document/SETTINGS.md`）；
- 托盘菜单点击（需要 UI 自动化，一次性脚本在 `build/verify/tray_verify.ps1`）；
- 区域框选交互（需要模拟拖拽 + 覆盖层，涉及时序，一次性脚本在 `build/verify/`）。
