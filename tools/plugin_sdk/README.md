# tools/plugin_sdk —— 离线 SDK 的模板与一句话 demo

> 这份文件给**维护者**看：`tools/plugin_sdk/` 是离线插件 SDK 的"内容源"，会被
> `cmake/export_sdk.cmake` 原样导出到 `<SDK>/templates/`、`<SDK>/demos/` 与
> `<SDK>/tools/new_plugin.cmake`。插件作者看的是导出后的 `<SDK>/README.md` 与
> `<SDK>/docs/AUTHORING.md`，**不是**本文件。

## 1. 目录

```text
tools/plugin_sdk/
├── templates/              # 工程模板：占位符 + 骨架代码，插件作者用它起步
│   ├── generic/            #   configurable + 事件订阅 + 托盘命令（DSH 默认起点）
│   ├── image_processor/    #   shot.image_processor + configurable（反色示例）
│   ├── image_saver/        #   shot.image_saver + configurable（PNG/JPEG/BMP，用内置 stb）
│   └── lib_provider/       #   shot.lib_provider（带库插件的最小骨架）
├── demos/                  # 一句话需求 demo：完整、可直接编译、不依赖 OpenCV
│   ├── watermark/          #   "每次截图后，在图片右下角加上拍摄时间水印。"
│   └── archive/            #   "截图保存后自动按日期归档到「年-月-日」文件夹。"
├── licenses/               # 随 SDK 分发的第三方许可证（nlohmann / stb）
└── new_plugin.cmake        # 脚手架：拷模板 + 替换占位符（SDK 线维护）
```

模板与 demo 的区别：

| | 模板（templates/） | demo（demos/） |
| --- | --- | --- |
| 定位 | 骨架：作者要改名字/配置/文案 | 成品：一个真实的一句话说需求 |
| 占位符 | 有（`{{TARGET}}` 等） | 无，直接可编译 |
| 目标名 / 插件 id | 由 `new_plugin.cmake` 生成 | 固定（`watermark` / `"demo.watermark"`） |
| 随 SDK 发布 | ✅ `<SDK>/templates/` | ✅ `<SDK>/demos/` |

## 2. 占位符

模板里的占位符在**文件内容**与**文件名**中都可以出现，由 `new_plugin.cmake` 统一替换：

| 占位符 | 含义 | 例 |
| --- | --- | --- |
| `{{TARGET}}` | CMake 目标名 / DLL 名（小写、下划线） | `acme_foo` |
| `{{PLUGIN_ID}}` | 全局唯一插件 id（`作者.名字`） | `acme.foo` |
| `{{PLUGIN_NAME}}` | 界面显示名（可以是中文） | `ACME Foo` |
| `{{PLUGIN_DESCRIPTION}}` | 一行说明（写进 metadata + 语言表） | `把截图画成油画效果` |
| `{{VERSION}}` | 插件语义化版本 | `1.0.0` |
| `{{OWNER}}` | 作者 / 组织（只用于注释与文档） | `ACME` |

不认识的多余占位符（`{{SOMETHING}}`）会被替换成空串，所以模板里可以写"可选占位符"。
**不要**在模板里留下没有替换却会被 C++ 编译的裸 `{{...}}`——那会变成非法标识符。

## 3. 生成一个插件工程

```bat
:: 1) 在"开发者命令提示"里准备 MSVC 环境（新终端执行一次）
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

:: 2) 脚手架：拷贝模板 + 替换占位符（DEST 已存在且非空时加 -DFORCE=ON）
cmake -DTEMPLATE=generic ^
      -DDEST=D:\work\acme_foo ^
      -DPLUGIN_ID=acme.foo ^
      -DPLUGIN_NAME="ACME Foo" ^
      -DPLUGIN_DESCRIPTION="演示一个 Generic 插件" ^
      -DVERSION=1.0.0 ^
      -DOWNER=ACME ^
      -DTEMPLATES_DIR=D:\work\sdk\templates ^
      -P D:\work\sdk\tools\new_plugin.cmake

:: 3) configure + build（SDK 根用 -DSHOT_SDK_ROOT 指定）
cmake -S D:\work\acme_foo -B D:\work\acme_foo\build -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DSHOT_SDK_ROOT=D:\work\sdk
cmake --build D:\work\acme_foo\build
```

