# Plugin-shot 插件开发 SDK

离线插件 SDK：**不装 vcpkg、不装 OpenCV、没有宿主源码**也能编译 Plugin-shot 插件。
当前只覆盖 **Windows / x64**（MSVC + Ninja；宿主本身支持 Linux，但 SDK 导出目前只有 Windows）。

> 这份文档是 SDK 说明的**唯一源文件**（仓库内 `document/README_SDK.md`）。`export_sdk`
> 导出时把它复制为 `<SDK>/README.md`，同时在 `<SDK>/docs/README_SDK.md` 留一份。
> 文中所有 `docs/xxx.md`、`cmake/xxx.cmake`、`templates/xxx` 路径都相对 SDK 根目录。

---

## 0. 30 秒上手

```bat
:: 1) 解压核心包（例：C:\sdk\plugin-shot-sdk-0.2.0-win64\）
:: 2) 需要 OpenCV 的插件才叠加：把 opencv 包里的 opencv\ 整个放进核心 SDK 根
:: 3) 从模板生成一个插件工程
"D:\src\cmake\bin\cmake.exe" -DTEMPLATE=generic ^
    -DDEST=%USERPROFILE%\src\acme_foo ^
    -DPLUGIN_ID=acme.foo -DPLUGIN_NAME="ACME Foo" ^
    -P C:\sdk\plugin-shot-sdk-0.2.0-win64\tools\new_plugin.cmake

:: 4) 构建（Release；Debug 换 -DCMAKE_BUILD_TYPE=Debug）
"D:\src\cmake\bin\cmake.exe" -S %USERPROFILE%\src\acme_foo -B %USERPROFILE%\src\acme_foo\build ^
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=C:\sdk\plugin-shot-sdk-0.2.0-win64
"D:\src\cmake\bin\cmake.exe" --build %USERPROFILE%\src\acme_foo\build

:: 5) 产物：acme_foo\build\bin\plugins\acme_foo.dll
```

不需要 vcpkg、不需要网络、不需要 Python / PowerShell 脚本——只有 CMake + Ninja + MSVC。

---

## 1. 两个发布包：核心包 + 可选 OpenCV 叠加包

SDK 从 v2 起拆成两个可独立下载的 zip（同一版本、同一次发布）：

| 包 | 文件 | 内容 | 什么时候需要 |
| --- | --- | --- | --- |
| **核心包** | `plugin-shot-sdk-<版本>-<平台>.zip`（约 0.8 MB） | 契约头、插件侧工具头、第三方头（nlohmann-json / stb）、CMake 构建模块、文档、模板、脚手架、许可证 | **所有插件**（Generic / 配置 / 订阅者 / 图像处理/保存不含 OpenCV 时） |
| **OpenCV 叠加包** | `plugin-shot-sdk-<版本>-<平台>-opencv.zip`（约 13 MB） | `opencv/` 目录：OpenCV 4.12 头 + 白名单导入库（Release/Debug）+ 运行时 DLL（Release/Debug） | 插件源码里 `#include <opencv2/...>` 或链接 `opencv_*` 时 |

拆包的原因：大多数插件根本不需要 OpenCV，不应该被迫下载全部 56 MB 的 OpenCV 头/库/DLL。
核心包只保证"契约 + 构建 + 模板"可用；OpenCV 是**可选叠加**，不叠加时 `ShotPlugin.cmake`
连 `opencv/` 都不会看。

本 SDK 版本：**sdk_version = 0.2.0**，契约 **ABI 2.0**（`major=2` / `minor=0`），
宿主最低版本 **0.2.0**。这些值以 `<SDK>/manifest.json` 为准（字段说明见 §10）。

---

## 2. 下载与校验

发布资产共 4 个文件（每个 zip 配一个 `.zip.sha256`）：

```text
plugin-shot-sdk-0.2.0-win64.zip
plugin-shot-sdk-0.2.0-win64.zip.sha256
plugin-shot-sdk-0.2.0-win64-opencv.zip
plugin-shot-sdk-0.2.0-win64-opencv.zip.sha256
```

`.zip.sha256` 是标准 `sha256sum` 格式（`<64 位 hex><两个空格><文件名>`），任选一种校验：

