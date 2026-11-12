# AGENTS.md —— 给接手这个仓库的 AI / 人

> 这份文件是**操作手册**：怎么构建、怎么验证、哪些规则不能破、哪些坑已经踩过。
> 风格与编码约定在 [`STYLE.md`](STYLE.md)，**两者的优先级一样高**：
> `STYLE.md` 管"代码长什么样"，`AGENTS.md` 管"怎么干活、怎么验收、别踩哪些坑"。
>
> 上一任负责人（AI）的上下文快满了，这份是交接。当前基线：`main` 分支、
> 契约 **ABI 2.0**、13 个插件工程、9 份设计文档。

---

## 0. 30 秒上手

| 事项 | 值 |
| --- | --- |
| 工程根 | `D:\src\repo\plugin-shot\Host`（Windows-first，C++23） |
| 构建目录 | `build\Windows`（Ninja + MSVC Debug），Linux 用 `build\Linux`（WSL 已验证） |
| 宿主产物 | `build\Windows\bin\Plugin-shot.exe`，插件在 `build\Windows\bin\plugins\*.dll` |
| 配置文件 | `%LOCALAPPDATA%\plugin-shot\config.json` |
| 日志 | `%LOCALAPPDATA%\plugin-shot\logs\host.log`（UTF-8，中文会显示成乱码是编码问题，不是 bug） |
| CLI | `--capture` 截一张存盘后退出 / `--settings` 打开设置页后常驻 / `--help` |
| 契约头 | `include/plugin_api.h`（汇总头，只 include）＋ `include/plugin_api/*.h`（按主题的子头；纯 C、append-only、**当前 2.0**） |

最小验证（本机路径见 §8）：

```bash
# 构建（等价于 cmake --build build/Windows，带构建锁）
bash tools/verify/build.sh                 # 全量
bash tools/verify/build.sh --target tray   # 单个 target

# 端到端冒烟（在交互桌面上跑，会启动宿主）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify/smoke_capture.ps1
```

---

## 1. 仓库地图

```
include/            宿主与契约头
  plugin_api.h        ★ ABI 契约汇总头（纯 C，只 include；正文在 plugin_api/）
  plugin_api/         ★ 契约正文，按主题一个子头（basics / interfaces / 各能力 / host_services /
                      module / abi_layout；不要嵌套子目录）
  shot_plugin_helpers/ 插件侧 C++ 封装（每类一个头，汇总头只 include）
  PluginInstances/   宿主侧插件包装（ImageProcessor / ScreenCapture / ImageSaver / UIProvider / LibProvider）
  hostRuntime.h      宿主编排（生命周期锁 + tick）
  hostListCatalog.h  通用列表服务实现
  capturePipeline.h  采集流水线（worker 线程）
src/                宿主实现 + main.cpp
plugins/            13 个插件工程，各自独立 CMakeLists + metadata.json
  example_processor  一 DLL 两插件（image_processor）+ 贡献一条托盘命令
  opencv_provider    lib_provider（把 OpenCV 带进进程）
  screen_capture     shot.screen_capture（Windows GDI 多屏；Wayland 走 portal ScreenCast + PipeWire，已编译、真机未验证）
  image_saver        shot.image_saver（PNG/JPEG，stb）
  settings_ui        设置页（ImGui + GLFW + OpenGL3，跨平台）
  translator         外部语言包（Generic；扫描官方/用户目录，跨平台代码，Windows 已验证）
  tray               托盘（图标 + 数据驱动菜单，Windows）
  region_selector    区域框选覆盖层（Windows 完整交互；Wayland 为门户定格图 + 拖选，已编译、真机未验证）
  hotkey             全局热键（Windows 用 RegisterHotKey；非 Windows 编 stub，不注册）
  session_control    Linux Wayland 的 D-Bus 触发入口（Generic；代替全局热键；已随 Linux 构建通过）
  long_capture       手动滚动长截图（Windows P1；已编译通过，真机交互待验证）
  clipboard          截图 → 剪贴板（Windows CF_DIB；Linux 走 X11 selection，已编译、真机未验证）
  notify             截图结果气泡（Windows Shell 通知；Linux 用 libnotify，已编译、真机未验证）
document/           设计文档（改代码必须同步，见 §4）
cmake/              构建规则（ShotPlugin.cmake / ValidatePluginMeta.cmake / export_sdk.cmake …）
tools/verify/       真机验证脚本（带构建锁的 build 包装 + e2e 冒烟，见 tools/verify/README.md）
STYLE.md            ★ 代码风格与工程约定（硬约定）
```

