# plugin-shot

**中文** | [English](README.en.md)

> 万物皆插件的截图工具。宿主只做「宿主」该做的事——加载、校验、调度；
> 截图、图像处理、录屏……所有能力都由插件提供。

> **接手开发 / 让 AI 接手**：先读 [`AGENTS.md`](AGENTS.md)（构建、验证、本机环境、已踩过的坑），
> 再读 [`STYLE.md`](STYLE.md)（代码风格与工程约定）。

## 设计理念

核心保持极小且稳定，能力通过插件无限扩展（类似 DeepSeek Harness 的思路）。

**宿主**（当前产物名为 `Plugin-shot.exe`）负责：

- 扫描插件目录，动态加载 `.dll` / `.so` / `.dylib`
- 只解析**一个**导出符号 `shot_module_entry`，拿到自描述的模块表
- 先看 `struct_size` 再读字段、比对 `abi_major`，不兼容立刻拒绝加载
- 用 `query_interface("shot.image_processor", 1)` 问能力，不认识的能力安静跳过
- 把实例包装成 C++ 对象，按 `PluginType` 分类索引：五种具名能力之外、但 metadata 声明过的
  纯订阅/纯配置插件按 `Generic` 兜底加载；插件对象由插件自己销毁，宿主绝不 `delete`
- 采集流水线在**独立工作线程**执行（有界队列，满即丢弃并 warn），抓屏/编码不再阻塞
  事件分发；区域截图由宿主整屏抓取后按物理像素裁剪
- 支持运行期启用/禁用插件：禁用即**热卸载**（销毁实例 → 退订事件总线 → `FreeLibrary`），
  重新启用则重新加载 DLL 并回灌已存配置
- 把插件贡献的 UI 数据按 **list id** 聚成只读 JSON 快照（`shot.host.plugin_lists`），
  列表变化用 `lists.changed` 通知；新增数据形状只加 list id + JSON schema，不再改 ABI

**插件**（动态库）负责一切具体能力。当前主线是截图。

## 当前状态

