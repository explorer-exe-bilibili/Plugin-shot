# 构建、自检与宿主冒烟

> 本文件随 Plugin-shot 离线 SDK 分发。命令按 SDK 模板工程（Ninja）验证。

## 1. 前提

| 项     | 要求                                                                    |
| ------ | ----------------------------------------------------------------------- |
| CMake  | ≥ 3.20（SDK/宿主同口径；建议 3.28+）                                   |
| 生成器 | Ninja（推荐）或 VS；多配置生成器请每配置一个 build 目录                 |
| 编译器 | MSVC v143+（VS2022/VS18，C++23）＋ Windows SDK；Linux GCC 13+/Clang 16+ |
| 运行库 | MSVC 必须 `/MD`（`shot_add_plugin` 已强制），与宿主一致             |
| 不要   | vcpkg、`find_package(opencv)`、宿主源码；OpenCV 才需要叠加包          |
| 磁盘   | ≥ 1.5 GB（SDK + 构建产物 + 可选 OpenCV）                               |

Windows 下 `cl` 不在 PATH 是正常的：先在 cmd 里执行一次 `vcvars64.bat`，再跑 cmake。
PowerShell 可 `cmd /c "call vcvars64.bat && cmake ..."`。

## 2. 构建命令

```bat
:: Windows（先 vcvars64，再执行）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT="<SDK>"
cmake --build build
```

```bash
# Linux/macOS（官方离线 SDK 目前只发布 win64，见 sdk-acquire.md §6）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT="$SDK"
cmake --build build -j
```

- 产物：`build/bin/plugins/<target>.dll`（Linux `.so`）；导入库/pdb 已剪枝。
- Debug：`-DCMAKE_BUILD_TYPE=Debug`；Debug 宿主必须配 Debug 插件，OpenCV 链接
  `opencv_*d` 变体（SDK 模块按 `$<CONFIG>` 自动选目录）。
- 重新配置：改 `CMakeLists.txt` 后 `cmake -S . -B build` 会增量重配；SDK 版本换目录
  时给新的 `-DSHOT_SDK_ROOT` 并删旧 build 目录。
- LNK1104 / 文件被占用：宿主正在运行或杀毒在扫；确认并关闭宿主后重试一次，不要删别人目录。
- 确认走的是 SDK 模式：configure 日志应有 `ShotPlugin: SDK 模式（根 ...）`。

## 3. 构建失败分类

| 现象（原文关键词）                                                           | 原因                                                                  | 修法                                                         |
| ---------------------------------------------------------------------------- | --------------------------------------------------------------------- | ------------------------------------------------------------ |
| `fatal error C1083: 无法打开包括文件: "nlohmann/json.hpp"`                 | 没走 SDK 模式，`third_party/include` 没进 include                   | 传 `-DSHOT_SDK_ROOT`；检查 SDK 是否完整；不要手工拷头      |
| `fatal error C1083: ... "stb_image_write.h"`                               | 同上（stb 未进 include）                                              | 同上；stb 是扁平文件名，不要放到子目录                       |
| `fatal error C1083: ... "opencv2/core.hpp"`                                | 没解压 OpenCV 叠加包                                                  | 把 `opencv/` 解压进 SDK 根                                 |
| `LNK1181: 无法打开输入文件"opencv_core4.lib"`                              | 只给了头没给库目录 / 没解压叠加包                                     | 解压叠加包；模板不用手写 link 目录                           |
| `LNK2019: 无法解析的外部符号 ... cv::debug_build_guard::...`               | Release/Debug 变体混用                                                | Debug 用 `opencv_*d` + `lib/debug`，整工程重配           |
| `LNK2019: 无法解析的外部符号 shot_module_entry`                            | 导出宏丢失 / 定义了 `SCREENSHOT_HOST_EXPORTS` / 没写 `extern "C"` | 照模板恢复；只导出这一个符号                                 |
| `LNK4098: 默认库"MSVCRT"与其他库的使用冲突`                                | `/MT` 与 `/MD` 混用                                               | 用 `shot_add_plugin`，它会设 `/MD`；删掉工程里的 `/MT` |
| configure 报 `ShotPlugin: SDK 模式` 缺失 / 找不到 `ShotPlugin.cmake`     | 没用模板的 SDK 定位逻辑                                               | 传 `-DSHOT_SDK_ROOT=<含 manifest.json 的目录>`             |
| `ValidatePluginMeta: ... id(...) 重复` / `interfaces 里的 id 不在契约里` | metadata 写错                                                         | 按报错改 metadata，不改契约头                                |
| `abi.major(1) 与契约头(2) 不一致`                                          | metadata 用了旧 ABI                                                   | 改成 SDK `manifest.abi`                                    |
| `这个工程还是模板`                                                         | 占位符没替换                                                          | 用 `new_plugin.cmake` 生成，或手工替换 `{{TARGET}}`      |
| `unknown pool name 'shot_link'`                                            | 极旧 SDK / 手工拼的 CMake                                             | 用 SDK 自带 `ShotPlugin.cmake`                             |
| `prune_output_dir ... 清理后仍存在违规文件`                                | 产物目录被外部占用                                                    | 关掉占用程序重试                                             |