---

## 2. 架构速览（改代码前必须懂）

**万物皆插件**：宿主只负责「扫描 → 校验 → 加载 → 调度」，一切能力都在 DLL 里。

> 平台状态见 [`document/PLATFORMS.md`](document/PLATFORMS.md)：Windows 是唯一真机验证过的完整链路；
> Linux 默认目标是 Wayland（Ubuntu 24.04 GNOME），X11 列为第二优先但仍缺 `XGetImage`/`XGrabKey`/
> 覆盖层；Wayland 链路（ScreenCast + PipeWire 抓屏、门户定格图 + B 方案框选、`session_control`
> 的 D-Bus 触发入口）已实现并随 Linux 构建通过，**尚未在真机验证**；`hotkey` 在 Linux 编 stub
> （提供列表与配置项、不注册热键）；`long_capture` 已随 Windows 构建通过，**交互待真机验证**；
> macOS 尚未成功构建。改平台相关代码、CMake 跳过条件或根 README 平台段前先看矩阵。

```text
插件 DLL ──(唯一导出符号 shot_module_entry)──► ShotModule{get_info, create, destroy, query_interface}
                                                      │
                          查询能力：shot.image_processor / shot.screen_capture /
                                    shot.image_saver / shot.configurable /
                                    shot.lib_provider / shot.ui_provider
```

**五类宿主服务**（插件用 `host->query_interface(host_context, id, 1)` 取）：

| 服务 id | 作用 |
| --- | --- |
| `shot.host.config` | 宿主普通键值配置（低频，如窗口几何） |
| `shot.host.event_bus` | 进程内主题发布/订阅（**跨线程通信的唯一方式**） |
| `shot.host.plugin_lists` | **通用列表服务**：按 list id 返回 JSON 快照（只读） |
| `shot.host.plugin_registry` | 已安装插件清单 + 异步启停（热卸载） |
| `shot.host.board_msg` | 旧广播接口（保留，新代码用事件总线） |

**两条最重要的设计原则**（违反会被 review 打回）：

1. **数据形状不进 ABI**：新增一类 UI/宿主数据（菜单项、配置项、热键、目标列表……）＝
   定义一个新 **list id + JSON schema**，插件用 `lists.add` 注册、`lists.changed` 通知、
   `lists.update` 写值、`lists.commit` 提交。**不要新增 ABI 结构体或宿主服务**。
   详见 [`document/LISTS.md`](document/LISTS.md)。
2. **跨线程通信只走事件总线**：宿主按加载顺序直接调用插件能力（同一 worker 线程上的
   `image_processor` 链共享同一张 `ShotImage`，不经过总线），插件之间不直接互调；
   需要通知其它线程/插件时发事件，订阅回调只在宿主 main 线程执行。宿主不硬编码任何
   插件的能力细节。

**采集流水线**（`Host::CapturePipeline`，详见 [`document/CAPTURE.md`](document/CAPTURE.md)）：

```text
capture.request（事件，main 线程入队） → worker 线程：
   screen_capture 抓整屏 → 按 region 裁剪 → 所有 image_processor 依次处理 → image_saver 落盘
   → 发布 capture.done {path,width,height,processors} / capture.failed {stage,reason}
```

`capture.request` 负载三态（**别改**）：

