---
name: plugin-shot-dev
description: Use when the user wants to add a feature to Plugin-shot (the plugin-based screenshot tool) or to write/build/debug/package a Plugin-shot plugin: screenshot watermark or timestamp, annotation-free pixel effects, resize, auto-archive by date, clipboard copy, notification, tray command, global hotkey action, configurable options, custom image saver, or a lib provider such as OpenCV. Works without the host source tree — download the official offline SDK (plugin-shot-sdk-<ver>-win64.zip), scaffold from a template, implement, build with CMake+Ninja, self-test with `Plugin-shot.exe --test-plugin`, then install into the host only with the user's consent.
---
# Plugin-shot 插件开发（离线 SDK）

把一句话需求做成一个可安装的插件 DLL：**下载官方离线 SDK → 从模板生成工程 → 写代码 → 编译 →
用宿主自检模式验收 → 征得同意后装进宿主**。默认假设本机**没有宿主源码**，只有宿主程序
（`Plugin-shot.exe` 与 `plugins/`）、CMake、MSVC 和网络。

插件与宿主**同进程**、拥有当前用户全部权限：生成代码前说明风险，安装进真实宿主前**必须**
得到用户确认。

## 0. 先判断能不能做（30 秒）

| 用户想要                                            | 用哪种能力/机制                                                                 | 模板                                                 |
| --------------------------------------------------- | ------------------------------------------------------------------------------- | ---------------------------------------------------- |
| 截图后改像素：时间水印/打码/模糊/调色/加边框        | `shot.image_processor`（原地改像素）                                          | `image_processor`                                  |
| 截图后做别的事：归档/复制/上传/通知/跑命令/缩图另存 | 订阅 `capture.done`（Generic 插件）                                           | `generic`                                          |
| 换保存格式/目录/命名（替换内置保存器）              | `shot.image_saver`                                                            | `image_saver`                                      |
| 加托盘菜单项或"功能热键"                            | 在插件 `create` 里发 `lists.add`（`tray_command_list` / `hotkey_list`） | `generic` 片段                                     |
| 加设置项（自动出现在设置页）                        | `shot.configurable`                                                           | 任意模板                                             |
| 把 OpenCV 等运行库带进进程                          | `shot.lib_provider`                                                           | `lib_provider`                                     |
| 自定义窗口/覆盖层/托盘、替换抓屏                    | `shot.ui_provider` / `shot.screen_capture`                                  | 宿主源码库中才有模板，克隆宿主源码库，`setting_ui` |

**做不了 / 要改宿主源码的**（不要写代码，按话术向用户解释）：

| 需求            | 结论                                                            |
| --------------- | --------------------------------------------------------------- |
| 录屏/动图/多帧  | ❌ 契约当前只支持单帧采集                                       |
| 上传到图床/网盘 | ⚠️ 能写但不"一句话"：需要地址/凭据/重试策略，且以用户权限联网 |

拿不准先读 [能力决策表](references/capability-decision.md)（含事件 payload 与边界话术）。
需求含糊（存到哪？要不要界面？）只问**阻塞性**问题，最多 2 个；其余给默认值并在交付说明写清。

成功标准（全部满足才算完成）：编译 exit 0；`--test-plugin` 无 FAIL；DLL 只导出
`shot_module_entry`；宿主 `--capture` 冒烟 exit 0 且能看到插件效果；交付说明含安装/验证/卸载/限制。

## 1. 环境检查

```bash
cmake --version            # 需要 >= 3.20（SDK 与宿主同口径；建议 3.28+）
ninja --version            # 或 cmake --build 自动调用的 Ninja
# Windows：cl 不在 PATH 是正常的，构建前用 vcvars64.bat 进 MSVC 环境
#   "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
# Linux：g++ --version（GCC 13+ / Clang 16+）
```

缺工具链时自己下载免安装版GCC/MSVC+CMake+Ninja

