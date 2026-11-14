# 交付：安装、验证、卸载、风险提示

> 本文件随 Plugin-shot 离线 SDK 分发。安装进真实宿主前**必须征得用户同意**。

## 1. 交付物清单

| 项 | 内容 |
| --- | --- |
| 插件 DLL | `build/bin/plugins/<target>.dll`（只这一个文件；不带任何基础依赖 DLL） |
| 工程源码 | 生成的工程目录（可交给用户继续改） |
| 自检报告 | `--test-plugin --json-out report.json` 的结果（`result` / `summary` / FAIL 清零） |
| 说明 | README（功能、配置项、限制）＋ 本对话里的"怎么验证/怎么卸载" |

交付信息里要写：SDK 版本 / ABI / `host_min_version` / `contract_sha256`（来自 manifest），
以及"未验证/依赖宿主源码"的边界。

## 2. 安装步骤（先问用户）

1. **备份**目标 `plugins/` 目录（至少记录将要新增/覆盖的文件名；必须覆盖同名 DLL 时先备份原文件）。
2. 复制 DLL 到**宿主 exe 所在目录的 `plugins/`**：
   - 不是 `bin/`，不是数据目录；
   - 不要拷 `opencv_*.dll` / `fmt` / `spdlog` 等基础依赖；
   - 新插件默认启用；只有 `config.json` 里 `plugins.disabled` 列出的才禁用。
3. 启动宿主（关闭旧实例后）。需要 OpenCV 的插件要确认宿主已带对应 provider DLL
   （依赖不满足时宿主只跳过这个插件，日志有原因）。
4. 冒烟：
   - `Plugin-shot.exe --capture` → exit 0，日志 `Capture saved: <绝对路径>`；
   - 打开设置页（`--settings` 或托盘）看到插件行与配置项；
   - 看 `host.log` 有插件 id、无 `error`。
5. 如果用户之前改过 `config.json`：测试后恢复备份，避免测试残留配置影响真实使用。

## 3. 用户如何验证（写进交付话术）

- "截图一次看效果"：`--capture` 或托盘"截屏"，看图片/目标目录/托盘/通知的变化。
- "设置页能看到/能改我的选项"：`--settings` → 对应插件一节。
- "禁用后行为消失"：设置页插件页取消勾选 → 应用 → 再截一张，效果不再出现。
- "日志里能确认加载"：`%LOCALAPPDATA%\plugin-shot\logs\host.log` 搜插件 id。

## 4. 卸载

1. 关闭宿主（确保进程退出）。
2. 删除 `plugins/<target>.dll`。
3. 重启宿主。（可选）删掉 `config.json` 里该插件 id 对应的配置键；不删也只是残留数据，
   不会导致加载。
4. 若装过多个插件包，只删本次新增的文件；不要删别人/内置的 DLL。

## 5. 风险提示（安装前说清楚）

- 插件是**进程内 DLL**，以当前用户权限运行：能读写文件、联网、读环境变量；
  只安装可信来源的插件。
- 插件与宿主没有沙箱隔离：像素越界写、异常跨边界、未注销窗口类都可能让宿主崩溃；
  自检通过不等于没有 bug。
- 依赖不匹配（ABI/宿主版本/Debug-Release、缺 provider）会导致插件被跳过或运行期崩溃；
  **Debug 插件只能进 Debug 宿主**。
- 覆盖同名 DLL、修改 `config.json`、移动/删除用户文件属于破坏性操作，必须先说明并得到同意；
  这类行为默认应做成可配置且默认关闭。
- 本次交付的验证范围：编译 + `--test-plugin` + `--capture` 冒烟；没有覆盖所有用户场景。

## 6. 交付话术模板（可直接改）

> 已给 Plugin-shot 写好插件 **<名称>**（id `acme.xxx`，用 SDK <ver> / ABI 2.0 构建）。
>
> - **做了什么**：截图保存后在右下角叠加时间水印；可在设置页改格式/字号/颜色（默认开启）。
> - **产物**：`<路径>/acme_watermark.dll`（安装到宿主 `plugins/` 即可）。
> - **怎么验证**：宿主目录运行 `Plugin-shot.exe --capture`，看新截图右下角；设置页 → <插件名>
>   一节可改配置；`host.log` 搜 `acme.watermark`。
> - **怎么卸载**：关宿主 → 删 `plugins/acme_watermark.dll` → 重启。
> - **自检结果**：`--test-plugin` exit 0（0 fail / N skip / N warn），详见 report.json。
> - **限制**：只处理 PNG/JPEG 落盘后的位图；不支持录屏；依赖宿主 ≥ <host_min>。
>
> 已按你的同意安装到 `<宿主目录>/plugins/` 并冒烟；需要我保留源码工程还是删除？

## 7. 回滚

- 安装前备份：`plugins/` 文件名清单 +（如有覆盖）原 DLL 与 `config.json`。
- 回滚 = 停宿主 → 恢复原 DLL / 删除新增 DLL → 恢复 config 备份 → 重启 → 再冒烟一次。
- 出现宿主崩溃/无法启动：先移除本次新增 DLL，确认宿主恢复正常，再报告复现步骤与日志，
  不要靠改宿主/系统设置绕过。