```bat
:: Windows PowerShell
Get-FileHash .\plugin-shot-sdk-0.2.0-win64.zip -Algorithm SHA256

:: Windows cmd（老机器 / 无 PowerShell 7）
certutil -hashfile plugin-shot-sdk-0.2.0-win64.zip SHA256

:: Git Bash / Linux / macOS
sha256sum -c plugin-shot-sdk-0.2.0-win64.zip.sha256
```

拿输出和 `.zip.sha256` 里的第一列比较，一致才解压。

> **注意**：zip 本身**不可复现**（同一棵树两次打包，zip 的 sha256 不同，因为归档里带时间戳）。
> 所以发布以"实际上传的 zip"的 sha256 为准；SDK **内容**的完整性另由 `manifest.json`
> 的 `files[]`（每个文件的 sha256）保证，不依赖 zip 哈希。

---

## 3. 解压与可选 OpenCV 叠加

两个包都是**单根目录** zip（解压不会把文件撒到当前目录）：

```text
plugin-shot-sdk-0.2.0-win64\              ← 核心包解压后 = SDK 根目录
plugin-shot-sdk-0.2.0-win64-opencv\       ← OpenCV 叠加包解压后
    opencv\                               ← 只有这一个目录，把它整个移进 SDK 根
    README-OPENCV.txt
```

叠加完成后的结构（与核心包自己的目录同级）：

```text
plugin-shot-sdk-0.2.0-win64\
├── manifest.json
├── ...
└── opencv\                               ← 叠加后才有
    ├── manifest.json
    ├── include\opencv4\...
    ├── lib\*.lib          lib\debug\*.lib
    └── bin\*.dll          bin\debug\*.dll
```

`ShotPlugin.cmake` 的 SDK 模式按 **`<SDK>/opencv/manifest.json` 是否存在**判断要不要补
OpenCV 头/库路径：没叠加的 SDK 构建 OpenCV 插件会直接报 `C1083: opencv2/core.hpp`——
那说明你确实需要叠加包，不是 SDK 坏了（见 §15 故障排查）。

---

## 4. SDK 目录树（核心包）

```text
plugin-shot-sdk-0.2.0-win64/
├── manifest.json                 # 机器可读清单：版本 / ABI / 契约哈希 / 文件哈希（§10）
├── README.md                     # 本文档
├── LICENSE                       # 宿主许可证（Apache License 2.0 全文）
├── NOTICE                        # 第三方组件清单
├── THIRD_PARTY_LICENSES/         # 分发的第三方许可证全文
│   ├── nlohmann-json.MIT.txt
│   └── stb.LICENSE
├── include/                      # ★ 契约头 + 插件侧工具头（导出即契约，勿手改）
│   ├── plugin_api.h              #   契约汇总头（只 include，插件只包含它）
│   ├── plugin_api/               #   契约正文，按主题一个子头（13 个）
│   ├── plugin_api_export.h       #   跨边界导出宏 SCREENSHOT_API
│   ├── shot_plugin_helpers.h     #   插件侧工具汇总头
│   ├── shot_plugin_helpers/      #   Logger / Config / EventBus / PluginLists / I18n …（7 个）
│   └── shot_semver.h             #   语义化版本比较工具
├── third_party/include/          # ★ SDK 内置的 header-only 依赖（§8）
│   ├── nlohmann/                 #   nlohmann-json 3.12.0 完整多文件树
│   └── stb_*.h                   #   stb（扁平文件名，源码写 "stb_image_write.h"）
├── cmake/                        # ★ 插件构建模块（宿主仓库同一份，SDK 模式内置）
│   ├── ShotPlugin.cmake          #   shot_add_plugin()：/MD、隐藏符号、贴元数据、剪枝
│   ├── AppendPluginMeta.cmake
│   ├── ValidatePluginMeta.cmake
│   ├── ReadContractHeader.cmake
│   └── prune_output_dir.cmake
├── docs/                         # 插件作者文档（§9）
│   ├── README_SDK.md             #   本文档副本
│   ├── AUTHORING.md              #   SDK 专用入门手册（建议先读）
│   ├── METADATA.md  LISTS.md  EVENT_BUS.md  CAPTURE.md
│   ├── ABI_POLICY.md  I18N.md  SETTINGS.md  PLATFORMS.md
├── templates/                    # 可直接编译的工程模板（§6.4）
│   ├── generic/  image_processor/  image_saver/  lib_provider/
├── demos/                        # 完整示例工程（watermark / archive，可直接编译）
├── skill/plugin-shot-dev/        # DSH skill（让 AI 助手按 SDK 约定写插件）
└── tools/
    └── new_plugin.cmake          # 脚手架：模板 + 占位符替换（§6.1）

叠加 OpenCV 后另有一个目录：opencv/（include/lib/bin/manifest.json，见 §3）。
```