## 4. 自检：`--test-plugin`

```bash
"<宿主安装目录>/Plugin-shot.exe" --test-plugin "build/bin/plugins/<target>.dll" \
    --json-out report.json
```

选项：`--json`（stdout 只输出 JSON，人类摘要进日志）、`--json-out <file>`、
`--keep-data`（保留隔离数据目录）、`--timeout-ms <n>`（默认 30000）、
`--language <tag>`（i18n 快照语言，默认 `en-US`）、`--verbose`。

退出码：**0 = 没有 FAIL；1 = 至少一条 FAIL；2 = 用法/IO 错误或超时**。

自检以真实宿主运行时加载被测 DLL：拷贝到隔离插件目录 → `PLUGIN_SHOT_DATA_DIR` 指向临时
目录 → 真实 HostRuntime 扫描/校验/依赖/加载 → 逐插件检查能力、配置项回写、列表、i18n、
事件 → 真实卸载并重载一次。**不读写用户真实配置/日志。**

### 4.1 读 JSON

```json
{
  "dll": "...", "copiedDll": "...", "dataDir": "...",
  "result": "pass",
  "summary": { "pass": 42, "fail": 0, "skip": 3, "warn": 1 },
  "checks": [ { "id": "host.init", "status": "pass", "detail": "..." } ],
  "plugins": [
    { "id": "acme.timestamp", "name": "...", "state": "loaded", "type": "image_processor",
      "declaredInterfaces": ["shot.image_processor"],
      "checks": [ { "id": "capability.image_processor.process", "status": "pass" } ] }
  ]
}
```

判定：`result` 或 `exitCode`；失败项取 `plugins[].checks[]` / `checks[]` 里
`status == "fail"` 的 `detail` 与检查 id，按 id 定位问题（如
`capability.image_processor.process`、`config.readback`、`lists.i18n_source`、
`module.export`）。`skip` 表示不适用（无显示器/UI 插件/GUI 副作用），不是失败；
`warn` 应修但不阻断，交付说明里列出。

### 4.2 常见 FAIL 与处理

检查 id 由自检工具生成，按前缀定位（以 `report.json` 实际 id 为准）：
`metadata.*`、`host.*`、`capability.*`、`config.*`、`list.*`、`events.*`。

| 检查（id 关键词）                | 原因                                                             | 修法                                                         |
| -------------------------------- | ---------------------------------------------------------------- | ------------------------------------------------------------ |
| `metadata.*`                   | 缺 metadata / JSON 不合法 / abi 不匹配                           | 修 `metadata.json`，重新构建（贴尾在 POST_BUILD）          |
| `host.init` / `host.*`       | 宿主在隔离目录里初始化失败 / 被测 DLL 让加载崩溃                 | 看 detail 与 `--keep-data` 留下的日志                      |
| `capability.image_processor.*` | process 返回非 0 / 改结构体字段 / 越界写填充                     | 只改像素、按 stride 写、返回 0                               |
| `capability.image_saver.*`     | 未落盘 / path 未写回 / 返回码不对                                | 返回最终绝对路径；path 不足返回 2                            |
| `capability.screen_capture.*`  | describe/capture 返回非 0 / 越界 / 全黑                          | 无桌面的机器会 SKIP；检查 stride 与几何                      |
| `capability.lib_provider.*`    | 库名/版本为空或不可解析                                          | 返回静态字符串的真实版本                                     |
| `config.*`                     | desc 的 key/label/kind 非法、get/set 不回读、未知 key 没返回负值 | 对齐 `ShotConfigDesc` 规则                                 |
| `list.*`                       | `lists.add` 的 owner 不是自己 / 条目缺 `id` / 写宿主生成列表 | owner = 插件 id；只写自己的 list 条目；不写 config/i18n list |
| `events.*`                     | `capture.done/failed` 试运行时插件回调崩溃                     | 回调里不要阻塞/越界；异常自行兜底                            |

自检修 3 轮仍无进展：停止，把 `report.json` 里 FAIL 条目与已尝试修改告诉用户。

## 5. 宿主冒烟（安装后）

```bat
cd /d "<宿主目录>"
Plugin-shot.exe --capture
```

验收：exit 0；日志出现 `Capture saved: <路径>`；`%LOCALAPPDATA%\plugin-shot\logs\host.log`
里出现插件 id、没有 `error`；`--settings` 能看到插件行与配置项。也可用宿主自带
`--test-plugin` 在安装前验收（推荐先自检再安装）。

## 6. 环境与编码注意

- Windows 控制台默认不是 UTF-8，宿主已设 UTF-8 代码页；日志文件是 UTF-8。
- 路径含中文/空格时整个 `-D` 参数加引号；不要用 PowerShell 的 `&&`（5.1 不支持）。
- 不要并行跑多个宿主实例：`config.json` 会被整文件覆盖、热键与托盘抢焦点。
- 构建/自检产物（`build/`、`report.json`）不要提交进用户工程仓库。