## 2. 获取并校验 SDK

```bash
# 优先用环境变量指定的 SDK；其次本地缓存；最后从 GitHub Releases 下载
# Windows: %PLUGIN_SHOT_SDK%          Linux/macOS: $PLUGIN_SHOT_SDK
# 缓存目录: Windows %USERPROFILE%\.plugin-shot-sdk\<ver>\   Linux ~/.plugin-shot-sdk/<ver>/
# 资产: plugin-shot-sdk-<ver>-win64.zip (+ .sha256)
#       plugin-shot-sdk-<ver>-win64-opencv.zip (+ .sha256)  ← 只有用 OpenCV 才要
# URL:  https://github.com/explorer-exe-bilibili/Plugin-shot/releases/download/v<ver>/plugin-shot-sdk-<ver>-win64.zip
```

1. 解析版本：`manifest.json` 的 `sdk_version`，或用户指定版本；**不要猜文件名**。
2. 下载后**先校验 sha256 再解压**（Windows `Get-FileHash -Algorithm SHA256`，Linux
   `sha256sum -c`）；校验失败删包重下一次，仍失败就停并报告 URL/哈希。
3. 解压后检查 `<SDK>/manifest.json` 存在、`abi.major == 2`，把 `sdk_version` / `abi` /
   `host_min_version` / `contract_sha256` 记进交付信息。SDK 根目录应含
   `include/ third_party/ cmake/ docs/ tools/ README.md manifest.json`。
4. 需要 OpenCV 时：把 `plugin-shot-sdk-<ver>-win64-opencv.zip` **解压到核心 SDK 根**
   （与 `include/` 同级出现 `opencv/`），`ShotPlugin.cmake` 会自动补头/库路径。
5. 解压到缓存目录，不要污染用户工程。下载/校验/离线回退的完整阶梯见
   [SDK 获取](references/sdk-acquire.md)。
6. GitHub无法连接成功则使用公共镜像/代理下载GitHub文件

## 3. 生成工程（模板 + 脚手架）

```bash
cmake -DTEMPLATE=generic \
      -DDEST="$WORK/acme_timestamp" \
      -DPLUGIN_ID=acme.timestamp \
      -DPLUGIN_NAME="ACME Timestamp" \
      -DPLUGIN_DESCRIPTION="在截图右下角加时间水印" \
      -DVERSION=1.0.0 \
      -DOWNER="ACME" \
      -P "<SDK>/tools/new_plugin.cmake"
```

- `TEMPLATE` ∈ `generic` / `image_processor` / `image_saver` / `lib_provider`；
  `DEST` 必须不存在或为空（覆盖要 `-DFORCE=ON`）。
- 占位符 `{{TARGET}} {{PLUGIN_ID}} {{PLUGIN_NAME}} {{PLUGIN_DESCRIPTION}} {{VERSION}} {{OWNER}}`
  由脚手架替换；`PLUGIN_ID` 含 `.` 时目标名自动变成下划线（`acme_timestamp`）。
- **先定 id 再写代码**：owner、i18n key、lists 贡献、配置 key 全部用它。
- 模板文件清单与"改哪里"见 [模板说明](references/templates.md)。

## 4. 实现（改这些文件）

| 文件                     | 改什么                                                                                                                                                                                      |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `metadata.json`        | `plugins[]` 顺序 = `create(index)` 顺序；`id/name/version`；`interfaces` 与 `query_interface` 实际回答一致；`requires` 写 `core>=<host_min>`，用 OpenCV 才加 `opencv>=4.12` |
| `src/plugin_entry.cpp` | **唯一导出 `shot_module_entry`**：模块表、能力目录、`create/destroy`；ABI 胶水只放这里                                                                                            |
| `src/*.cpp`            | 业务实现；图像按 `stride` 逐行写 `width*channels` 字节，不碰结构体字段                                                                                                                  |
| `src/lang_tables.cpp`  | 每语言一张单行 JSON（`@meta` 必填）；至少 `plugin.name` / `plugin.description` / `config.<key>.label` / `.tooltip`                                                                |
| `README.md`            | 功能、构建、配置表、验证步骤、限制（交付时原样给用户）                                                                                                                                      |