---

## 5. 环境要求

| 项 | 要求 | 说明 |
| --- | --- | --- |
| 操作系统 | Windows 10/11 x64 | 当前 SDK 只导出 Windows（`win64` / `x64`） |
| 编译器 | MSVC v143 及以上（VS 2022/2026） | 宿主与插件都用 `/MD` 动态 CRT |
| CMake | **≥ 3.20**（建议 3.24+） | 构建模块与模板按 3.20 写；`cmake -E tar` 打 zip 也在 3.20+ |
| 生成器 | **Ninja**（推荐）或单配置 Makefiles | 多配置生成器（Visual Studio）请为 Debug/Release 各配一个构建目录 |
| Windows SDK | 随 VS 安装即可 | — |
| C++ 标准 | C++23 | 模板里已设 `CMAKE_CXX_STANDARD 23` |

**不需要**：vcpkg、OpenCV 安装、Python、PowerShell 执行策略、git、网络。
Debug 构建额外需要 OpenCV 叠加包里的 `opencv/lib/debug` 与 `opencv/bin/debug`。

---

## 6. 构建第一个插件（5 分钟）

### 6.1 用脚手架从模板生成

`tools/new_plugin.cmake` 是纯 CMake 脚本（不依赖 Python / PowerShell），用 `cmake -P` 运行：

```bat
"D:\src\cmake\bin\cmake.exe" ^
    -DTEMPLATE=generic ^
    -DDEST=%USERPROFILE%\src\acme_foo ^
    -DPLUGIN_ID=acme.foo ^
    -DPLUGIN_NAME="ACME Foo" ^
    -DPLUGIN_DESCRIPTION="我的第一个 Plugin-shot 插件" ^
    -DVERSION=1.0.0 ^
    -DOWNER="ACME" ^
    -P <SDK 根>\tools\new_plugin.cmake
```

参数：

| 参数 | 必填 | 缺省 | 说明 |
| --- | --- | --- | --- |
| `TEMPLATE` | ✔ | — | 模板名（`templates/` 下的子目录，如 `generic`）或模板绝对路径 |
| `DEST` | ✔ | — | 生成目录；已存在且非空时报错（`-DFORCE=ON` 才覆盖） |
| `PLUGIN_ID` | ✔ | — | 插件 id（全局唯一，建议 `domain.name`；会写进 `metadata.json`） |
| `PLUGIN_NAME` | — | = `PLUGIN_ID` | 界面显示名 |
| `PLUGIN_DESCRIPTION` | — | 空 | 描述 |
| `VERSION` | — | `1.0.0` | 插件版本 |
| `OWNER` | — | 空 | 作者 / 组织 |
| `FORCE` | — | `OFF` | `ON` = 允许写入非空 `DEST` |

脚手架把模板里的占位符 `{{TARGET}}` `{{PLUGIN_ID}}` `{{PLUGIN_NAME}}`
`{{PLUGIN_DESCRIPTION}}` `{{VERSION}}` `{{OWNER}}` 替换成实际值（文件内容与文件名都会替换；
模板里多写的 `{{别的}}` 通配替换为空串，不会让生成失败）。

### 6.2 构建

```bat
:: Release
"D:\src\cmake\bin\cmake.exe" -S %USERPROFILE%\src\acme_foo -B %USERPROFILE%\src\acme_foo\build ^
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=C:\sdk\plugin-shot-sdk-0.2.0-win64
"D:\src\cmake\bin\cmake.exe" --build %USERPROFILE%\src\acme_foo\build

:: Debug（换构建目录，别和 Release 混）
"D:\src\cmake\bin\cmake.exe" -S %USERPROFILE%\src\acme_foo -B %USERPROFILE%\src\acme_foo\build-debug ^
    -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSHOT_SDK_ROOT=C:\sdk\plugin-shot-sdk-0.2.0-win64
"D:\src\cmake\bin\cmake.exe" --build %USERPROFILE%\src\acme_foo\build-debug
```