可用模板：`generic` / `image_processor` / `image_saver` / `lib_provider`。

产物（`shot_add_plugin()` 的默认布局）：

| 文件 | 路径 |
| --- | --- |
| 插件 DLL | `<build>/bin/plugins/<target>.dll` |
| 符号 | `<build>/pdb/plugins/<target>.pdb`（Debug 有，Release 默认不生成） |
| 导入库 | `<build>/lib/<target>.lib`（随剪枝删除，一般用不到） |

`ShotPlugin.cmake` 还会在链接后自动**校验 metadata 并贴到 DLL 尾部**，所以构建通过
= metadata 的 abi / interface id / requires 语法已经过校验。

### 3.1 Debug 构建

SDK 包同时带 `bin/debug` 与 `lib/debug`；Debug 必须**单独一个构建目录**（Ninja 是
单配置生成器）：

```bat
cmake -S D:\work\acme_foo -B D:\work\acme_foo\build-debug -G Ninja ^
      -DCMAKE_BUILD_TYPE=Debug -DSHOT_SDK_ROOT=D:\work\sdk
cmake --build D:\work\acme_foo\build-debug
```

不要用 `RelWithDebInfo` 混搭：OpenCV 叠加包按 Release/Debug 选库，混用会在链接期
报 `LNK2019 cv::debug_build_guard::...`。

### 3.2 Linux

当前发布的 SDK 包是 **win64**（`plugin-shot-sdk-<ver>-win64.zip`），Linux 版 SDK
尚未导出。模板的 CMakeLists 本身是跨平台的（不需要 vcpkg），拿到 Linux SDK 后
命令相同（GCC ≥ 13 / Clang ≥ 16，`-G Ninja`）：

```bash
cmake -S acme_foo -B acme_foo/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=<SDK 根>
cmake --build acme_foo/build
# 产物：acme_foo/build/bin/plugins/acme_foo.so
```

## 4. 手工改模板要改哪几处

不用脚手架、直接把模板目录拷走也行，但要保证下面四处**同名同值**（否则宿主会在
加载后给一条"metadata 与真实回答不一致"的警告）：

| 位置 | 要改什么 |
| --- | --- |
| `CMakeLists.txt` | `project()` 与 `shot_add_plugin()` 里的 `{{TARGET}}`；顺手检查 `if(PROJECT_NAME MATCHES "\\{\\{")` 那段可以删掉 |
| `metadata.json` | `id` / `name` / `version` / `description` / `interfaces` / `requires` / `provides` |
| `src/plugin_entry.cpp` | 静态描述 `kXxx` 里的 `id` / `name` / `version`（必须与 metadata 一致） |
| 业务源文件 | 配置项 key、语言表（`plugin.name` / `plugin.description` / `config.*`）、默认值 |

## 5. 自检（`--test-plugin`）

宿主支持"用真实加载路径检查一个插件然后退出"，不启动常驻宿主、不碰用户配置
（隔离数据目录在临时目录里）：

```bat
build\Windows\bin\Plugin-shot.exe --test-plugin D:\work\acme_foo\build\bin\plugins\acme_foo.dll ^
                                  --json-out D:\work\acme_foo\selfcheck.json
```

- 退出码：`0` = 全部检查通过；`1` = 至少一条 FAIL；`2` = 用法 / IO / 超时。
- 人类摘要最后一行是 `结果: PASS（exit 0）`；JSON 里有 `result` 与逐插件的 `checks[]`。
- 检查覆盖：metadata 可解析、ABI 可加载、能力与声明一致、配置项能读回、i18n 快照、
  列表贡献、一次 `capture.done` / `capture.failed` 事件试运行。
- **注意构建配置**：宿主是 Debug 时，插件最好也用 Debug 构建（Release 通常也能加载，
  但跨 CRT 分配有风险；自检输出的 abiComment 会把这条说明打出来）。

## 6. 安装到宿主

