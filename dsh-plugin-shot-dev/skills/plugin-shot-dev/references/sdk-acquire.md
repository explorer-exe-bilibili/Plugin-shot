# SDK 获取、校验、缓存与版本兼容

> 本文件随 Plugin-shot 离线 SDK 分发。SDK 版本以 `manifest.json` 为准。

## 1. 先找现成的（按优先级）

```bash
# 1) 用户/CI 显式指定
echo "$PLUGIN_SHOT_SDK"          # Windows: %PLUGIN_SHOT_SDK%
# 2) 本机缓存
#    Windows %USERPROFILE%\.plugin-shot-sdk\<ver>\
#    Linux/macOS ~/.plugin-shot-sdk/<ver>/
# 3) 仓库内导出（只有维护者/宿主源码环境有）：<repo>/sdk/
```

有效 SDK 根的判据：目录内同时存在 `manifest.json` 与 `cmake/ShotPlugin.cmake`。

## 2. 下载（GitHub Releases）

| 资产 | 用途 |
| --- | --- |
| `plugin-shot-sdk-<ver>-win64.zip` | 核心包：契约头 + helpers + third_party + cmake + docs + templates + skill + tools |
| `plugin-shot-sdk-<ver>-win64.zip.sha256` | 校验文件（`<hash>  <文件名>` 格式） |
| `plugin-shot-sdk-<ver>-win64-opencv.zip` | 可选叠加包：`opencv/` 目录（头 + lib{/debug} + bin{/debug}） |

```text
https://github.com/explorer-exe-bilibili/Plugin-shot/releases/download/v<ver>/plugin-shot-sdk-<ver>-win64.zip
https://github.com/explorer-exe-bilibili/Plugin-shot/releases/download/v<ver>/plugin-shot-sdk-<ver>-win64-opencv.zip
```

- **不要猜最新版本号**：先查 release 信息（GitHub API 或用户给的版本）；下载失败时退回
  "让用户提供 zip/路径"，不要静默改装。
- 只有插件需要 OpenCV（`#include <opencv2/...>` 或链接 `opencv_*`）时才下叠加包；
  Generic / processor（纯 std）/ saver（stb）不需要。核心包约 3–5 MB（zip），
  OpenCV 叠加包 zip 约 15 MB、解压约 58 MB。
- 解压发现顶层是 `plugin-shot-sdk-<ver>/` 时以它为 SDK 根；不要套两层目录。

## 3. 校验（先校验再解压）

```powershell
# Windows
Get-FileHash .\plugin-shot-sdk-<ver>-win64.zip -Algorithm SHA256
# 与 .sha256 文件内容比对；不一致就删包重下一次
```

```bash
# Linux/macOS
sha256sum -c plugin-shot-sdk-<ver>-win64.zip.sha256
```

解压后再看 `manifest.json`：

```json
{
  "schema": 1, "kind": "core", "sdk_version": "0.1.0",
  "host_min_version": "0.1.0",
  "abi": { "major": 2, "minor": 0 },
  "contract_sha256": "…", "platform": "win64", "arch": "x64",
  "opencv_overlay": true, "files": [ { "path": "include/plugin_api.h", "sha256": "…" } ]
}
```

记录 `sdk_version` / `abi` / `host_min_version` / `contract_sha256` 到交付说明。
`opencv_overlay` 只表示该构建**支持**叠加包，不代表核心包里已有 `opencv/`。

## 4. 缓存与目录规范

```text
<用户目录>/.plugin-shot-sdk/<ver>/          ← 解压后的核心 SDK（只读使用）
<用户目录>/.plugin-shot-sdk/<ver>-opencv/   ← 叠加包解压临时目录，随后合并进 <ver>/
```

- 一个版本一个目录；**升级 SDK = 整体换目录 + 重编插件**，不要新旧头混用。
- 不要在 SDK 目录里构建插件（保持只读）；工程放用户自己的工作目录，构建目录在工程内。
- Windows 用户目录 = `%USERPROFILE%`；Linux/macOS = `~`。

## 5. 校验失败 / 下载失败的回退阶梯

| 情况 | 处理 |
| --- | --- |
| sha256 不匹配 | 删包重下一次；仍失败 → 停，报告 URL 与期望/实际哈希 |
| 404 / 版本不存在 | 查 release 列表确认版本；让用户给可用 URL 或本地 zip |
| 无网络 | 先查 `PLUGIN_SHOT_SDK` 与缓存；再问用户要 zip；最后才说明无法继续 |
| 只有仓库源码 | 维护者可用 `cmake --build <build> --target package_sdk` 生成 zip；第三方不可假设有源码 |
| SDK 里缺 `templates/` 或 `skill/` | 该构建未打全；用 SDK `tools/new_plugin.cmake` 前先确认模板存在，缺了就报告包损坏 |
| 需要 OpenCV 但只有核心包 | 下叠加包；或改用不依赖 OpenCV 的实现（纯 std / stb） |

## 6. 版本与兼容

三条版本轴，**不要混用**：

| 轴 | 值示例 | 载体 | 规则 |
| --- | --- | --- | --- |
| 宿主版本 | 0.1.0 | git tag / exe | 产品发布节奏 |
| SDK 包版本 | = 宿主版本 | zip 名 / `manifest.sdk_version` | 与宿主同步发布 |
| ABI 版本 | 2.0 | `plugin_api/basics.h`、`metadata.abi`、`ShotModule` | 追加 = minor+1；破坏 = major+1 |

- 加载硬条件：插件 `abi_major` 必须等于宿主（当前 2）；`abi_minor` 大于宿主只提示、照常加载。
- `metadata.abi` 构建期校验：major 相等且 minor ≤ SDK 头 minor；建议只写"实际需要的最低 minor"。
- `host_min_version` 是给作者/agent 的文档性门槛：插件 README 里写明"用 SDK x.y.z（ABI 2.0）
  构建，需要宿主 ≥ host_min_version"。
- 升级 SDK 后**整体替换 include/ 与 cmake/**，重新构建；旧插件不重编也能装进新宿主
  （append-only 契约保证）。
- Linux：官方离线 SDK 目前只导出 win64；Linux 宿主可以构建，但插件需要自建 SDK 或
  使用宿主源码仓库的 `include/`，且不能依赖 Windows 库。

## 7. 安全

- 只从官方 release 资产下载；校验哈希后才解压。
- 不要把 SDK 解压到系统目录或宿主安装目录；不要覆盖用户已有工程。
- 解压/下载失败要如实报告，不要用"看起来像"的第三方 SDK 冒充。