> 需要 MSVC 环境（`cl.exe` / `link.exe` 在 PATH 上）。从普通 cmd 里构建前先执行
> `"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"`
> （或从 "x64 Native Tools Command Prompt" 打开）。SDK 本身不捆绑编译器。
>
> `-DSHOT_SDK_ROOT` 也可以用环境变量 `SHOT_SDK_ROOT` 代替；工程模板会依次尝试
> `-D` 参数 → 环境变量 → （模板还留在 SDK 内时的）`../../manifest.json`。

### 6.3 产物与验证

- 插件 DLL：`<构建目录>/bin/plugins/<目标名>.dll`
- 插件导入库：`<构建目录>/lib/`；pdb 已被构建脚本剪枝（不随产物发布）
- 构建成功后 DLL 尾部会带 `metadata.json` 全文（`---PLUGIN_META_START---` /
  `---PLUGIN_META_END---` 标记），宿主扫描时**不加载 DLL** 就能读出插件清单。
- 构建期还会跑 `ValidatePluginMeta`：`abi.major` 必须与 SDK 契约头一致、`abi.minor`
  不得比 SDK 新、`interfaces` 里的能力 id 必须真实存在——写错了在链接后立刻失败。

### 6.4 模板清单

| 模板 | 用途 | 额外依赖 |
| --- | --- | --- |
| `generic` | 纯配置项 + 事件订阅 + 托盘命令（Generic 插件，无具名能力） | 无 |
| `image_processor` | `shot.image_processor`（逐像素滤镜 / 水印） | 无（纯 std 实现） |
| `image_saver` | `shot.image_saver`（PNG/JPEG 落盘） | 内置 stb |
| `lib_provider` | `shot.lib_provider`（把 OpenCV 之类运行库带进进程） | 可选 OpenCV 叠加包 |

每个模板目录里都有 `CMakeLists.txt` + `metadata.json` + `src/`（+ 可选 `include/`），
模板文件开头注释写明"哪些名字要改"。手工拷贝模板时，把 `{{TARGET}}` 等占位符改成实际值即可。

SDK 还附带两个**完整示例**（`demos/`，不是模板、没有占位符，可直接编译）：

| 示例 | 内容 |
| --- | --- |
| `demos/watermark` | 截图右下角加拍摄时间水印（`shot.image_processor` + 配置项，纯标准库、不用 OpenCV） |
| `demos/archive` | 按日期归档截图（Generic 插件 + 订阅 `capture.done`，含"防把自己归档走"） |

```bat
:: 直接编译示例（不用脚手架）
"D:\src\cmake\bin\cmake.exe" -S <SDK 根>\demos\watermark -B %TEMP%\wm-build ^
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=<SDK 根>
"D:\src\cmake\bin\cmake.exe" --build %TEMP%\wm-build
```

---

## 7. `shot_add_plugin()` 与 SDK 模式

插件工程侧只需要两行接入（之后所有路径都由 SDK 自己算）：

```cmake
list(APPEND CMAKE_MODULE_PATH "${SHOT_SDK_ROOT}/cmake")
include(ShotPlugin)
shot_add_plugin(acme_foo
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp src/foo.cpp
    INCLUDE_DIRS include                              # 可选
    LINK_LIBRARIES opencv_core4                       # 可选；Debug 见下
)
```

`ShotPlugin.cmake` 会统一处理：产物名 = 目标名、隐藏符号（只导出
`shot_module_entry`）、MSVC `/MD`、动态库落 `<build>/bin/plugins`、导入库落
`<build>/lib`、构建后校验 metadata 并贴到 DLL 尾部、剪枝 pdb/ilk/exp。

**SDK 模式**（判据：`<ShotPlugin.cmake 所在目录>/../manifest.json` 是否存在）在本 SDK
里自动补三件事：