- 空 / 非 region → 整屏采集；
- `{"mode":"region","region":{x,y,width,height}}` → 物理虚拟桌面像素，裁剪采集；
- `{"mode":"region"}`（无 region）→ **"请开始框选"**，流水线忽略不采集（region_selector 消费）。

---

## 3. 契约（ABI）规则

- 契约头是**汇总头 + 子头**：`include/plugin_api.h` 只 include，正文在
  `include/plugin_api/*.h`（basics / interfaces / 各能力 / host_services / module / abi_layout）。
  改契约 = 改对应子头，**不要**把内容塞回汇总头；子头之间用 `#include "plugin_api/xxx.h"`。
- 契约头 **只追加**：末尾加字段、加新 interface id + 新结构体；
  永不重排/改类型/删字段。破坏性变更才涨 `SHOT_ABI_MAJOR`（当前 2）。
- 每个跨边界结构体第一个字段是 `struct_size`；读取前用 `SHOT_HAS_FIELD`。
- `abi_minor`：追加能力时 +1；`cmake/ValidatePluginMeta.cmake` 要求
  **metadata.abimajor == 头文件 major** 且 **minor ≤ 头文件 minor**。
- 所有 interface id / 配置 key 约定登记在 [`document/ABI_POLICY.md`](document/ABI_POLICY.md)，
  加新的必须补表。
- 布局锚点：`include/plugin_api/abi_layout.h` 里的 `static_assert` 是 ABI 的机器可读副本
  （汇总头里最后 include），改结构体必须同步。
- 构建期脚本按文本读契约（`cmake/ReadContractHeader.cmake` 拼接汇总头 + 全部子头），
  所以子头里的宏名/字面量格式不能随便改，`plugin_api/` 下也不要再嵌套子目录。

---

## 4. 硬性工作规则（用户会亲自 review）

1. **STYLE.md 是硬约定**：中文注释解释"为什么"、Doxygen、4 空格/K&R/`if (`、≤120 列、
   UTF-8 + LF、一个类一个头文件、`.cpp` 按职责拆分（>500 行就该拆）、所有类成员函数必须在
   `.h` 里声明（匿名命名空间内部辅助函数除外）。
2. **改代码必须同步改文档**（STYLE §9.1 矩阵）：`document/*.md`、根 `README*.md`、
   插件自己的 `README.md` 一起改；接口/行为变化还要改 `ABI_POLICY/LISTS/EVENT_BUS` 等。
3. **未完成的事写成代码里的 `// TODO:`**，不要写在文档里；做完就删掉那行（STYLE §5.2）。
4. **不要自动 `git push`**。改完 → 构建/验证 → `git add` 具体文件 → 本地 commit（中文说明）
   → 把 hash 和摘要告诉维护者，**推送由维护者手动执行**。
5. 提交时**排除 `build_number.txt`**（每次构建自动 +1，是构建脚本改的，不是你的改动）。
6. 不提交 `build/`、`sdk/`（已在 `.gitignore`）。
7. **不要为了消 bug 去"优化"已经写死原因的地方**（例如覆盖层窗口 +1px、GLFW 延迟初始化、
   隐藏用条件变量），代码注释里都写了真机实测原因，改了会复现历史 bug。

---

## 5. 常用修改配方

### 5.1 给插件加一个配置项

1. 插件内 `ShotConfigDesc` 数组加一项（`kind` 用 `SHOT_CONFIG_KIND_*`）；
2. `get_config/set_config` 里处理这个 key（不认识的 key 必须返回负值、不影响其它项）；
3. 值一律字符串；范围类只对 INT/FLOAT 有意义；PATH 项用 `options="directory"` 或 `"file"`
   指定"浏览…"弹哪种对话框（不写则按当前值推断）；STRING 项可以写 `options="hotkey"`，
   设置页会把它渲染成"当前组合键 + 录入…"控件（见 `document/SETTINGS.md` §6）；