| 能力 | 状态 |
| --- | --- |
| 插件加载 / 长度与版本校验 / 按类型索引（含 `PluginType::Generic` 兜底） | ✅ 可用 |
| 单入口 `shot_module_entry` + 自描述模块表（`struct_size` 约定） | ✅ 可用 |
| 一 DLL 多插件（`plugin_count` + `create(index)` + `get_info`） | ✅ 可用 |
| 能力查询 `query_interface("shot.image_processor", 1)` | ✅ 可用 |
| 屏幕采集 `shot.screen_capture`（`plugins/screen_capture`，GDI 抓虚拟桌面**物理像素**；可选把鼠标光标合成进截图（`include_cursor` 配置，默认开）；混合 DPI 双屏实测 3584x1600；仅 Windows） | ✅ 可用（`document/CAPTURE.md`） |
| 图像保存 `shot.image_saver`（`plugins/image_saver`，PNG / JPEG + 四个配置项） | ✅ 可用（`document/CAPTURE.md`） |
| 采集 → 处理 → 保存流水线（**工作线程 + 有界队列 4 + 整段持生命周期锁**；`capture.request` / `capture.done` / `capture.failed`） | ✅ 可用（`document/CAPTURE.md`） |
| 宿主主循环（事件驱动**阻塞等待**，无 16ms 轮询；`publish` / `post_to_host` / 插件启停都会唤醒） | ✅ 可用（`document/EVENT_BUS.md` §3） |
| 区域截图（`capture.request` 带合法 `region`，宿主抓整屏后按物理虚拟桌面像素裁剪、越界夹取；`plugins/region_selector` 框选覆盖层；无 region 的框选触发请求由流水线忽略、不采集；仅 Windows） | ✅ 可用（`document/CAPTURE.md` §3.1） |
| 通用插件类型 `PluginType::Generic`（只声明 `shot.configurable` + `create` 里订阅事件；有 metadata 才兜底加载） | ✅ 可用（`document/SETTINGS.md` §3.3） |
| 宿主事件总线 `shot.host.event_bus`（主题发布 / 订阅） | ✅ 可用（`document/EVENT_BUS.md`） |
| 通用列表服务 `shot.host.plugin_lists`（按 list id 返回 JSON 快照：`config_list` / `tray_command_list` / `i18n_source_list` / `i18n_list`） | ✅ 可用（`document/LISTS.md`） |
| 列表事件协议 `lists.*`（`add` / `remove` / `update` / `commit` / `updated` / `changed`，事件驱动写值） | ✅ 可用（`document/LISTS.md`） |
| 宿主插件注册表 `shot.host.plugin_registry`（启停 / 热卸载） | ✅ 可用（`document/SETTINGS.md`） |
| 运行库提供者 `shot.lib_provider`（`PluginType::Lib`） | ✅ 可用（`plugins/opencv_provider` 把 OpenCV 带进进程） |
| UI 提供者 `shot.ui_provider`（窗口/托盘/热键/覆盖层由插件自己创建） | ✅ 可用（`plugins/settings_ui`、`plugins/tray`、`plugins/hotkey`、`plugins/region_selector`） |
| 插件配置 `shot.configurable`（插件声明，宿主聚成 `config_list`、事件驱动写值 / 存盘 / 回灌） | ✅ 可用（`document/SETTINGS.md`） |
| 设置页插件 `plugins/settings_ui`（读 `config_list`，ImGui + GLFW + OpenGL3，描述符驱动表单；双屏 150%+100% DPI 已验证） | ✅ 可用（跨平台；Windows 已验证） |
| 托盘入口 `plugins/tray`（`tray_command_list` 数据驱动菜单：core 四项 + 插件贡献；图标默认宿主命名资源 `IDI_ICON1`，可用 `icon` 换 `.ico`；仅 Windows） | ✅ 可用 |
| 通知插件 `plugins/notify`（截屏成功/失败气泡，隐藏占位图标；配置 enabled / show_on_success / show_on_error / sound；仅 Windows） | ✅ 可用 |
| 剪贴板插件 `plugins/clipboard`（订阅 `capture.done` → CF_DIB / 可选 CF_HDROP；配置 enabled / also_file_drop；仅 Windows） | ✅ 可用 |
| 全局热键插件 `plugins/hotkey`（默认 Ctrl+Alt+S / Ctrl+Alt+A / Ctrl+Alt+O → `capture.request` / `settings.open`；主键支持 A-Z / 0-9 / F1-F12 / PrintScreen（可加修饰键；Win11 默认把裸按 PrintScreen 交给系统截图工具，建议用 Ctrl+PrintScreen 或关闭该 Windows 设置）；设置页用「按组合键录入」设置（`options="hotkey"`，录入期间经 `hotkey.suspend/resume` 临时撤销全局注册）；非法热键 warn 跳过；仅 Windows） | ✅ 可用 |
| 区域框选覆盖层 `plugins/region_selector`（悬停窗口吸附 + 单击截窗口 + 拖动选区；四角主题色折角与约 100ms 平滑动画；Esc / 右键取消；仅 Windows） | ✅ 可用（`document/CAPTURE.md` §3.1） |
| 主题色（`core/theme_color`，6 个预设 + 任意取色；区域框选边框/折角读它） | ✅ 可用（`document/SETTINGS.md` §5/§6） |
| 混合 DPI 多显示器采集（物理虚拟桌面 3584x1600；采集线程 DPI 上下文兜底；负 origin 未实机验证） | ✅ 可用（`document/CAPTURE.md` §2.1） |
| 设置页 DPI（双屏 150%+100%：跨屏拖动重缩放、几何按目标屏 clamp、不累积；其它 DPI 组合/三屏未验证） | ✅ 可用（`document/SETTINGS.md` §6.1） |
| 命令行 `--capture`（工作线程 + 退出前派发一轮事件）/ `--settings` / `--help` | ✅ 可用 |
| GitHub Actions 跨平台 CI（Linux/macOS/Windows 矩阵 + vcpkg 缓存 + preset 构建 + `--help` 冒烟） | workflow 已就绪；Actions 因账号 billing 锁定暂无法启动（Linux 本地 WSL 实测通过，见「平台」） |
| 宿主自身设置（`core` 伪插件：界面语言 / 日志级别 / 控制台 / 文件日志 / 主题色） | ✅ 可用 |
| 插件元数据贴尾（`metadata.json` → DLL 尾部，纯 CMake） | ✅ 可用（宿主加载前读取：预筛 + 依赖拓扑排序） |
| 元数据构建期校验（id 唯一 / interfaces 已注册 / abi 与头一致 / 依赖语法） | ✅ 可用（`cmake/ValidatePluginMeta.cmake`） |
| 插件级依赖声明（`requires` / `provides`，带版本区间，`core` 为宿主） | ✅ 可用（`document/METADATA.md`） |
| 依赖驱动加载顺序（拓扑排序，不满足只跳过该插件） | ✅ 可用 |
| 插件工程公共构建规则（`shot_add_plugin()`） | ✅ 可用（宿主与离线 SDK 共用） |
| 多语言 i18n（`core/language` = `auto` 或语言目录 tag；`i18n_source_list` 插件贡献 + 宿主合并生成只读 `i18n_list`；外部语言包 `files/lang/` 与 `%LOCALAPPDATA%\screen-shot\langs\`；设置页/托盘按 `display(owner,key) ?? key` 解析，旧插件字面量零破坏） | ✅ 可用（`document/I18N.md`；内置 27 个标签，仓库内置译文仅 zh-CN / en-US；外部语言包协议已就绪、翻译插件 `plugins/translator` 已加入（默认关）；字体动态图集 + Segoe UI/Arial 补字，RTL 与韩/泰/阿拉伯字形未做） |
| 离线插件 SDK 导出（`export_sdk` 目标） | ✅ 可用 |

> 产物名取自 `CMakeLists.txt` 的 `PROJECT_NAME`（当前 `Plugin-shot`）：宿主叫 `Plugin-shot.exe`，
> 插件落 `bin/plugins/`。应用图标缺失时 `.rc` 会自动降级成不含 ICON 行，所以改名不会把构建搞挂。

## 目录结构

```
.
├── CMakeLists.txt          # 跨平台构建脚本（vcpkg 清单模式 + GLOB 自动发现插件）
├── CMakePresets.json       # debug / release 两个 preset（CI 与跨平台构建用同一套）
├── vcpkg.json              # 依赖清单
├── vcpkg-configuration.json# vcpkg builtin-baseline
├── .github/
│   └── workflows/cmake.yml # 跨平台 CI（Linux/macOS/Windows；当前受账号 billing 限制）
├── files/
│   └── lang/                 # 随安装包分发的官方语言包（<tag>.json；构建同步到 bin/files/）
├── include/
│   ├── plugin_api.h          # ★ 宿主 ⇄ 插件的 ABI 契约汇总头（只 include；跨边界入口仍是它）
│   ├── plugin_api/           # ★ 契约正文：按主题一个子头（basics / interfaces / 各能力 /
│   │                         # host_services / module / abi_layout）
│   ├── plugin_api_export.h   # 导出宏：插件默认 dllexport，宿主定义 SCREENSHOT_HOST_EXPORTS
│   ├── shot_plugin_helpers.h # 插件侧工具汇总头（只 include，不声明类）
│   ├── shot_plugin_helpers/  # 每类一个头文件：iface_entry / logger / config /
│   │                         # event_bus / plugin_lists / plugin_registry
│   ├── dllHandler.h           # LoadLibrary / dlopen 封装（含不抛异常的 tryGetFunction）
│   ├── pluginManager.h       # 插件扫描、依赖排序、加载与热卸载（含 Generic 兜底）
│   ├── hostServices.h        # 宿主服务表：日志、配置、事件总线、列表服务、插件注册表
│   ├── hostListCatalog.h     # 通用列表目录：config_list 生成 + lists.* 事件协议
│   ├── hostRuntime.h         # 宿主编排：PluginManager + 列表目录 + 注册表 + 采集流水线 + 生命周期锁
│   ├── capturePipeline.h     # 采集流水线：工作线程 + 有界队列 + 区域裁剪
│   ├── config.h              # core::Config：JSON 配置文件读写
│   └── PluginInstances/      # 宿主侧插件包装（base / ImageProcessor / LibProvider / ScreenCapture / ImageSaver；Generic 用 base）
├── src/                      # 宿主实现与入口
├── plugins/                  # 每个子目录都是一个独立插件工程
│   ├── example_processor/    # 完整示例：一个 DLL 两个插件，都带配置项（依赖 OpenCV）
│   ├── opencv_provider/      # 合法但零能力的插件包（把 OpenCV 带进进程）
│   ├── screen_capture/       # 屏幕采集：GDI 抓虚拟桌面物理像素（仅 Windows）
│   ├── image_saver/          # 图像保存：PNG/JPEG，目录/格式/质量/文件名可配置
│   ├── clipboard/            # 剪贴板：capture.done → CF_DIB / CF_HDROP（仅 Windows）
│   ├── notify/               # 截屏结果通知：隐藏占位图标 + Shell 气泡（仅 Windows）
│   ├── hotkey/               # 全局热键 → 事件（仅 Windows）
│   ├── region_selector/      # 区域框选覆盖层：悬停窗口吸附、单击截窗、四角主题色折角（仅 Windows）
│   ├── settings_ui/          # 设置页：ImGui + GLFW + OpenGL3（跨平台，Windows 已验证）
│   └── tray/                 # 托盘入口：数据驱动菜单 + 图标（仅 Windows）
├── document/
│   ├── README_SDK.md         # SDK 使用与完整编译约束
│   ├── ABI_POLICY.md         # 只追加规则、interface id 登记表
│   ├── SETTINGS.md           # 设置页：config_list、事件驱动写值/提交、Generic、DPI、插件启停与热卸载
│   ├── LISTS.md              # 通用列表与事件驱动：list id 登记、lists.* 协议与刷新时机
│   ├── CAPTURE.md            # 采集流水线：工作线程、区域截图、capture.* 事件与 --capture
│   ├── EVENT_BUS.md          # 宿主事件总线：主题约定、线程模型、生命周期
│   ├── I18N.md               # 多语言协议：语言目录、外部语言包、两个 i18n 列表、回退链、字体与限制
│   └── METADATA.md           # metadata.json 的字段定义、权威规则与不写什么
├── cmake/
│   ├── ShotPlugin.cmake        # shot_add_plugin()：插件工程公共构建规则
│   ├── AppendPluginMeta.cmake  # 把 metadata.json 贴到 DLL 尾部
│   ├── prune_output_dir.cmake  # 产物目录剪枝（不留 pdb/ilk/exp/lib/基础依赖 DLL）
│   └── ...                     # 运行时依赖布局、SDK 导出、链接前杀进程
├── tools/
│   └── verify/                 # 真机验证：带构建锁的 build 包装 + capture/settings/热键/剪贴板/多语言 e2e
└── sdk/                    # export_sdk 生成的离线 SDK（不入库，见 .gitignore）
```

## 构建

前置：CMake ≥ 3.20（直接用 `CMakePresets.json` 时需 ≥ 3.21）、支持 C++23 的编译器（Windows 上为 MSVC）、[vcpkg](https://github.com/microsoft/vcpkg)。

```bash
# 工具链优先读 $VCPKG_ROOT / $VCPKG_INSTALLATION_ROOT，都没有则回落到 /opt/vcpkg
export VCPKG_ROOT=/path/to/vcpkg      # Windows: set VCPKG_ROOT=D:\vcpkg