1. `<SDK>/third_party/include` 进 include 路径 —— nlohmann-json / stb 开箱即用（§8）；
2. 若叠加了 OpenCV：`<SDK>/opencv/include/opencv4` 进 include 路径；
3. 若叠加了 OpenCV：按配置给**链接目录**（per-target，不用全局 `link_directories`）：
   `Debug → <SDK>/opencv/lib/debug`，其余 → `<SDK>/opencv/lib`。

所以工程里**不要**再写死 `<SDK>/lib` 或 `include/opencv4`，也不要 `find_package(OpenCV)`。
链接 OpenCV 库时**库名要按配置切换**（SDK 只给目录，库名是插件自己的选择）：

```cmake
shot_add_plugin(acme_cv
    METADATA metadata.json
    SOURCES  src/plugin_entry.cpp
    LINK_LIBRARIES
        "$<$<CONFIG:Debug>:opencv_core4d>"
        "$<$<NOT:$<CONFIG:Debug>>:opencv_core4>"
)
```

同一个 `ShotPlugin.cmake` 也服务宿主仓库内的构建：宿主仓库根没有 `manifest.json`，
SDK 模式判为关，13 个内置插件的行为完全不变。

---

## 8. 第三方依赖：已内置，不要自己装

`shot_plugin_helpers.h` 的 `shot::I18n` 无条件包含 `<nlohmann/json.hpp>`；写 PNG/JPEG
的插件（`image_saver` 类）用 stb。这两个库**已经随核心包分发**，并且 SDK 模式自动加
include 路径，插件工程**不要**再 `find_package(nlohmann_json)` / `find_package(Stb)`：

| 库 | 版本 | 位置 | 源码里的写法 | 许可证 |
| --- | --- | --- | --- | --- |
| nlohmann-json | 3.12.0 | `third_party/include/nlohmann/`（完整多文件树） | `#include <nlohmann/json.hpp>` | MIT（`THIRD_PARTY_LICENSES/nlohmann-json.MIT.txt`） |
| stb | 2024-07-29 | `third_party/include/stb_*.h`（**扁平**文件名） | `#include "stb_image_write.h"` | MIT / public domain 双许可（`THIRD_PARTY_LICENSES/stb.LICENSE`） |

不要只拷 `json.hpp` 一个文件：vcpkg 的 `json.hpp` 是多文件入口，单独拷会报
`C1083: 无法打开 "nlohmann/adl_serializer.hpp"`。也不要给 stb 建 `stb/` 子目录：
stb 的头是按扁平文件名互相引用的。

插件要引入**别的** header-only 库时：自己放进插件工程（例如 `third_party/` 目录 +
`INCLUDE_DIRS`），不要往 SDK 的 `include/` 里塞——`include/` 是"导出即契约"，只放
宿主发布的契约头。

---

## 9. 文档索引（`docs/`）

SDK 自带插件作者需要的设计文档（完整列表见 §4 目录树）：

| 文档 | 什么时候读 |
| --- | --- |
| `docs/AUTHORING.md` | **先读这个**：SDK 专用入门手册，从模板到能力实现的全流程 |
| `docs/PLUGIN_TEST.md` | 宿主 `--test-plugin` 自检手册：检查项、JSON、退出码、局限 |
| `docs/METADATA.md` | `metadata.json` 每个字段的语义与校验规则 |
| `docs/ABI_POLICY.md` | interface id / 配置 key 登记表；什么算破坏性变更 |
| `docs/LISTS.md` | 给宿主/设置页/托盘提供数据：list id + JSON schema + `lists.*` 事件（"数据形状不进 ABI"铁律） |
| `docs/EVENT_BUS.md` | 事件主题、线程模型（订阅回调只在宿主 main 线程执行） |
| `docs/CAPTURE.md` | 采集/处理/保存流水线与线程语义；三类能力插件必读 |
| `docs/I18N.md` | 多语言：语言表、`i18n_list`、`shot::I18n` |
| `docs/SETTINGS.md` | `shot.configurable` 在设置页的渲染与应用语义 |
| `docs/PLATFORMS.md` | 宿主支持矩阵（写平台守卫前先看） |

---

## 10. `manifest.json` 与版本 / ABI 兼容

`<SDK>/manifest.json` 是 SDK 的机器可读身份，主要字段：