4. 什么都不用改宿主：它会自动出现在 `config_list` → 设置页出现一节。
5. 插件 README 的配置表 + 根 README 状态表（如果是一眼可见的行为）。

### 5.2 给托盘菜单加一项（插件零代码之外的推荐做法）

在插件 `init/create` 后发布一条事件即可：

```cpp
shot::EventBus(host).publish("lists.add", R"json({
  "list":"tray_command_list","owner":"<自己的插件 id>",
  "items":[{"id":"my.command","label":"显示名","event":"some.topic","data":"","order":50}]
})json");
```

宿主校验 `owner` 必须是"当前已加载的插件 id"；同 `(list,owner,id)` 覆盖；`order` 缺省 1000；
点击时托盘只 `publish(item.event, item.data)`。插件卸载时宿主自动清掉它的贡献。
条目的 `label` 可以写文本 key（推荐用命令 id 作 key），宿主会把列表级 owner 注入每条
条目，托盘按 `display(owner,label)` 用 `i18n_list` 解析（`document/I18N.md`）；写中文
字面量的老插件照常显示。

### 5.3 给功能加热键 = 贡献 hotkey_list 条目，不要自己注册

全局热键只有 `plugins/hotkey`（`hotkey.main`）一个 provider 负责注册；功能插件只贡献数据：

```cpp
shot::EventBus(host).publish("lists.add", R"json({
  "list":"hotkey_list","owner":"<自己的插件 id>",
  "items":[{"id":"long_capture.start","combo":"Ctrl+Alt+L",
            "event":"long_capture.start","data":"","label":"long.config.hotkey.label"}]
})json");
```

- `id` 在 owner 内唯一，**同时是设置页配置键** `hotkey.main/<id>`，所以不同 owner 之间
  也应避免同名（冲突时先到者生效并 WARN）；发布后不要改；
- `combo` 是默认组合键（语法见 `plugins/hotkey/include/hotkey_binding.h`）；用户在设置页
  改过的值存 `hotkey.main/<id>`，key 不存在或非法时回落到 `combo`；
- `event` / `data` 是按下时发布的事件主题与负载（`data` 空串 = 空负载）；
- `label` 是设置页显示文本 key（i18n，缺省用 `id`）；设置页按 `display("hotkey.main", label)`
  解析（配置项 owner 是 provider），所以贡献方自己的 key 要么配 `target:"hotkey.main"` 的
  i18n 译文，要么直接写中文字面量；
- provider 在 `lists.changed` 后重读快照并**整批重注册**，注册顺序 = 列表顺序；插件卸载时
  宿主自动清掉它的 `hotkey_list` 条目；
- **不要**在功能插件里自己调用 `RegisterHotKey`，也不要为热键新增 ABI 结构体/宿主服务；
  完整 schema 与运行时语义见 `document/LISTS.md` §5.5。

### 5.4 新增一类 UI 数据（新 list）

1. 在 `document/LISTS.md` §5.6 登记新 list id + JSON schema；
2. 提供方用 `lists.add` 注册、变化时（数据变化后由宿主）发 `lists.changed`；
3. 消费方读 `shot::PluginLists(host).json("<list id>")`；
4. 需要写值就定义 `lists.update` 语义并在宿主 `HostListCatalog` 里实现对应分支
   （**只允许宿主 main 线程处理**）。

### 5.5 新建一个插件

照抄 `plugins/image_saver`（最干净的样板）：

```text
plugins/<name>/{CMakeLists.txt, metadata.json, README.md, include/, src/}
```

- `CMakeLists.txt` 用 `shot_add_plugin(<target> METADATA metadata.json SOURCES ... INCLUDE_DIRS include)`
  （Windows-only 插件先判断 `if(NOT WIN32)` → `set(SHOT_PLUGIN_SKIPPED TRUE PARENT_SCOPE)` + `return()`）；
- `metadata.json`：`abi` 与契约头一致、`interfaces` 必须是契约里真实存在的 id、`requires` 写
  `core>=0.1.0`；