cmake -S . -B build/Windows -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows

# 导出离线 SDK（供第三方插件在不装 OpenCV 的机器上编译）
cmake --build build/Windows --target export_sdk

# 也可以用 CMake preset（CI 与跨平台构建走同一套；产物在 build/<platform>/）
cmake --preset release -DVCPKG_TARGET_TRIPLET=x64-linux   # triplet 按平台填
cmake --build --preset release
```

Windows 本机还把 vcvars + cmake 包成了便利脚本：`bash tools/verify/build.sh`（带构建锁，
多终端 / 多 agent 共用构建目录时串行；`--target <t>` 只构建一个 target）。真机 e2e 冒烟
脚本与运行纪律见 [`tools/verify/README.md`](tools/verify/README.md)。

可选开关 `-DSHOT_ENABLE_OPENCV=OFF` 跳过依赖 OpenCV 的插件（`opencv_provider` /
`example_processor`），用于 CI 提速或没有 OpenCV 的环境；默认 `ON`。

首次 configure 会以清单模式自动安装依赖（`opencv4` + jpeg/png/tiff/webp 特性、`nlohmann-json`、
`spdlog`、`glfw3`、`nativefiledialog-extended`、`stb`，以及设置页插件用的 `imgui`——
静态库，`glfw-binding` + `opengl3-binding`），耗时较长。设置页的运行期依赖 `glfw3.dll`、
`nfd.dll` 与 OpenCV 一样由 `cmake/copy_runtime_deps.cmake` 从 vcpkg 运行时目录布置到 `bin/`，
插件目录不含这些 DLL。通用列表协议不引入新依赖：列表快照就是 `nlohmann-json` 序列化的 JSON。
新增插件也没有引入新的 vcpkg 依赖：`clipboard` 复用 `stb` / `nlohmann-json`，`notify` 只用
`user32` / `shell32`，`hotkey` 只用 `user32`，`region_selector` 复用 `imgui` / `glfw3` /
`nlohmann-json`。

**CI**：`.github/workflows/cmake.yml` 已就绪（Linux / macOS / Windows 矩阵 + vcpkg 清单缓存 +
CMake preset 构建 + `--help` 无头冒烟），但当前 GitHub 账号的 Actions 受 **billing 锁定**
无法启动（0 秒失败）——workflow 待账号解封后验证。

**跨平台可编译性现状**：Linux 已在 WSL2 实测（Ubuntu 24.04.5 LTS / GCC 13.3.0 / x86_64 /
vcpkg `x64-linux` / CMake 3.28.3 / Ninja 1.11.1 / Release）：configure + build 通过
（43s、**0 warning**），产出 `build/Linux/bin/Plugin-shot` 与
`plugins/{settings_ui,image_saver,example_processor,opencv_provider}.so`，元数据校验与产物
剪枝通过；`--help` 退出码 0，`--capture` 无头运行能 dlopen 全部 `.so`、初始化 OpenCV
provider、在 WSLg 下建出 OpenGL 上下文并按设计失败（非 Windows 没有 `screen_capture`）
后干净退出。`SHOT_ENABLE_OPENCV=OFF` 也实测通过。macOS **未在真机验证**（只做过代码审计）。
Windows 是主要开发与验证平台。

### 运行

```bash
cd build/Windows/bin && ./Plugin-shot.exe            # 不带参数：托盘常驻
cd build/Windows/bin && ./Plugin-shot.exe --capture  # 截一张、打印路径后退出
cd build/Windows/bin && ./Plugin-shot.exe --settings # 打开设置页后继续常驻（调试入口）
cd build/Windows/bin && ./Plugin-shot.exe --help     # 打印用法
```

插件目录按相对路径 `./plugins` 解析，因此**必须在 `bin/` 下启动**。

不带参数启动后宿主不会主动打开设置页（托盘正常时）：托盘菜单「截屏 / 区域截屏 / 打开设置 /
退出」分别发布 `capture.request`（空负载）/ `capture.request`（负载
`{"mode":"region"}`）/ `settings.open` / `app.quit`；「区域截屏」由
`plugins/region_selector` 弹出框选覆盖层，用户确认后它再发一条带 `region` 的请求，宿主按
物理虚拟桌面像素裁剪（见 `document/CAPTURE.md` §3.1）。框选触发请求本身**不会采集**
（流水线直接忽略，不落盘，不会再额外存一张全屏图）。若 `tray.main` 未加载（被禁用或
加载失败），启动会自动发布 `settings.open` 兜底。

截图默认保存到 `%USERPROFILE%\Pictures`，文件名模板 `shot_%Y%m%d_%H%M%S`、默认 PNG；
目录/格式/质量/文件名都是 `plugins/image_saver` 的配置项，可在设置页修改。截屏结果由
`plugins/notify` 弹系统通知、`plugins/clipboard` 把图片写进系统剪贴板（两者都可在设置页
开关）；`plugins/hotkey` 默认用 `Ctrl+Alt+S` / `Ctrl+Alt+A` / `Ctrl+Alt+O` 触发同样的
截屏 / 区域截屏 / 打开设置，全部热键可在设置页改（热键项用「录入…」按一次组合键录入，
不用手输字符串）。

`--capture` 成功时日志打印 `Capture saved: <path>`、退出码 `0`；失败（采集/保存/插件缺失/
请求队列满）退出码 `1`，未知参数 `2`。一次性模式在退出前会**派发一轮事件**，所以剪贴板/
通知这类订阅者同样能收到本次 `capture.done`（详见 `document/CAPTURE.md` §6）。

日志用 spdlog：控制台 +
`%LOCALAPPDATA%\screen-shot\logs\host.log`（追加写）；配置写在
`%LOCALAPPDATA%\screen-shot\config.json`（宿主设置与插件配置项都在这里，键是 `<owner>/<id>`，
与 `config_list` 条目的 `owner` / `id` 一一对应）。

### 多显示器与 DPI

| 项 | 现状 |
| --- | --- |
| 宿主 DPI 感知 | 启动最早阶段显式声明 **per-monitor v2**（`SetProcessDpiAwarenessContext`，逐级回退 `SetProcessDpiAwareness` / `SetProcessDPIAware`，全失败只 warn）；日志行 `DPI awareness: <模式>` |
| 多显示器采集 | 虚拟桌面**物理像素**，本机混合 DPI 双屏（150% 2560x1600 + 100% 1024x768）实测 `3584x1600`；负 origin（副屏在左/上）**未实机验证** |
| 采集线程 DPI | `plugins/screen_capture` 每次 `describe`/`capture` 临时提升调用线程到 PMv2、返回前恢复，不依赖宿主进程声明 |
| 设置页 DPI | 双屏 150%+100% 已验证：跨屏拖动自动重缩放（不累积）、几何恢复按**目标显示器** DPI clamp（含标题栏/边框）；其它 DPI 组合（125% / 200%）、三屏及以上、负 origin **未验证** |
| 区域坐标 | 物理虚拟桌面像素，原点 = 虚拟桌面左上角；越界夹取（详见 `document/CAPTURE.md` §3.1） |

### 产物布局（硬约束）

| 路径 | 内容 |
| --- | --- |
| `bin/` | 宿主 exe 与本配置的运行时依赖 DLL（**禁止出现 `*.pdb` / `*.ilk`**） |
| `bin/plugins/` | 只放插件自身产物（**禁止出现 OpenCV 等基础依赖**） |
| `build/<platform>/pdb/` | 全部调试符号 |
| `sdk/` | 离线 SDK：`include/`、`lib/`(release)、`lib/debug/`、`bin/`、`bin/debug/` |

这些约束由 `cmake/copy_runtime_deps.cmake` 与 `cmake/prune_output_dir.cmake` 在构建期强制。
插件产物的扩展名随平台：Windows `.dll`、macOS `.dylib`、Linux `.so`；非 Windows 不复制运行库，
由 RPATH 指向 vcpkg 的 `lib/`。
理由见下文契约第 3 条：插件一旦自带 OpenCV，同一进程就会出现两份实例。

## 插件契约

跨边界的入口头只有一个：`include/plugin_api.h`（纯 C，不依赖 STL）。它只是汇总头，
正文按主题拆在 `include/plugin_api/*.h`（版本与读取规则、interface id、每个能力一张表、
宿主服务、`ShotModule`、布局锚点）——插件源码照旧只写 `#include "plugin_api.h"`。
整个插件包只导出**一个** C 函数：

```c
SCREENSHOT_API const ShotModule* shot_module_entry(void);
```

它返回一张自描述的模块表 `ShotModule`，其余一切都在表里：

| 字段 | 作用 |
| --- | --- |
| `struct_size` / `abi_major` / `abi_minor` | 自描述长度 + 版本：宿主先看长度再读字段，然后比对 major |
| `plugin_count` | 本 DLL 里有几个插件 |
| `get_info(index, info)` | 不创建实例就能拿到 id / 名称 / 版本 |
| `create(index, host)` | 在插件自己的堆上分配实例，返回不透明指针 |
| `destroy(instance)` | 销毁实例——**只能由插件自己调用**，宿主绝不 `delete` |
| `query_interface(instance, id, version)` | 问"你会不会这个能力" |
| `init(host, flags)` / `shutdown()` | 可选：模块级初始化与收尾，`nullptr` 表示不需要 |

一个 DLL 就是**一个插件包**：`plugin_count` + `create(index)` 让宿主不必提前知道包里有什么，
`metadata.json` 的 `plugins[]` 顺序与索引一一对应。

### 四条让 ABI 不必再改的规则

1. **只加字段，不加符号**：整个包只有 `shot_module_entry` 一个导出。加能力靠往结构体末尾**追加字段**，不是加导出函数。
2. **读字段前先问长度**：每个跨边界结构体第一个字段都是 `struct_size`，读取方一律先过 `SHOT_HAS_FIELD(ptr, type, field)`。于是老插件（短结构）与新宿主（想读新字段）能安全共存——**加字段永远不需要改版本号**。
3. **能力用字符串 id 问**：`query_interface(instance, "shot.image_processor", 1)` 返回函数表，没有就返回 `NULL`，宿主安静跳过。新增一类能力 = 新增一个 id + 一个结构体，不动任何已有结构体。
4. **数据形状不进 ABI**：新增一类 UI/宿主数据（菜单项、配置项、目标列表……）= 新 list id + JSON schema + `lists.*` 事件；ABI 只描述能力与行为，不描述数据结构，见 `document/LISTS.md`。

`include/plugin_api/abi_layout.h` 用 `static_assert` 钉住了每个结构体的字段偏移与总大小
（汇总头里最后 include 它）：重排或改类型会直接编译失败，而"追加字段"必须手动更新那一行——
把破坏 ABI 变成一次有意识的行为。

### 已注册的能力

| interface id | 结构体 | 作用 |
| --- | --- | --- |
| `shot.image_processor` | `ShotImageProcessorV1` | 处理一张图：`process(self, ShotImage*)`，返回 0 表示成功 |
| `shot.screen_capture` | `ShotScreenCaptureV1` | 屏幕采集：`describe` 报告虚拟桌面几何（**物理像素**，多显示器合成），`capture` 把 BGRA8 像素写进宿主分配的 `ShotImage`；区域截图由宿主抓整屏后按 `region` 裁剪，见 `document/CAPTURE.md` |
| `shot.image_saver` | `ShotImageSaverV1` | 图像保存：把 `ShotImage` 落盘并写回最终路径；格式/质量/目录来自插件配置项，见 `document/CAPTURE.md` |
| `shot.configurable` | `ShotConfigurableV1` | 声明并读写配置项，见下 |
| `shot.lib_provider` | `ShotLibProviderV1` | 声明"我是运行库提供者"，报告库名与版本；宿主按 `PluginType::Lib` 登记 |
| `shot.ui_provider` | `ShotUIProviderV1` | 窗口/托盘由插件自己创建，宿主只调 `init` / `deinit` / `post_to_plugin` |
| `shot.host.event_bus` | `ShotHostEventBusV1` | **宿主服务**：进程内主题发布/订阅 |
| `shot.host.config` | `ShotHostConfigV1` | **宿主服务**：宿主配置读写（旧接口；新代码用 `plugin_lists` 的 `config_list`） |
| `shot.host.board_msg` | `ShotHostBoardEventV1` | **宿主服务**：旧版广播队列（新代码用 event_bus） |
| `shot.host.plugin_lists` | `ShotHostPluginListsV1` | **宿主服务**：通用列表，按 list id 返回只读 JSON 快照（`config_list` / `tray_command_list`），见 `document/LISTS.md` |
| `shot.host.plugin_registry` | `ShotHostPluginRegistryV1` | **宿主服务**：已安装插件状态与异步启停 |

当前契约版本 `SHOT_ABI_MAJOR = 2` / `SHOT_ABI_MINOR = 0`：2.0 删除了旧的配置目录服务，
改为通用列表服务 `shot.host.plugin_lists`（列表数据是 JSON 快照，注册/写值/提交全部走
`lists.*` 事件）。删除已发布的服务属于破坏性变更，所以 major 涨到 2；
**从 2.0 起，新增一种 UI/宿主数据不再改 ABI**：新 list id + JSON schema 即可。

`shot.screen_capture` / `shot.image_processor` / `shot.image_saver` 的能力回调在采集流水线的
**工作线程**上执行（一次一张、串行），`shot.configurable` 的 `set_config` 仍在 main 线程。
插件如果让能力回调与配置共享状态，必须自己做同步（线程模型见 `document/CAPTURE.md` §7）。

### 纯订阅者 / 纯配置项插件（Generic）

插件也可以不实现任何宿主可调用的具名能力：只声明 `shot.configurable`，在
`create(index, host)` 里订阅事件、在 `destroy` 里退订。只要 metadata 声明过它，宿主就按
`PluginType::Generic` 用基类包装加载：注册表照常列 LOADED、可启停、`config_list` 照常
枚举它的配置项；没有 metadata 又认不出任何具名能力的包会被跳过。完整写法和约束见
[`document/README_SDK.md`](document/README_SDK.md)，现成例子是
[`plugins/clipboard/README.md`](plugins/clipboard/README.md) 与
[`plugins/notify/README.md`](plugins/notify/README.md)。

`ShotImage` 描述一块 8 位像素缓冲：

- `pixel_format` 显式声明布局（当前只有 `SHOT_PIXEL_FORMAT_BGRA8`），不再靠 `channels` 猜
- `stride` 以字节计，**可以为负**（自底向上存储），也可大于 `width * channels`（行尾填充）
- 所以必须按行寻址，不能假设 `data` 是连续块

### 配置：插件声明，宿主渲染与存盘

`shot.configurable` 让插件自己说清"我有哪些配置项"，宿主据此生成界面、保存文件、灌值——
**宿主不硬编码任何配置项名字**：

```c
typedef struct ShotConfigurableV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t (*get_config_count)(void* self);
    int32_t  (*get_config_desc)(void* self, uint32_t index, ShotConfigDesc* out);
    int32_t  (*get_config)(void* self, const char* key, char* buf, uint32_t buf_size);
    int32_t  (*set_config)(void* self, const char* key, const char* value);
} ShotConfigurableV1;
```

- 配置项由 `ShotConfigDesc` 描述：`kind`（BOOL / INT / FLOAT / STRING / ENUM / COLOR / PATH）、
  `key`、显示名、默认值、取值范围、枚举候选。
- **值一律是字符串**：跨边界没有宽度与对齐问题，天然可以存成 JSON，插件也不必为读配置引入 STL。
- 插件遇到不认识的 `key` 必须安全忽略（返回负值）——宿主升级后多传一项，老插件也不会出问题。
- 约定：`set_config` 可能在运行中被调用（用户拖滑块），实现要么线程安全，要么声明"下次 `process` 生效"。

**宿主的设置也走同一套描述符**：宿主内部以 `core` 伪插件实现 `shot.configurable`（日志级别、
控制台/文件日志开关），宿主把所有可配置实例聚成 `config_list` JSON 快照，设置页只写一套
渲染逻辑。

- 存盘键是 `<owner>/<id>`，值由**宿主**落盘到 `%LOCALAPPDATA%\screen-shot\config.json`；
  插件只负责 get/set，不需要自己写配置文件。
- 启动时宿主把已存值灌回插件（没有已存值就用 `default_value`），插件不需要"读配置"这一步。
- 写值走 `lists.update`（事件驱动、逐项回结果），落盘走 `lists.commit`；成功后宿主发布既有
  `config.applied`（空负载）：缓存了派生状态的插件重读当前值即可，**没有单独的 reload 调用**。
- 输出/保存相关的设置不归宿主：谁实现保存能力，谁在自己的 `kConfig[]` 里声明这些项。

**列表：新数据形状不进 ABI**：`config_list` 与托盘菜单的 `tray_command_list` 来自同一个
通用列表服务。插件要给 UI 提供一批新数据（菜单项、目标列表……）时，定义新 **list id + JSON
schema**，运行期用 `lists.add` 注册、宿主用 `lists.changed` 通知；**不要为数据形状新增 ABI
结构体或宿主服务**。完整协议、list id 登记表与性能约定见 [`document/LISTS.md`](document/LISTS.md)。

完整链路（`config_list`、设置页插件、插件启停与热卸载）见 [`document/SETTINGS.md`](document/SETTINGS.md)。

### 设置页与插件启停

设置窗口本身也是插件（`plugins/settings_ui`，实现 `shot.ui_provider`），宿主没有主窗口：

- 设置页通过 `shot.host.plugin_lists` 读 `config_list` JSON 快照，按条目 schema 渲染 7 种控件
  （BOOL / INT / FLOAT / STRING / ENUM / COLOR / PATH）；写值走 `lists.update`、落盘走
  `lists.commit`（带 `tag` 的事件往返，逐项结果回 `lists.updated` / `config.applied`）；
  窗口的 DPI / 多屏行为见「多显示器与 DPI」与 `document/SETTINGS.md` §6.1；
- 通过 `shot.host.plugin_registry` 管理插件启停。**禁用是热卸载**：销毁实例、UI 插件先
  `host.shutdown` + `deinit`、退订事件总线，同包最后一个实例卸载后 `FreeLibrary`；
  重新启用则重新加载 DLL 并回灌已存配置。被其他已加载插件 `requires` 的插件不允许单独禁用；
- **启用会级联启用依赖**：启用一个插件时，宿主先递归启用它 `requires` 的提供者；提供者
  启用失败则整条启用失败，原因里写明是哪个依赖；
- 托盘 `plugins/tray` 可以禁用/重新启用：右键菜单内容来自 `tray_command_list`（宿主预置
  **core 四项**：截屏 / 区域截屏 / 打开设置 / 退出；插件用 `lists.add` 贡献），点击菜单项
  只发布条目声明的事件；图标默认取宿主 exe 的**命名资源 `IDI_ICON1`**，可用 `icon` 配置换成
  任意 `.ico`。托盘**不再弹任何气泡**：成功/失败提示全部由 `plugins/notify` 负责；
  `tray.main` 未加载（被禁用或加载失败）时，常驻启动会自动发布 `settings.open`，以设置页作为
  入口兜底；
- 其它插件也走同一套生命周期与配置链路：`plugins/notify`（截屏结果通知）、
  `plugins/clipboard`（写系统剪贴板）、`plugins/hotkey`（全局热键 → 事件）、
  `plugins/region_selector`（框选后发布带 `region` 的 `capture.request`）——它们都只通过
  事件/配置与宿主协作，不直接调用其它插件，设置页会按 `config_list` 自动出现它们的配置节；
- 启停请求是异步的：设置页只登记，宿主 main 线程执行完才发布 `plugins.changed`；一次启用
  可能级联多个依赖，所以设置页可能连续收到多条 `plugins.changed`。

### 必须遵守的 ABI 约定

1. **运行库必须是 /MD（`MultiThreadedDLL`）**，宿主与插件一致；`/MT` 会得到两份 CRT。
2. **Debug/Release 必须与宿主一致**——Debug 插件只能加载进 Debug 宿主。
3. **插件不得自带基础依赖**（`opencv_*.dll` 等）：宿主 exe 旁边的 `bin/` 里已经有一套，插件目录里再放一份会让同一进程出现两个 OpenCV，类型信息、全局状态与内存分配器全部割裂。

   > 宿主本身**不链接** OpenCV（`CMakeLists.txt` 只链接 `nlohmann_json` 与 `spdlog`）。`bin/` 里的
   > `opencv_*.dll` 来自 `cmake/copy_runtime_deps.cmake` 对 vcpkg 运行时目录的整目录布置，
   > 不是链接拉进来的。需要 OpenCV 的插件在**自己的** `CMakeLists.txt` 里
   > `find_package(OpenCV QUIET)` 并链接（见 `plugins/opencv_provider`），
   > 运行时从 `bin/` 里那一份解析到同一个 DLL。
4. **跨边界不要传 STL 容器 / `std::string` / `cv::Mat`**，也不要传带虚函数的 C++ 对象：跨边界只有 POD 结构体 + 函数指针。
5. **只用定宽类型**（`uint32_t` / `int32_t` / `double`），不用 `int` / `long` / `size_t`；枚举不进结构体，改用定宽常量。
6. **异常绝不跨越 DLL 边界**：每个 `extern "C"` 函数内部自行兜底，错误用返回值表达。
7. **`abi_major` 必须与宿主一致**；只追加能力时提高 `abi_minor` 即可，宿主接受 minor ≤ 自己的插件。
8. **结构体只能末尾追加**：永不重排、永不改类型、永不删，作废字段改名 `retired_xxx` 占位。

### 元数据贴尾

`metadata.json` 由 `cmake/AppendPluginMeta.cmake` 在**链接之后**追加到 DLL 尾部：

```text
<原始二进制>
---PLUGIN_META_START---
{ "schema": 1, "abi": { ... }, "plugins": [ ... ] }
---PLUGIN_META_END---
```

- 纯文本标记，不需要 Python、不需要贴尾小工具，宿主 `rfind` 就能取出来。
- 文件尾的额外数据不在 PE/ELF 的节表里，加载器直接忽略，不影响装载。
- 元数据跟着文件走：用户复制或重命名插件也不会出现「清单与 DLL 分家」。
- 标记字面量只在 `plugin_api/plugin_info.h` 里定义一次；构建脚本构建期把汇总头 + 全部子头
  拼成整份文本再取宏值（`cmake/ReadContractHeader.cmake`）。
- 贴尾之前会先校验：`plugins[]` 非空、`id` 唯一、`interfaces` 里的 id 在契约头里有定义、
  `abi` 与 `SHOT_ABI_*` 一致、`package` 与目标名一致。写错就构建失败，不用等宿主发现。
- 字段定义、权威规则（谁是权威、为什么不写配置项、依赖怎么解析）见 [`document/METADATA.md`](document/METADATA.md)。
- 依赖是**插件级**的：`"requires": ["core>=0.1.0", "opencv>=4.12"]`，`core` 指宿主自己的版本
  （来自 CMake 生成的 `version.h`）。宿主先读所有 DLL 的元数据建依赖图，拓扑排序后加载，
  不满足的插件**只跳过它自己**，同包其它插件和整个宿主继续工作。

### 写一个插件

一个插件一个 `.cpp`：状态、`process`、可选的配置读写，最后组装能力函数表：

```cpp
// plugins/acme_tools/src/blur.cpp
static int32_t blur_process(void* self, ShotImage* image) {
    auto* state = static_cast<BlurState*>(state_of(self));   // 注意：self 是不透明指针
    // … 按行处理 image …
    return 0;                                                 // 0 = 成功
}

const ShotImageProcessorV1 kImageOps =
    SHOT_OPS(ShotImageProcessorV1, .process = &blur_process);

const ShotIfaceEntry kIfaces[] = {                            // 能力目录
    { SHOT_IFACE_IMAGE_PROCESSOR, 1, &kImageOps },
    { nullptr, 0, nullptr },                                  // 结束标记
};
```

`plugin_entry.cpp` 只做三件事：把各插件登记成 `kEntries[]`、转发 `query_interface`、返回模块表：

```cpp
extern "C" {

SCREENSHOT_API const ShotModule* shot_module_entry(void) {
    static const ShotModule module = {
        .struct_size     = sizeof(ShotModule),
        .abi_major       = SHOT_ABI_MAJOR,
        .abi_minor       = SHOT_ABI_MINOR,
        .plugin_count    = kEntryCount,
        .get_info        = &get_info,
        .create          = &create_instance,
        .destroy         = &destroy_instance,
        .query_interface = &query_instance,   // 转发 shot_query_interface(ifaces, id, ver)
    };
    return &module;
}

}
```

完整可编译样板见 [`plugins/example_processor/`](plugins/example_processor/README.md)：一个 DLL 装两个插件、
都带配置项，目录结构 `CMakeLists.txt` + `metadata.json` + `include/` + `src/`。
SDK 说明见 [`document/README_SDK.md`](document/README_SDK.md)，
只追加规则与 interface id 登记表见 [`document/ABI_POLICY.md`](document/ABI_POLICY.md)。

插件工程的 CMakeLists 只有三件事——找模块、列源文件、指元数据：

```cmake
list(APPEND CMAKE_MODULE_PATH "${HOST}/cmake")   # 或 <SDK>/cmake
include(ShotPlugin)

shot_add_plugin(acme_tools
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp src/blur.cpp src/sharpen.cpp
    INCLUDE_DIRS include)
```

`shot_add_plugin()` 统一处理 `/MD`、`PREFIX ""`、隐藏符号、产物目录、pdb 重定向、
贴元数据与产物剪枝——这些规则以前散落在根 `CMakeLists.txt` 里，容易和文档漂移。

## 平台

Windows 是主要开发与验证平台（MSVC + vcpkg `x64-windows`）。平台相关插件在非 Windows 上
由各自 `CMakeLists.txt` **主动跳过**（不产出伪实现）：`screen_capture`（GDI 抓屏）、
`tray`（Win32 托盘）、`clipboard`（Win32 剪贴板）、`hotkey`（RegisterHotKey）、
`region_selector`（Win32 物理坐标 + 覆盖层）、`notify`（Shell 通知气泡）。
跨平台可编译的是 `settings_ui`（GLFW）、`image_saver`、`example_processor`、
`opencv_provider`。

| 平台 | 状态 |
| --- | --- |
| Windows（MSVC x64） | ✅ 主要开发与验证平台（含双屏混合 DPI、插件启停、采集/保存/通知/剪贴板/热键/区域框选） |
| Linux（WSL2 Ubuntu 24.04.5 LTS / GCC 13.3.0 / x86_64 / vcpkg `x64-linux`） | ✅ 已实测：configure + build（43s、0 warning），产出 `Plugin-shot` 与 `settings_ui` / `image_saver` / `example_processor` / `opencv_provider` 四个 `.so`；`--help` 退出 0，`--capture` 无头运行可加载全部 `.so` 并干净退出（非 Windows 没有 `screen_capture`，按设计失败返回 1）；`SHOT_ENABLE_OPENCV=OFF` 也通过 |
| macOS | **未在真机验证**（只做过代码审计：`base.cpp` 的 `_NSGetExecutablePath` / 空串 UB / HOME 兜底已修）；没有真实构建结论 |
| GitHub Actions | workflow 已就绪；当前账号 billing 锁定导致 Actions 无法启动，待账号解封后验证 |

Linux 构建的宿主侧系统依赖（Ubuntu 24.04 实测）：`build-essential ninja-build pkg-config`、
`autoconf autoconf-archive automake libtool libltdl-dev`、`bison flex gperf`、
`gettext autopoint python3-venv`、`nasm yasm`、`curl zip unzip`、
`libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev`、X11/wayland 开发库
（`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev
libxkbcommon-dev libwayland-dev`）、`libselinux1-dev libpam0g-dev`。`nativefiledialog-extended`
在 Linux 走 vcpkg 的 `NFD_PORTAL=ON`，**不需要**系统 GTK3/dbus 开发包。

## 许可

尚未指定。