| 字段 | 例 | 含义 |
| --- | --- | --- |
| `schema` | `1` | 清单格式版本 |
| `kind` | `"core"` | `core` = 核心包；`opencv` = 叠加包自己的清单 |
| `sdk_version` | `"0.2.0"` | SDK 包版本（与宿主版本同步） |
| `host_min_version` | `"0.2.0"` | 首次携带该 ABI 的宿主版本，插件 README 可据此写要求 |
| `abi.major` / `abi.minor` | `2` / `0` | 契约头里的 `SHOT_ABI_MAJOR` / `SHOT_ABI_MINOR` |
| `contract_sha256` | 64 位 hex | 契约头整份文本（汇总头 + 全部子头）的 SHA256 |
| `platform` / `arch` | `win64` / `x64` | 目标平台 |
| `opencv_overlay` | `true`/`false` | 导出时是否带了 OpenCV 叠加包 |
| `generated_at` | `2026-11-14T15:23:56Z` | 导出时间（UTC） |
| `files[]` | `[{path, sha256}, …]` | 关键文件（include / cmake / third_party / docs / templates / tools …）的相对路径与哈希 |

`opencv/manifest.json` 额外有 `version`（OpenCV 版本，如 `4.12.0`）。

### ABI 兼容规则

1. **加载硬条件**：宿主要求插件的 `abi_major` 与自己的 `SHOT_ABI_MAJOR` **相等**，
   否则整包拒绝加载。**2.0 与 2.x 之间可以加载**（append-only 契约）。
2. **minor 只增不改**：契约只在结构体末尾追加字段；新 minor 的能力要先用
   `SHOT_HAS_FIELD` / `query_interface` 判空，不能假设老宿主有。
3. **metadata.abi 建议写"实际需要的最低版本"**：`abi.major` 必须 = SDK 头 major，
   `abi.minor` ≤ SDK 头 minor。盲目追新会让插件在老宿主上更脆弱。
4. **`contract_sha256` 是"新旧头混用"的报警器**：插件工程若引用了与 SDK 不一致的
   契约头（例如从别处拷了旧头），哈希对不上；排查 ABI 相关怪问题时先对一下它。
5. **升级 SDK = 整体替换 SDK 目录 + 重新编译插件**，不要新旧头混用。
   旧 SDK 编的插件在新宿主上仍能加载（major 相同即可），反之亦然（新插件在老宿主上
   能力降级，不会崩）。

---

## 11. 离线自检：`--test-plugin`

SDK 本身只保证"能编译 + 产物结构正确"。要验证宿主**真的会怎么加载它**，用宿主可执行
文件（与 SDK 同版本；从宿主发布包获取，SDK 不包含 `Plugin-shot.exe`）的自检模式：

```bat
Plugin-shot.exe --test-plugin C:\path\to\acme_foo.dll --json-out report.json
```

它会走**真实宿主加载路径**：扫描 metadata、校验 ABI/依赖、加载 DLL、创建实例、逐项
检查能力/配置/列表/事件，跑完把宿主卸载掉；配置、日志、插件贡献都隔离在临时目录，
**不会**读写你正在用的 Plugin-shot 配置。

| 选项 | 说明 |
| --- | --- |
| `--test-plugin <dll>` | 被测插件（必填） |
| `--json-out <file>` | 把完整报告写成 JSON（CI / AI 助手解析用） |
| `--json` | stdout 只输出 JSON（人类摘要进日志） |
| `--keep-data` | 保留隔离数据目录（排错用） |
| `--timeout-ms <n>` | 整体超时，默认 30000；插件挂起时硬退出 |
| `--language <tag>` | i18n 快照使用的语言，默认 `en-US` |
| `--verbose` | 人类摘要里连 PASS/SKIP 明细一起打印 |

退出码：`0` = 没有失败；`1` = 至少一条检查失败（插件不符合契约）；`2` = 用法 / IO
错误或超时。`--json-out` 的报告里有 `result`（`pass`/`fail`）、`checks[]`
（`id` / `status` / `detail`）和按插件分组的 `plugins[]`，`status` 取
`pass` / `fail` / `skip` / `warn`。

典型 CI/skill 用法：

```bat
Plugin-shot.exe --test-plugin build\bin\plugins\acme_foo.dll --json-out build\selftest.json
if errorlevel 1 exit /b 1
```