- `src/plugin_entry.cpp` 是唯一导出 `shot_module_entry` 的 TU，ABI 胶水只放这里；
- 纯订阅者/纯配置插件（只声明 `shot.configurable`）也能加载——宿主会按 `PluginType::Generic`
  包装（**前提是有 metadata**）；订阅必须在 `create` 做、`destroy` 里退订。

### 5.6 新增一种能力（ABI minor +1）

1. 契约头：新 interface id 宏加进 `plugin_api/interfaces.h`，新 `...V1` 结构体加进对应的
   能力子头（`plugin_api/*.h`，末尾定义、带 `struct_size`），布局 `static_assert` 加进
   `plugin_api/abi_layout.h`；汇总头 `plugin_api.h` 只 include，不用动；
2. `SHOT_ABI_MINOR` +1；所有插件 `metadata.json` 的 `abi.minor` 可以不跟（校验只要求 ≤）；
3. `document/ABI_POLICY.md` §4/§6 登记；
4. 宿主 `PluginInstances/` 加包装 + `PluginManager::createPlugin` 识别 + `PluginType` 枚举；
5. 新插件实现它并在 README 写清线程/返回码语义。

### 5.7 UI 插件的热卸载要求（硬要求）

`deinit` 返回前必须：停线程 → 销毁窗口 → **`UnregisterClassW` 注销窗口类**（类属于进程，
不随 DLL 卸载消失，不注销会在禁用→启用后踩悬空 WndProc，历史上崩过）→ 退订事件总线 →
join。`host.shutdown` 事件要先响应。

---

## 6. 构建与验证

### 6.1 构建

仓库里已固化构建包装脚本 `tools/verify/build.bat`（vcvars + cmake）与 `tools/verify/build.sh`（加构建锁）：

```bat
@echo off
cd /d "D:\src\repo\plugin-shot\Host"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
"D:\src\cmake\bin\cmake.exe" --build build/Windows %*
exit /b %ERRORLEVEL%
```

在本会话的 shell 里**不能直接 `cmd //c`**（会被包装吃掉参数），统一用：

```bash
powershell.exe -NoProfile -Command "& 'D:\src\repo\plugin-shot\Host\tools\verify\build.bat' --target tray 2>&1 | Select-Object -Last 40; exit \$LASTEXITCODE"
```

`tools/verify/build.sh` 只是给上面这条命令加了**构建锁**（多个 agent/终端共用同一个
Ninja 目录时必须串行；锁目录 `build/.build-lock` 里写 PID，能自动清理过期锁）。用法：
`bash tools/verify/build.sh [cmake --build 的参数]`，**调用时给足超时**（默认 2 分钟不够，
建议 600000ms）。

### 6.2 冒烟 / e2e 脚本

常用验证脚本已固化在 `tools/verify/`（用法与断言语义见 `tools/verify/README.md`）：

| 脚本 | 验证内容 |
| --- | --- |
| `smoke_capture.ps1` | `--capture`：exit 0、日志有 `Capture saved:`、落盘尺寸 = 物理虚拟桌面 |
| `smoke_cursor_capture.ps1` | 抓屏合成鼠标光标：光标两个位置各截一张，比对对应区域像素差异且尺寸 = 物理虚拟桌面 |
| `smoke_settings.ps1` | `--settings`：设置窗口出现、能截到画面、日志有"设置窗口已显示"、无 error（窗口查找语言无关） |
| `e2e_hotkey_capture.ps1` | 常驻 + 配置里的截屏热键 → 采集落盘（hotkey + 流水线 + 保存） |
| `e2e_clipboard.ps1` | 截图后系统剪贴板里有一张图（CF_DIB） |
| `smoke_settings_i18n.ps1` | 多语言：`core/language=en-US` / `zh-CN` 两次启动设置页，标题/截图/日志锚点（`界面语言 = <tag>`），备份恢复 config |
| `smoke_i18n_translator.ps1` | 外部语言包：翻译插件扫描官方/用户目录，`@language/` 与 `target` 条目、用户包优先级、坏包只 WARN（与翻译插件同期新增） |
| `smoke_region_window.ps1` | 区域框选：悬停窗口吸附、单击截窗口（尺寸 = DWM 扩展边界）、拖动选区尺寸、边框/折角主题色（像素抽检）；自己开关目标窗口并恢复 config |