1. 关闭宿主；
2. 把 `<build>/bin/plugins/<target>.dll` 复制到宿主的插件目录
   （开发机是 `build\Windows\bin\plugins\`，发布包是 `bin\plugins\`）；
3. 重新启动宿主，插件会出现在设置页的插件列表里；
4. 配置项落盘在 `%LOCALAPPDATA%\plugin-shot\config.json` 的 `<plugin-id>/<key>`。

不要把插件依赖的库 DLL 放进 `plugins/`：那个目录只放插件自己的动态库，基础依赖
属于宿主运行环境（详见 SDK `docs/PLATFORMS.md`）。

## 7. 一句话需求 → 该抄哪个

| 一句话需求 | 起点 | 用到的能力 / 数据 |
| --- | --- | --- |
| "每次截图后，在图片右下角加上拍摄时间水印。" | `demos/watermark/` | `shot.image_processor` + `shot.configurable`；内置 5x7 点阵字体 |
| "截图保存后自动按日期归档到「年-月-日」文件夹。" | `demos/archive/` | `shot.configurable` + 订阅 `capture.done` + 自己的 worker 线程 |
| "给截图加个滤镜 / 调亮度 / 打马赛克" | `templates/image_processor/` | `shot.image_processor` + `shot.configurable` |
| "换个保存格式 / 目录 / 文件名规则" | `templates/image_saver/` | `shot.image_saver` + `shot.configurable` |
| "只订阅事件、加配置项、给托盘加命令" | `templates/generic/` | `shot.configurable` + `capture.done` + `tray_command_list` |
| "我这个插件只是把某个库带进进程" | `templates/lib_provider/` | `shot.lib_provider` + metadata `provides` |

更多示例（屏幕采集、UI 提供者、OpenCV 叠加包）见 SDK `docs/AUTHORING.md`。

## 8. 验证现状（2026-11-14，Windows 开发机）

用 `dist/plugin-shot-sdk-0.1.0-win64.zip` 解压出的 SDK、MSVC 19.51 + CMake 4.4.0-rc1
+ Ninja 实测：

| 工程 | Release | Debug | `--test-plugin` |
| --- | --- | --- | --- |
| `templates/generic`（经脚手架生成 `acme_generic`） | ✅ exit 0 | —— | ✅ PASS |
| `templates/image_processor`（`acme_image_processor`） | ✅ exit 0 | —— | ✅ PASS |
| `templates/image_saver`（`acme_image_saver`） | ✅ exit 0 | —— | ✅ PASS |
| `templates/lib_provider`（`acme_lib_provider`） | ✅ exit 0 | —— | ✅ PASS |
| `demos/watermark` | ✅ exit 0 | ✅ exit 0 | ✅ PASS（15/15 插件检查） |
| `demos/archive` | ✅ exit 0 | ✅ exit 0 | ✅ PASS（真实归档了自检生成的截图） |

产物都在 `<build>/bin/plugins/*.dll`，metadata 已贴尾（构建日志里的
`AppendPluginMeta: 已贴元数据`）。

## 9. 已知限制

- **模板/ demo 只覆盖"核心包"能编译的东西**：`shot.screen_capture`、`shot.ui_provider`
  需要平台 API（GDI/Win32/GLFW…），不在核心 SDK 的模板范围内。
- **Linux SDK 尚未发布**（宿主支持 Linux，但 SDK 导出目前只有 win64）；详见
  `docs/PLATFORMS.md` 与 `docs/AUTHORING.md`。
- **`--test-plugin` 是编译期/加载期自检**：不做像素级断言（例如水印画得对不对），
  这类验证要真实截一张图看，或不启动宿主的离线单测。
- demo 的字体只有 ASCII 数字/字母与 `: - . / _ 空格`；中文水印需要换点阵表或接
  FreeType（不在 demo 范围）。
- `new_plugin.cmake` 不校验 metadata 的 abi：模板写错时由构建期
  `ValidatePluginMeta.cmake` 报错（这样脚手架不重复实现一遍规则）。

## 10. 维护检查清单

- [ ] 改了模板/ demo 的 CMakeLists 路径约定后，同步改本文件 §3 与各 README 的构建命令
- [ ] 改了占位符集合后，同步改 `new_plugin.cmake` 与本文件 §2
- [ ] 新增模板/ demo 后，同步改本文件 §1、§7 与 `cmake/export_sdk.cmake` 的导出门禁
- [ ] 每次改完至少用 SDK zip 跑一次 §8 的六工程编译 + 两个 demo 自检
- [ ] 未完成项写成代码里的 `// TODO:`，不要写在本文件