没有宿主 exe 时，"编译期自检"的最小集合是：构建 exit 0 + DLL 尾部有
`---PLUGIN_META_START---` 标记 + `--test-plugin` 报告里的 `abi` 说明与 SDK 一致。

---

## 12. 安装到宿主

1. 把 `<构建目录>/bin/plugins/<目标名>.dll` 拷进宿主安装目录的 `plugins\`（与
   `Plugin-shot.exe` 同级的那个目录）。
2. 启动宿主，或对运行中的宿主热启用插件（设置页 / 托盘插件列表；宿主会重新扫描）。
3. 插件目录里**只放插件自己的 DLL**：不要拷 `opencv_*.dll`、VC 运行库或 SDK 里的
   任何基础依赖——它们由宿主安装目录（exe 旁边）提供。插件再带一份会造成"同一进程
   加载两个 OpenCV 实例"，类型信息/全局状态/内存分配器全部割裂，表现为莫名崩溃。
4. Debug 插件只能进 Debug 宿主，Release 同理（见 §13）。
5. 插件用 `/MD` 动态 CRT；目标机器需要 VC++ 运行库（宿主本身也依赖，装机通常已有）。

---

## 13. Debug / Release 与 DLL 依赖

- SDK 的 OpenCV 叠加包里有**两套完整变体**：
  `opencv/lib` + `opencv/bin`（Release）与 `opencv/lib/debug` + `opencv/bin/debug`（Debug）。
  Debug 的库名通常带 `d` 后缀（`opencv_core4d.lib`），但也有**文件名与 Release 完全相同**
  的依赖（`jpeg62.dll`、`turbojpeg.dll`、`liblzma.dll` …）——所以两套**绝对不能合并到
  同一个目录**，会静默互相覆盖。
- `ShotPlugin.cmake` 的 SDK 模式按 `$<CONFIG:Debug>` 自动选链接目录：单配置 Ninja
  （看 `CMAKE_BUILD_TYPE`）与多配置生成器都能得到正确结果。库名由插件工程按配置选
  （§7 的生成器表达式），混用会链接期报
  `LNK2019: cv::debug_build_guard::_InputArray ...`。
- 运行期：宿主 exe 所在的 `bin\` 里是该配置对应的那一套 DLL。Debug 插件配 Debug 宿主、
  Release 插件配 Release 宿主，否则会出现"找不到 `opencv_core4d.dll`"或行为异常。
- 多配置生成器（Visual Studio）下 `CMAKE_BUILD_TYPE` 无效、Debug/Release 输出同目录；
  **建议用 Ninja 单配置**，或给每个配置各一个构建目录（§6.2）。
- 需要把运行库带进进程的能力（典型：OpenCV）应实现 `shot.lib_provider`（`lib_provider`
  模板），并在 `metadata.json` 里写 `requires` / `provides`；**不要让普通插件自己
  `LoadLibrary` 基础库**，也不要假定 provider 一定在——依赖不满足时宿主根本不会创建消费者。

---

## 14. 许可证

| 组件 | 许可证 | 文件 |
| --- | --- | --- |
| Plugin-shot 宿主与契约（SDK 的 `include/`、`cmake/`、`docs/`、`templates/`） | Apache License 2.0 | `LICENSE` |
| nlohmann-json 3.12.0（`third_party/include/nlohmann/`） | MIT（Copyright (c) 2013-2022 Niels Lohmann） | `THIRD_PARTY_LICENSES/nlohmann-json.MIT.txt` |
| stb 2024-07-29（`third_party/include/stb_*.h`） | MIT / Public Domain 双许可（Copyright (c) 2017 Sean Barrett） | `THIRD_PARTY_LICENSES/stb.LICENSE` |
| OpenCV 4.12.0（可选叠加包 `opencv/`） | Apache License 2.0（OpenCV 4.x 起） | `NOTICE` / 上游 OpenCV 发布包 |
| OpenCV 的编解码/压缩传递依赖（jpeg / libpng / libtiff / libwebp / zlib / lzma / sharpyuv / turbojpeg） | 各自 BSD/MIT/Zlib/Public-Domain 类许可 | `NOTICE` 列了组件清单；全文见各上游发布包 |

`NOTICE` 是随 SDK 分发的第三方组件清单；再分发 SDK 或把 SDK 内容打进你的发布物时，
请保留 `LICENSE`、`NOTICE` 与 `THIRD_PARTY_LICENSES/` 三处。

---

## 15. 故障排查

| 报错 / 现象 | 原因 | 修法 |
| --- | --- | --- |
| `C1083: 无法打开包括文件: "nlohmann/json.hpp"` 或 `"nlohmann/adl_serializer.hpp"` | 没用 `shot_add_plugin()`（SDK 模式没生效），或 third_party 头缺失 | 确认工程 `include(ShotPlugin)` 且 `SHOT_SDK_ROOT` 指向含 `manifest.json` 的 SDK 根；不要自己 `find_package` |
| `C1083: 无法打开包括文件: "opencv2/core.hpp"` | 没有叠加 OpenCV 包，或 SDK 模式没生效 | 把 opencv 包的 `opencv/` 放进 SDK 根（§3），然后重新 configure |
| `LNK1181: 无法打开输入文件 "opencv_core4.lib"` | 同上（链接目录没进） | 同上；另外别自己写 `link_directories` 覆盖 SDK 模式 |
| `LNK2019: 无法解析的外部符号 ... cv::debug_build_guard::_InputArray` | Debug 插件链了 Release 库（或反之） | 库名用 §7 的 `$<CONFIG>` 生成器表达式；检查 `CMAKE_BUILD_TYPE` |
| 宿主日志说 ABI 不匹配、插件被跳过 | 插件 `metadata.abi.major` 与宿主契约不一致，或用了别处的旧契约头 | 用本 SDK 整体替换旧 SDK 后重新编译；`abi.major` 写 `2` |
| 构建报 `找不到 ShotPlugin` / `include could not find requested file: ShotPlugin` | 没设 `CMAKE_MODULE_PATH` 或 `SHOT_SDK_ROOT` 不对 | 照 §6.2 传 `-DSHOT_SDK_ROOT=<SDK 根>` |
| 构建报"这个工程还是模板：..." | 直接编了 `templates/` 里的模板，没替换占位符 | 用 `tools/new_plugin.cmake` 生成（§6.1） |
| Debug 宿主提示找不到 `opencv_core4d.dll` / `zd.dll` | Debug 插件配了 Release 宿主（或环境里只有一套 DLL） | Debug 插件只进 Debug 宿主；确认宿主 `bin\` 是 debug 变体 |
| 插件加载后崩溃/异常 | 插件自带了基础依赖（两份 OpenCV），或 CRT 不是 `/MD` | 插件目录只放自己的 DLL；`shot_add_plugin()` 已统一 `/MD`，不要覆盖 `MSVC_RUNTIME_LIBRARY` |
| `--test-plugin` 退出码 2 | 用法/IO 错误或超时 | 看 `--json-out` 报告与宿主日志；必要时加 `--verbose` / `--timeout-ms` |

---

## 16. 平台支持

| 平台 | SDK 编译插件 | 说明 |
| --- | --- | --- |
| Windows 10/11 x64 | ✅ 已验证 | 当前唯一的 SDK 导出目标（`win64`/`x64`） |
| Linux | ❌ 暂无 SDK | 宿主本身支持 Linux 构建；SDK 的 `.so`/RPATH 导出是后续计划，别拿 Windows SDK 跨平台 |
| macOS | ❌ 未支持 | 宿主无真机验证 |

本文档与 `manifest.json` 描述的是 **Windows SDK**。换平台时需要重新导出的 SDK，不要
把 Windows 的 `opencv/lib/*.lib` 与 `.dll` 拿去 Linux 用。

---

## 17. 反馈与版本

- 版本 / ABI / 契约哈希以 `<SDK>/manifest.json` 为准；插件 README 建议写上
  "本插件用 SDK x.y.z（ABI 2.0）构建，需要宿主 ≥ 0.2.0"。
- SDK 内容由宿主的 `cmake/export_sdk.cmake` 生成，**不要手改** SDK 里的
  `include/`、`cmake/`、`manifest.json`（改了就和新包对不上；需要什么能力请提给宿主）。
- 模板与文档的更新随宿主发布；升级时整体替换 SDK 目录，再重新编译插件。