运行方式（**必须带 `-ExecutionPolicy Bypass`**，原因见 §7 第 15 条）：

```bat
tools\verify\run.bat smoke_settings.ps1
:: 或
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify/smoke_settings.ps1
```

`build/verify/` 里还留着开发期的一次性探针脚本（托盘点击、区域框选、DPI 探针、假宿主
harness 等），**不入库**，换机器会丢；需要时按同样思路重写一个。

运行宿主的脚本大多会**杀掉已有宿主进程**、写 `config.json`，所以：

- **同一时间只允许一个 agent/终端跑宿主 e2e**（互相抢焦点/抢 config/被 kill_running 杀掉，
  结果不可信）；
- 跑前确认 `build/Windows/bin/plugins/` 里**没有别人的临时探针 DLL**（探针会污染菜单/配置/退出流程；
  社区约定：验证完立刻删）。

---

## 7. 已踩过的坑（真机血泪，改动前务必读）

1. **全屏覆盖窗口不要"刚好等于显示器尺寸"**：尺寸与显示器完全相等时 Windows 会判定为全屏窗口、
   走 fullscreen optimization，表现为显示/隐藏闪一下，进程退出后甚至留下黑色残影（要重启 dwm）。
   修法：窗口比显示器**每边大 1px**（`kOverlayOversize`，见 `plugins/region_selector`）。
2. **不要用透明帧缓冲做覆盖层**：`GLFW_TRANSPARENT_FRAMEBUFFER` 首次可用，`hide→show` 一轮后
   DWM 合成失效 → 整屏黑 + 只剩十字线。现在的做法是"先截桌面快照当背景的不透明窗口"。
3. **不要 hide/show 覆盖层来消闪**：`ShowWindow`/`HideWindow` 触发 DWM 过渡动画，同样闪；
   而且"挪到屏幕外代替隐藏"会让 DWM 留下残影。目前用**普通 hide/show + 尺寸 +1px**。
4. **GLFW 全局 helper window 只属于"第一个 `glfwInit` 的线程"**：`glfwPostEmptyEvent` 只唤醒那
   个线程。所以后加载的插件**不要**在启动阶段 init GLFW，否则 settings_ui 的 `stop()` 永远等不到
   唤醒，宿主退出卡 30s。region_selector 因此延迟到第一次框选才初始化 GLFW，隐藏态用条件变量唤醒。
5. **DPI**：宿主 `main()` 最先声明 `PER_MONITOR_AWARE_V2`；采集插件再对线程做
   `SetThreadDpiAwarenessContext` 兜底。坐标一律物理像素，`content scale` 只放大字号、**绝不乘坐标**。
6. **窗口类属于进程，不随 DLL 卸载**：UI 插件 deinit 必须 `UnregisterClassW`，否则热卸载→重新加载
   会踩已卸载代码里的 WndProc（0xC000041D，历史上崩过）。
7. **`kill_running.bat` 会在链接前杀掉正在运行的 `Plugin-shot.exe`**：并行验证/构建时会误杀别人的
   e2e，这不是崩溃。
8. **多 agent 共用一个构建目录**：任何一方写了"引用了不存在源文件"的 CMakeLists，全仓 configure
   都会失败（报 `shot_add_plugin` / `Cannot find source file`）。写 CMakeLists 时保持可配置。
9. **CMake `if(NOT CMAKE_MATCH_1)` 会把字符串 "0" 当假**：曾导致 `SHOT_ABI_MINOR 0` 报"契约头里
   找不到"，已改为 `if(NOT _m)`。
