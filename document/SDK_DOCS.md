# SDK 文档映射与防漂移

**目的**：离线 SDK（`plugin-shot-sdk-<ver>-win64.zip`）内的文档、模板、skill 必须与宿主仓库
源码同步。本文登记"仓库源文件 → SDK 内路径 → 处理方式"，以及发布前必须满足的防漂移条件。

单一事实源：`document/`（规范）+ `skills/`（技能）+ `tools/plugin_sdk/`（模板/脚手架）。
SDK 内文件是**发布产物**，不手工编辑；改内容改仓库源，再重新导出。

## 1. 映射表

| 仓库源 | SDK 内路径 | 处理方式 | 说明 |
| --- | --- | --- | --- |
| `document/README_SDK.md` | `README.md` | 原文拷贝 | SDK 总览/目录/构建/ABI 约定 |
| `document/AUTHORING.md` | `docs/AUTHORING.md` | 原文拷贝 | **插件开发单一事实源**（14 节教程） |
| `document/PLUGIN_TEST.md` | `docs/PLUGIN_TEST.md` | 原文拷贝 | `--test-plugin` 自检手册（检查项/JSON/退出码/局限） |
| `document/METADATA.md` | `docs/METADATA.md` | 原文拷贝 | metadata 字段/依赖/加载顺序 |
| `document/ABI_POLICY.md` | `docs/ABI_POLICY.md` | 原文拷贝 | 只追加规则、id/key 登记表 |
| `document/LISTS.md` | `docs/LISTS.md` | 原文拷贝 | 列表协议与各 list schema |
| `document/EVENT_BUS.md` | `docs/EVENT_BUS.md` | 原文拷贝 | 主题/线程/64 KiB |
| `document/CAPTURE.md` | `docs/CAPTURE.md` | 原文拷贝 | 采集/处理/保存流水线 |
| `document/I18N.md` | `docs/I18N.md` | 原文拷贝 | 语言表/两个 i18n 列表/回退链 |
| `document/SETTINGS.md` | `docs/SETTINGS.md` | 原文拷贝 | config_list、控件映射、提交语义 |
| `document/PLATFORMS.md` | `docs/PLATFORMS.md` | 原文拷贝 | 平台矩阵（含未验证项） |
| `include/plugin_api.h`、`include/plugin_api/`、`include/plugin_api_export.h` | `include/` | 原文拷贝 | 契约头，SDK 的 ABI 事实源 |
| `include/shot_plugin_helpers.h`、`include/shot_plugin_helpers/` | `include/shot_plugin_helpers/` | 原文拷贝 | 插件侧 C++ 封装 |
| `cmake/ShotPlugin.cmake` 等 5 个模块 | `cmake/` | 原文拷贝 | `shot_add_plugin()` 与构建期校验 |
| vcpkg 的 nlohmann-json 整树 | `third_party/include/nlohmann/` | 抽取拷贝 | 必须是整树，单 `json.hpp` 编不过 |
| vcpkg 的 `stb_*.h` | `third_party/include/` | 抽取拷贝（扁平） | 源码写 `"stb_image_write.h"` |
| `tools/plugin_sdk/licenses/` | `THIRD_PARTY_LICENSES/` | 原文拷贝 | nlohmann / stb 许可证全文 |
| 根 `LICENSE` / `NOTICE` | `LICENSE` / `NOTICE` | 原文拷贝 | 宿主与第三方声明 |
| `tools/plugin_sdk/templates/` | `templates/` | 整树拷贝 | `generic` / `image_processor` / `image_saver` / `lib_provider` |
| `tools/plugin_sdk/demos/` | `demos/` | 整树拷贝 | `watermark` / `archive` 完整示例工程 |
| `tools/plugin_sdk/new_plugin.cmake` | `tools/new_plugin.cmake` | 原文拷贝 | 纯 `cmake -P` 脚手架 |
| `skills/plugin-shot-dev/` | `skill/plugin-shot-dev/` | 整树拷贝 | `SKILL.md` + `references/`（6 份） |
| vcpkg OpenCV 头 | `opencv/include/opencv4/` | 抽取拷贝（可选叠加包） | 只有 `-DWITH_OPENCV=ON` 时导出 |
| vcpkg OpenCV 白名单 lib/dll | `opencv/lib{/debug}/`、`opencv/bin{/debug}/` | 白名单拷贝（可选叠加包） | release/debug 同名文件分目录 |
| — | `manifest.json` / `opencv/manifest.json` | 导出时生成 | 版本、ABI、`contract_sha256`、`files[]` 哈希 |