**线程与生命周期（硬要求）**：能力回调在采集 worker 线程；`set_config` 与事件回调在
main 线程；订阅在 `create`、退订在 `destroy`；自起线程必须 `join`；异常绝不跨 DLL 边界；
UI 插件 `deinit` 前必须停线程、销毁窗口并 `UnregisterClassW`。详见
[Authoring 速查](references/authoring.md) 与 SDK `docs/AUTHORING.md`。

## 5. 编译

Windows（先开一个 vcvars64 环境，再执行同样的 cmake 命令）：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT="<SDK 根>"
cmake --build build
```

Linux（**当前官方离线 SDK 只发布 win64**；在宿主源码仓库或自建 Linux SDK 时才可用）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT="$SDK"
cmake --build build -j
```

- 产物：`build/bin/plugins/<target>.dll`（Linux `.so`）；metadata 已贴到文件尾。
- `SHOT_SDK_ROOT` 也可用环境变量；不传时模板按 `../../manifest.json` 自动探测。
- 不要 `find_package(opencv)`、不要 vcpkg：SDK 自带 `third_party/include`（nlohmann / stb），
  OpenCV 走叠加包。
- 常见错误（C1083 / LNK2019 / metadata 校验失败 / LNK1104）见
  [构建与自检](references/build-and-test.md#3-构建失败分类)。

## 6. 自检（必须做）

```bash
"<宿主安装目录>/Plugin-shot.exe" --test-plugin "build/bin/plugins/acme_timestamp.dll" \
    --json-out report.json
# 退出码：0 = 无 FAIL；1 = 至少一条 FAIL；2 = 用法/IO 错误或超时
```

读 `report.json`：`result`（`pass`/`fail`）、`plugins[].checks[]`（`status` 为
`pass`/`fail`/`skip`/`warn` + `detail`）、`summary.{pass,fail,skip,warn}`。

- FAIL：按 `detail` 指向的原因改代码/元数据后重跑；同一问题 3 轮没进展就停下问用户。
- SKIP：不适用（如无显示器的机器测屏幕采集、UI 插件不做 UI 检查），不是失败。
- WARN：可容忍但应修（字段缺失、版本号不可解析等），交付说明里写清。
- 自检使用**隔离数据目录**，不碰用户真实配置；加 `--keep-data` 可保留目录排错。
- 选项与检查含义见 SDK `docs/AUTHORING.md`「自检」一节；`Plugin-shot.exe --test-plugin --help`
  可看完整用法。

## 7. 迭代修错

| 现象                               | 先查                                                           | 处理                                                |
| ---------------------------------- | -------------------------------------------------------------- | --------------------------------------------------- |
| configure 失败                     | `SHOT_SDK_ROOT` 是否传、SDK 是否解压完整                     | 修正路径；重新解压                                  |
| C1083 找不到 `nlohmann/json.hpp` | 是否真的走 SDK 模式（日志有 `ShotPlugin: SDK 模式`）         | 传 `-DSHOT_SDK_ROOT`，不要自己拷头文件            |
| C1083 找不到 `opencv2/...`       | 是否解压了 OpenCV 叠加包                                       | 解压 `opencv/` 到 SDK 根，或去掉 OpenCV 依赖      |
| LNK2019 `shot_module_entry`      | 导出宏/`extern "C"` 丢失、定义了 `SCREENSHOT_HOST_EXPORTS` | 照模板恢复                                          |
| metadata 校验失败                  | id 重复、`interfaces` 拼错、abi 不匹配、`requires` 语法    | 按报错改 metadata，不改契约                         |
| 宿主没加载                         | ABI major 不符、无 metadata、依赖缺失、架构不对                | 看 `host.log` 里 `plugin.error.*` / 失败原因行  |
| 加载了但没效果                     | 主题拼错、订阅晚于发布、返回非 0、路径不可写                   | 加 `shot::Logger` 日志；用 `--capture` 单次复现 |
| 宿主崩溃                           | 像素越界、异常跨边界、UI 窗口类未注销                          | 立即移除该 DLL；按排错章节修                        |

## 8. 安装到真实宿主（可选，先征得同意）

1. 备份目标 `plugins/`（只新增；必须覆盖同名 DLL 时先备份原文件）。
2. 复制 DLL 到**宿主 exe 所在目录的 `plugins/`**（不是 `bin/`；不要带任何 `opencv_*.dll`
   等基础依赖 DLL）。
3. 从宿主目录启动并冒烟：`Plugin-shot.exe --capture` → exit 0，日志有 `Capture saved:`。
4. 验收：`%LOCALAPPDATA%\plugin-shot\logs\host.log` 出现插件 id 与效果；`--settings` 能看到
   插件与配置项；新插件默认启用（只有 `plugins.disabled` 里的才禁用）。
5. 卸载：关闭宿主 → 删除该 DLL → 重启（必要时清理 `config.json` 里该插件的配置项）。

安装/验证/卸载与回滚话术见 [交付](references/deliver.md)。

## 9. 安全与硬约束

- 每个 DLL **只导出 `shot_module_entry`**；同一 DLL 可装多个插件，入口表顺序必须与
  metadata `plugins[]` 一致。
- **不打包** `opencv_*.dll`、`fmt`、`spdlog`、`glfw3` 等基础依赖——宿主 `bin/` 已有；
  需要 OpenCV 就声明 `requires`，依赖宿主已加载的 provider。
- 插件间**不直接互调**；跨插件/跨线程只走事件总线，跨插件数据走列表协议。
- 不写死本机路径/用户名；不偷偷联网、不读凭据、不写注册表、不修改宿主文件、不杀无关进程。
- 不把 ABI 结构体/宿主服务当扩展点：新数据形状 = 新 list id + JSON schema；新能力要改
  宿主（明确告诉用户）。
- `/MD` 且 Debug/Release 与宿主一致；不要 `/MT`，不要混用 release/debug 库。
- 破坏性操作（移动/覆盖/删除文件、上传）默认关闭且可配置。

## 10. 参考文件

- [能力决策表](references/capability-decision.md) — 需求→能力/事件/列表 + 边界话术
- [SDK 获取](references/sdk-acquire.md) — 版本/URL/缓存/sha256/离线回退
- [模板说明](references/templates.md) — 4 个模板、占位符、改哪里
- [Authoring 速查](references/authoring.md) — 骨架/metadata/config/i18n/lists/线程
- [构建与自检](references/build-and-test.md) — 命令、错误分类、`--test-plugin` 输出
- [交付](references/deliver.md) — 安装/验证/卸载、交付模板、风险提示

技能来源与安装：本文件随 Plugin-shot 仓库与离线 SDK 分发（仓库 `skills/plugin-shot-dev/`，SDK
`skill/plugin-shot-dev/`）。推荐安装方式是装仓库里的 DSH 插件 bundle **`dsh-plugin-shot-dev/`**
（设置 → 插件 → 添加插件 → 选择该目录，或按 DSH 的插件安装入口），它在
`skills/plugin-shot-dev/` 里带有同一份正文并注册技能 `plugin-shot-dev`；禁用/卸载插件即移除技能。
若你的 DSH 版本支持用户级技能目录，也可把整个 `plugin-shot-dev/` 目录放进去
（`%USERPROFILE%\.dsh\skills\`、`~/.dsh/skills/` 或工作区 `<workspace>/.dsh/skills/`），
重载 DSH 后技能列表出现 `plugin-shot-dev`。