10. **文件系统大小写**：NTFS 不区分大小写，`include/PluginManager.h` 曾与代码里的
    `#include "pluginManager.h"` 不一致，Linux 干净 checkout 才炸（已改 git 跟踪名）。
11. **PowerShell 5.1 读 UTF-8 脚本需要 BOM**：否则中文串变乱码、语法报错。新写 `.ps1` 一律
    **UTF-8 + BOM + CRLF**；`.bat` 只写英文注释。
12. **GDI 截屏对 layered/透明窗口不可靠**：用截图验证覆盖层效果时，"看着没变"可能是抓取问题而不是
    没渲染，优先看日志（`region_selector` 会打印"已截取覆盖层背景"）。
13. **`--capture` 一次性模式**必须在退出前 `dispatchEvents()` 一轮，否则剪贴板/通知类订阅者收不到
    `capture.done`（已修，勿回退）。
14. **config.json 会被多个宿主进程整文件覆盖**：并行跑宿主时测试配置会被互相踩，验证脚本要
    备份/恢复，测完确保 `plugins.disabled` 等恢复原值。
15. **PowerShell 执行策略会拒跑"新建目录"里的脚本**：本机 `RemoteSigned`，但仓库里新建的
    `tools/`、`scripts/` 下的 `.ps1` 会被判"not digitally signed"（`build/` 反而不受影响）。
    统一用 `-ExecutionPolicy Bypass` 启动：`tools\verify\run.bat <script>` 已包好。
16. **cmd 批处理括号块里的 `%VAR%` 在整块解析时展开**：`tools/verify/build.bat` 曾把
    vcvars 调用包在 `if not defined VCINSTALLDIR ( ... call "%VSPATH%\..." )` 里，for 循环
    set 的 `VSPATH` 在解析整块时还是空串 → vcvars 从未执行 → 所有编译报
    `C1083: 无法打开包括文件: "cstdint"`。修复：改用 `goto` 流程，别把 `set`/`call`
    包进括号块（或 `setlocal EnableDelayedExpansion` + `!VAR!`）。
17. **Win11 会把裸按 PrintScreen 交给系统截图工具**：默认开启的「设置 → 辅助功能 → 键盘 →
    使用 Print Screen 键打开截图工具」会先消费掉裸 PrintScreen，`RegisterHotKey(VK_SNAPSHOT)`
    虽然返回成功，但 `WM_HOTKEY` 不会到达插件（本机实测：keybd_event/SendInput 各种方式都不行）。
    带修饰键的 `Ctrl+PrintScreen` 等不受影响（实测能触发并落盘）。hotkey 插件注册裸
    PrintScreen 时会打 WARN；要用裸键需关闭该 Windows 设置。

---

## 8. 本机环境（Windows 开发机）

| 项 | 路径 / 值 |
| --- | --- |
| CMake | `D:\src\cmake\bin\cmake.exe`（4.4.0-rc1） |
| vcpkg | `D:\src\vcpkg`（`VCPKG_ROOT` 已设置；清单模式，baseline 见 `vcpkg-configuration.json`） |
| MSVC | VS 18 Community：`C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat` |
| Ninja | 随 VS / `cmake --build` 自动调用 |
| 显示器 | 双屏混合 DPI，虚拟桌面 3584x1600（详见下方说明） |
| WSL | Ubuntu 24.04（Linux 构建已验证，见 `.github/workflows/cmake.yml`） |
| Python | DSH 运行时自带，装了 Pillow/numpy；路径用 `load_workspace_dependencies` 查 |

显示器细节（写测试/坐标相关代码时用）：

- 主屏：物理 `(0,0)` 起，`2560x1600`，150% DPI（逻辑 1707x1067，content scale 1.5）
- 副屏：物理 `(2560,0)` 起，`1024x768`，100% DPI（content scale 1.0）
- 物理虚拟桌面 = `3584x1600`，origin `(0,0)`；两个屏都在原点右侧，**负 origin 未实机验证**