**不导出**（对第三方无意义或含宿主内部信息）：`document/BUILD_LINUX.md`、`AGENTS.md`、
`STYLE.md`、根 `README*.md`、`CHANGELOG.md`、`document/assets/`、`plugins/`（宿主自带插件
源码）、`src/`、`tools/verify/`、`build/`。

**待补齐（发布前确认）**：

- 文档版本页眉（见 §2.2）尚未由导出脚本注入；`docs/*.md` 缺失目前只 WARN，打包门禁
  只要求 ≥3 份。

## 2. 防漂移策略

### 2.1 `contract_sha256`（已实现）

`export_sdk.cmake` 用 `ReadContractHeader.cmake` 把 `plugin_api.h` + 全部子头拼成整份文本，
算 SHA-256 写进 `manifest.contract_sha256`。用途：

- 下载方/自检发现"契约头被改过"或"新旧头混用"；
- 比对文档/模板声称的 ABI 与 SDK 实际头文件；
- 发布记录里可复现"这份 SDK 对应哪版契约"。

规则：**契约头任何改动 → SDK 必须重新导出并升 `sdk_version`**（至少 PATCH；ABI 语义变化时
按 ABI 规则升 minor/major）。不允许只发新头不升版本。

### 2.2 版本页眉（待实现）

计划在导出时给每份 `docs/*.md` 与 `README.md` 注入一行页眉：

```text
> SDK <sdk_version> | ABI <major>.<minor> | contract sha256: <前 12 位> | <generated_at>
```

在页眉落地前，以 `manifest.json` 为版本事实源；文档正文里的 ABI/版本描述必须与
`manifest.json` 一致（发布检查清单会核对）。

### 2.3 单一事实源与"改一处"

| 内容 | 唯一事实源 | 同步方式 |
| --- | --- | --- |
| 契约 ABI/结构体 | `include/plugin_api/` | 源文件即 SDK 内容，导出拷贝 |
| metadata 语义 | `document/METADATA.md` | 导出拷贝 |
| 列表/事件/i18n/设置 | `document/LISTS.md` 等 | 导出拷贝 |
| 插件开发教程 | `document/AUTHORING.md` | 导出拷贝 |
| DSH 工作流 | `skills/plugin-shot-dev/SKILL.md` | 导出拷贝 |
| 模板工程 | `tools/plugin_sdk/templates/` | 整树拷贝 |
| SDK 布局/构建 | `cmake/export_sdk.cmake`（代码即事实） | 文档 §1 与本表必须同步 |
| 第三方版本/许可证 | vcpkg 安装树 + `tools/plugin_sdk/licenses/` | 导出抽取 |

禁止：在 SDK 里手工改文档/模板/契约头后不回写仓库；在 `document/` 里用"宿主仓库的
`document/xxx.md`"作为对第三方的引用（第三方拿不到）——第三方可见的引用一律指向
**SDK 内 `docs/`**（或 SDK 根 `README.md`）。`AUTHORING.md` 与 `SKILL.md` 已按此约束书写。

### 2.4 链接与可执行性约束

- SDK 内文档之间的链接用**同目录相对路径**（`METADATA.md`、`LISTS.md` …），因为
  `document/*.md` 与 `docs/*.md` 的相邻关系一致。
- `README_SDK.md` 里指向插件作者文档的链接用 `docs/xxx.md`（导出后 `README.md` 在 SDK 根）。
- `SKILL.md` 的 `references/*.md` 链接相对 `skills/plugin-shot-dev/`；SDK 内位置
  `skill/plugin-shot-dev/` 结构相同，链接仍然有效。
- 文档中的命令/路径必须与实际产物一致：`cmake -DSHOT_SDK_ROOT`、`build/bin/plugins/`、
  `tools/new_plugin.cmake` 参数、`--test-plugin` 选项与退出码。

### 2.5 构建期/发布期的门禁