---

## 9. 当前 backlog（按优先级）

1. **CI 跑绿（进行中）**：billing 已解除（维护者确认），最近的 run 已真实执行：Windows job ✅，
   Linux / macOS 在 configure 阶段失败，根因是 `vcpkg-configuration.json` 的 builtin-baseline
   指向维护者 fork 上的合并提交（CI fresh clone 里没有该对象，vcpkg `git show` 失败）——已改成
   上游 microsoft/vcpkg 上的等价提交 `44819aa…`（`versions/baseline.json` 内容一致，端口版本不变）。
   待维护者 push 后看 CI 结果；macOS 从未真机构建，可能仍会暴露编译问题。
   Windows job 另有一个隐患：`ilammy/msvc-dev-cmd` 之后 VCPKG_ROOT 被指到 VS 自带 vcpkg，
   Windows 实际没用工作流 clone 的 vcpkg；本轮未动，等后续单独处理。
2. ~~把关键验证脚本固化进仓库~~ **已完成**：`tools/verify/`（build + 冒烟 / e2e，见 §6.2）。
   可继续把托盘点菜单、区域框选、DPI 探针也整理进去。
3. **跨屏框选**（`region_selector` TODO）：现在只覆盖鼠标所在显示器，跨屏选择被夹在当前屏。
4. ~~光标合成~~ **已完成**：`plugins/screen_capture` 可选 `include_cursor`（默认开），
   `GetCursorInfo` + `DrawIconEx` 合成；`tools/verify/smoke_cursor_capture.ps1` 用两处光标
   位置的像素差异验证通过（13/13）。
5. **负 origin 多屏**：代码链已复核 + 桩测试通过，但未实机验证（本机两屏都在原点右侧）。
6. **macOS**：无真机；代码级审计已做（`base.cpp` 用 `_NSGetExecutablePath`），已知风险：
   Mach-O 贴尾可能影响 ad-hoc 签名、settings_ui 的 GLFW 要求主线程。
7. ~~主循环 16ms 轮询~~ **已完成**：main 循环改为事件驱动阻塞等待
   （`HostServices::waitForActivity()` + `requestMainThreadWork()` 唤醒，1s 仅兜底），
   见 `src/main.cpp` 与 `document/EVENT_BUS.md` §3。
8. **录屏能力**：契约里留了 TODO（新 interface id + 结构体），尚未实现。
9. 产品向可选：另存为/上传入口、设置页搜索、配置导入导出。

---

## 10. 多 agent 协作约定（如果用 Agent Teams）

- **写作用域不重叠**：每个 agent 只改自己任务列出的文件/目录；跨作用域（契约头、别人的插件、
  根 README/document）先问 Lead。
- **构建串行**：一律走 `tools/verify/build.sh`；构建被别的进程占用（`LNK1104`）是正常现象，
  等它退出重试。
- **宿主 e2e 串行**：同一时间只允许一个 agent 跑宿主；跑完恢复 `config.json`、删掉自己放进去的
  探针 DLL、确认没有残留 `Plugin-shot.exe`。
- **不要 git commit/push**：多 agent 共用工作区，提交由 Lead 统一做（且**不 push**）。
- 汇报时给证据：构建 exit code、日志关键行、落盘文件尺寸、截图路径（`build/verify/` 下）。

---

## 11. 交接检查清单（接第一单前）

- [ ] 读过 [`STYLE.md`](STYLE.md) 与本文档
- [ ] `bash tools/verify/build.sh` 能 exit 0（没有就先修构建环境）
- [ ] 知道配置文件/日志在哪，能跑一次 `Plugin-shot.exe --capture` 并看到落盘
- [ ] 知道"数据形状不进 ABI"这条铁律，以及 `document/LISTS.md` 在哪
- [ ] 知道改完代码要同步文档、TODO 只写在代码里、**不自动 push**