| 门禁 | 位置 | 行为 |
| --- | --- | --- |
| `manifest.json` 可解析且 `sdk_version` / `abi.major` 与脚本参数一致 | `export_sdk.cmake` 已实现 | 不一致 FATAL |
| 契约头存在、`SHOT_ABI_*` 可读、`contract_sha256` 非空 | `export_sdk.cmake` 已实现 | 失败 FATAL |
| `docs/` 每份文件存在 | 当前缺失只 WARN（AUTHORING 未写完时的临时策略）；打包只要求 `docs/*.md` ≥ 3 份 | 发布前应升为逐文件 FATAL |
| `templates/` 至少一个模板目录 | `package_sdk.cmake` 已实现（<!-- -->`docs >= 3`、`templates >= 1`） | 缺失即打包失败 |
| `skill/plugin-shot-dev/SKILL.md` 存在且 frontmatter 合法 | **无自动门禁**（脚本只拷贝） | 发布检查清单人工核对 |
| 第三方 DLL 依赖闭包（`dumpbin /dependents` 白名单） | 打包门禁 | 防白名单漂移 |
| `include/plugin_api*` 与 `third_party/include` 非空 | 打包门禁 | 防"能 configure 编不过" |

## 3. 发布检查清单

导出/打包（维护者）：

- [ ] `document/README_SDK.md` 无"宿主仓库的 `document/`"式引用；插件作者文档链接指向 `docs/`
- [ ] `document/AUTHORING.md` 存在，且与 `include/` 契约、`cmake/ShotPlugin.cmake`、
      `tools/plugin_sdk/new_plugin.cmake` 的接口一致
- [ ] `skills/plugin-shot-dev/SKILL.md` frontmatter `name: plugin-shot-dev`、≤250 行、
      `references/` 6 份存在且无死链
- [ ] `templates/` 四个模板全部存在，`new_plugin.cmake` 能生成并编译
- [ ] `demos/` 两个示例（`watermark` / `archive`）已进 SDK 且能编译（缺失时导出日志有 WARN）
- [ ] `docs/PLUGIN_TEST.md` 已导出（自检手册与宿主 `--test-plugin` 行为一致）
- [ ] `manifest.json` 的 `sdk_version`、`host_min_version`、`abi`、`contract_sha256` 正确
- [ ] `THIRD_PARTY_LICENSES/`、`LICENSE`、`NOTICE` 存在
- [ ] `opencv/` 叠加包（如发布）：`manifest.kind == "opencv"`、release/debug 分目录、
      白名单外文件未混入
- [ ] 干净目录解压：`cmake -S templates/generic -B tmp -DSHOT_SDK_ROOT=<SDK>` 能配置
- [ ] 用宿主 `--test-plugin` 对一个模板产物自检 PASS（验证 SDK 与宿主 ABI 兼容）

文档修订（任何人改契约/行为时）：

- [ ] 改了 `include/plugin_api/` → 同步 `docs/ABI_POLICY.md` 登记表 + `abi_layout.h`
      `static_assert`，升 `SHOT_ABI_MINOR`/`MAJOR`
- [ ] 改了 metadata/列表/事件/i18n/设置语义 → 同步对应 `document/*.md`（即 SDK `docs/`）
- [ ] 改了插件开发流程/命令/模板参数 → 同步 `document/AUTHORING.md` 与
      `skills/plugin-shot-dev/`
- [ ] 改了 SDK 布局/导出规则 → 同步本文件 §1 与 `document/README_SDK.md`
- [ ] 重新导出 SDK 并复核 `contract_sha256` 变化（版本号必须跟着升）

## 4. 已知漂移点（截至本文）

1. `document/README_SDK.md` 仍保留部分"宿主仓库的 `document/xxx.md`"式引用与"nlohmann/stb
   自备"的旧说法，需在 SDK v2 文档改写时一并处理（本文件与 `AUTHORING.md` 已按新事实书写）。
2. `include/shot_plugin_helpers/shot_i18n.h` 头注释仍写"插件工程需要
   `find_package(nlohmann_json)`"；SDK 模式已内置 nlohmann（`third_party/include`），
   注释待宿主侧同步。
3. `demos/` 已由 `export_sdk.cmake` 拷贝进 SDK（`sdk/demos/` 并计入 manifest.files）；若某次
   导出日志出现"示例目录不存在"的 WARN，说明模板线还没产出，文档引用需回退到 `templates/`。
4. 文档版本页眉未注入（§2.2）；发布前以 `manifest.json` 为准。
