# settings_ui —— 设置窗口插件（`shot.ui_provider`）

无主窗口架构下的设置页：窗口、OpenGL 上下文和 ImGui 全部由插件自己创建和管理，
宿主只调 `init` / `deinit` / `post_to_plugin`，既不知道插件开了几个线程，也拿不到窗口句柄。
配置项来自宿主列表服务 `shot.host.plugin_lists` 的 `config_list` JSON 快照，插件启停来自
`shot.host.plugin_registry`；写值与提交是事件（`lists.update` / `lists.commit`），本插件
**不直接调用任何其它插件**——所有读写都由宿主转调（契约与线程模型见
`document/LISTS.md`、`document/SETTINGS.md`、`document/EVENT_BUS.md`）。

窗口层用 **GLFW**（跨平台：Windows / Linux / macOS 同一套代码），渲染用
**ImGui + OpenGL3**，PATH 配置项的「浏览…」用 **nativefiledialog-extended**。

## 1. 目录

```
plugins/settings_ui/
├── CMakeLists.txt                  # shot_add_plugin + imgui / glfw3 / nfd / nlohmann-json 依赖
├── metadata.json                   # 插件清单（id settings_ui.main，接口 shot.ui_provider）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── settings_window.h           # SettingsWindow 公共接口（PIMPL，不含平台/图形库类型）
│   ├── settings_panel.h            # SettingsPanel 公共接口（PIMPL，不含模型细节）
│   ├── settings_window_impl.h      # 内部头：SettingsWindow::Impl 与窗口内部类型
│   ├── settings_panel_impl.h       # 内部头：SettingsPanel::Impl 与设置页模型
│   └── settings_dialog.h           # 内部头：原生文件/目录对话框（NFD）封装
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + kEntries + ABI 胶水
    ├── settings_window.cpp         # 生命周期 / GLFW 窗口 / 事件循环 / 事件队列 / 显示隐藏
    ├── settings_graphics.cpp       # ImGui 后端 + 字体/DPI + 每帧渲染
    ├── settings_geometry.cpp       # 窗口几何持久化 + 屏幕内 clamp
    ├── settings_dialog.cpp         # nativefiledialog-extended 的封装与降级
    ├── settings_panel.cpp          # SettingsPanel 公共接口转发 + render() 顶层框架
    ├── settings_model.cpp          # config_list JSON 解析 + 模型刷新 + 跨文件共用的值解析/格式化工具
    ├── settings_form.cpp           # 操作条 + 配置节 + 7 种 kind 的控件渲染
    ├── settings_plugin_table.cpp   # 插件管理表渲染
    └── settings_apply.cpp          # 应用状态机（lists.update / commit）、取消 / 恢复默认、事件 / 等待超时
```

公共头一个类一个（`SettingsWindow` / `SettingsPanel` 各占一个），`*_impl.h` 与
`settings_dialog.h` 是**内部头**：只被本插件的 `.cpp` 包含。`.cpp` 按关注点拆分
（模型 / 表单 / 插件表 / 提交、生命周期 / 图形 / 几何 / 对话框），每个文件都能一句话
说清职责（`STYLE.md` §6.8）。

## 2. 构建

宿主仓库内构建（`shot_add_plugin()` 由根工程提供）：

```bash
cmake -S . -B build/settings_ui -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/settings_ui --target settings_ui
```

离线 SDK 独立构建：

```bash
cmake -S plugins/settings_ui -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

产物：`<构建树>/bin/plugins/settings_ui.dll`（仓库内整体构建时是
`build/Windows/bin/plugins/settings_ui.dll`；下面示例用自己的 `build/settings_ui` 构建树）。本插件在宿主
仓库内构建时，根 `CMakeLists.txt` 会通过 `plugins/*/CMakeLists.txt` 的 glob 自动纳入，不需要手改根文件。

依赖（都由 vcpkg 清单模式提供，插件不链接 spdlog）：

| 依赖 | 用途 | 说明 |
| --- | --- | --- |
| `imgui`（glfw / opengl3 binding） | 表单控件与后端 | 静态库，两个后端已编进 `imgui.lib`，只链接 `imgui::imgui` |
| `glfw3` | 窗口 / OpenGL 上下文 / 事件循环 | vcpkg 导出的 CMake 包名是 `glfw3`，target 是 `glfw`；是动态库，运行时与宿主同目录 |
| `nativefiledialog-extended` | PATH 的「浏览…」 | 动态库；vcpkg 包名与它同名，CMake 包名却是 **`nfd`**（target `nfd::nfd`） |
| `nlohmann-json` | 解析 `config_list` / `lists.updated` / `plugins.changed` 的 JSON 负载 | header-only，不引入运行库 |
| OpenGL | 渲染 | Windows 链系统 `opengl32`；Linux/macOS 链 `OpenGL::GL` |

**平台支持**：

| 平台 | 状态 |
| --- | --- |
| Windows（MSVC x64） | ✅ 已验证（构建 + 端到端冒烟） |
| Linux / macOS | **未验证**：代码走同一套 GLFW + OpenGL3 + NFD 分支、CMake 也有 `OpenGL::GL` 分支，但当前开发机只有 Windows，没有真机验证 |

POSIX 未验证的具体点（都留了代码里的 `// TODO:`）：

- macOS 上 GLFW（Cocoa）要求窗口与事件循环在进程主线程，而本插件的 UI 线程是自建线程；
- macOS 上 NFD（NSOpenPanel）可能同样要求主线程，且主线程调用才最稳；
- Linux 的中文字体路径（Noto CJK / 文泉驿）只是常见发行版的候选；
- `glfwGetMonitorWorkarea` 在没有窗口管理器的环境下可能拿不到，代码会退回视频模式分辨率；
- 多显示器且各屏 DPI 不同时没有验证：clamp 用的是窗口当前 DPI 的边框尺寸（本机单屏
  150% 已验证），跨 DPI 屏需要按目标显示器重新取 `glfwGetWindowFrameSize`（代码里有 TODO）。

## 3. 生命周期与事件

| 时机 | 插件做什么 |
| --- | --- |
| `init(self, host)` | 订阅 5 个主题（下表全部）；起 UI 线程，`glfwInit` 后建**隐藏**窗口 + GL 上下文 + ImGui；返回 `0` 时已就绪 |
| `settings.open` | 显示窗口 + 全量刷新模型（窗口已显示时只刷新，不重置位置） |
| `config.applied` | 保存完成：结束"保存配置"等待；空闲时重建配置区（保留未应用的本地修改，见 §4） |
| `lists.changed` | `list == "config_list"` 时重建配置区：按 owner+id 把未应用的本地修改合并回去，不丢用户正在编辑的值 |
| `lists.updated` | 写值结果：按同 `tag` 匹配本次提交；`ok=false` 的项标红显示 `reason`，然后自动进入 `lists.commit` |
| `plugins.changed` | 整表重新枚举注册表与配置列表（节可能增删），把负载里的 `reason` 显示在对应行，结束"应用中…" |
| X / Esc | 只隐藏窗口：点 X 清掉 GLFW 的 should-close 标记，Esc 由键盘回调置位后统一 `glfwHideWindow`；线程、窗口、GL 上下文全部保留 |
| `host.shutdown` / `deinit` | 先退订事件总线，再让事件循环退出并 `join` UI 线程，最后销毁窗口、终止 GLFW |

窗口几何在每次隐藏（以及宿主退出时窗口仍开着）时写进**宿主配置**的
`settings_ui.window`（字符串 `"x,y,w,h"`，GLFW 的**内容区**坐标/大小），下次显示前恢复，
并用 `glfwGetMonitorWorkarea` 做屏幕内 clamp——换分辨率、拔掉副显示器后窗口不会跑到屏幕外。

clamp 的坐标语义与边框处理（实测确认）：GLFW 的窗口位置/尺寸/工作区/边框是**同一坐标
空间**；Windows 上进程被 GLFW 设为 per-monitor DPI aware，所以都是**物理像素**（150%
缩放下 760x620 的内容区外层是 782x676、标题栏 45px）。保存的是内容区，但"完整可见"
要求**外层矩形**落在工作区内，所以 clamp 用 `glfwGetWindowFrameSize` 把标题栏/边框一起
算进去，先夹尺寸、再夹位置（含标题栏与底边）。

事件总线回调只做两件事：拷进队列 + `glfwPostEmptyEvent` 唤醒 UI 线程。事件循环空闲时
隐藏状态阻塞在 `glfwWaitEvents()`、可见状态用 `glfwWaitEventsTimeout(0.016)` 驱动重绘；
所有 GLFW / ImGui 调用都只在插件自己的 UI 线程上发生。

## 4. 表单与编辑模型

顶部固定操作条（`应用` / `取消` / `恢复全部默认` / 状态文本），下面是一页按提供者分
`CollapsingHeader` 的滚动内容，宿主 `core` 固定在最前，插件管理表紧随其后。

| `SHOT_CONFIG_KIND_*` | 控件 | 值格式 |
| --- | --- | --- |
| `BOOL` | Checkbox | `"1"` / `"0"` |
| `INT` / `FLOAT` | Slider + Input（用 `min/max/step`，越界 clamp，非法输入回退） | 十进制字符串 |
| `ENUM` | Combo（`options` 按 `\|` 切） | 选项字符串 |
| `STRING` | InputText | 原样 |
| `COLOR` | ColorEdit4 + hex 输入 | 统一存 `#RRGGBBAA`，读取兼容 `#RRGGBB` / `R,G,B,A` |
| `PATH` | InputText + `浏览…`（nativefiledialog-extended） | 原样 |
| 未知 kind | 灰色"不支持的配置类型"，不崩 | —— |

`PATH` 的「浏览…」：描述符 `options` 写 `"directory"`（或 `"dir"` / `"folder"`）时弹
"选择文件夹"，写 `"file"` 时弹"打开文件"；两者都没写时按当前值推断——当前值指向**已存在
的目录**就弹"选择文件夹"，否则弹"打开文件"
（与旧版"只选文件"一致，目录是额外支持）；描述符里没有文件/目录类型信息，只能按现状判断。
对话框库不可用（如 Linux 缺 GTK/portal）时，在配置项下方显示原因并**降级为手输路径**。

每项都有 `label` + 悬停 tooltip（含配置键）+ `恢复默认` 小按钮。编辑采用**本地副本 +
应用/取消**（`document/SETTINGS.md` §4、`document/LISTS.md`）：

1. 打开时把值读进本地缓冲，拖滑块只改缓冲（不会每帧写宿主打断插件）；
2. 点「应用」：脏配置项打包发布 `lists.update`（带本插件自增 `tag`），等**同 tag** 的
   `lists.updated`；操作条显示「应用中…（写入配置）」；
3. 逐项结果：`ok=false` 的项标红「写入失败：reason」并**保留脏标记**，其余项以提交值
   为新基线；然后自动发布 `lists.commit`（同 tag），等 `config.applied` 确认落盘
   （保存失败时宿主回同 tag 的 `lists.updated` + `error`）；两个等待都有 5 秒超时兜底；
4. 配置阶段结束后再登记插件启停（`shot.host.plugin_registry` 不变）：有变更时进入
   「应用中…（等待宿主完成插件启停）」，等 `plugins.changed` 到达后刷新并结束等待
   （5 秒超时兜底）；
5. 点「取消」丢弃全部未应用修改；「恢复全部默认」只把值改回 `default`，仍要点「应用」。

插件管理表列：启用复选框 / 插件名(id) / 版本 / 状态（已加载、已禁用、加载失败：原因）。
勾选只改本地 pending，应用时才 `set_enabled`；入口插件（`settings_ui.main` 自己）的
复选框置灰——禁用入口插件等于把当前设置页一起卸载，`document/SETTINGS.md` §3.1 明确
这一行由设置页负责置灰，宿主不加特判。

## 5. 为什么这么设计

| 决定 | 理由 |
| --- | --- |
| 窗口/线程/GL 上下文全在 `Impl` 里（PIMPL） | 平台/图形库类型不可能顺着契约漏给宿主；公共头连 `<GLFW/glfw3.h>` 都不包含 |
| 一个类一个头文件，`.cpp` 按关注点拆 | 头文件是类的完整契约（`STYLE.md` §6.7/§6.8）：模型 / 表单 / 插件表 / 提交各自一个文件，改渲染不用碰模型，改窗口不用碰设置页 |
| GLFW 而不是各平台自己写窗口层 | 一套窗口/上下文/输入代码跨平台；事件循环用 `glfwWaitEventsTimeout` / `glfwWaitEvents`，宿主事件用 `glfwPostEmptyEvent` 唤醒 |
| 事件回调只入队 + `glfwPostEmptyEvent` | 总线回调跑在宿主分发线程上，而 GLFW / ImGui 不是线程安全的；唤醒后统一在 UI 线程 drain |
| 配置数据用 `config_list` JSON + `lists.*` 事件 | 数据形状不进 ABI（`document/LISTS.md`）：宿主只暴露只读 JSON 快照，写值与提交是事件、天然单线程；插件新增配置项设置页代码不动 |
| 本地编辑 + 应用/取消 | 避免每帧写宿主；批量提交走 `lists.update` + `lists.commit`，逐项结果（失败原因）能回到界面上（`document/LISTS.md`） |
| 列表数据驱动而不是硬编码配置项 | 插件新增配置项只改自己的 `kConfig[]`；宿主把它聚进 `config_list`，设置页按条目渲染，代码一行不动 |
| 请求 3.3 core 上下文 | ImGui 1.92 的 OpenGL3 后端在 core profile 下最省心；macOS 另加 forward-compatible |
| GLSL 按 GL 版本选 150/130 | 3.2+ 用 150，3.0/3.1 用 130；低于 3.0 干净失败（`NewFrame` 建 shader 失败是 `IM_ASSERT`） |
| 中文字体按平台候选（msyh / PingFang / Noto CJK / WenQuanYi） | 只是显示效果降级；全部缺失时退回 ImGui 内置字体，功能不受影响 |
| DPI：content scale + `style.ScaleAllSizes` + `FontScaleMain` | ImGui 1.92 的字体缩放字段；不改进程级 DPI 感知 |
| 几何存宿主配置，`io.IniFilename = nullptr` | 不在插件目录/工作目录旁边生成 `imgui.ini`；几何属于用户界面状态，不占用任何插件配置节 |
| 几何恢复 clamp 计入窗口边框 | 上次位置可能来自别的显示器；把标题栏/边框算进去，外层矩形完整落在工作区内，不会"打开看不见"或底部被裁 |
| 原生对话框只经 `settings_dialog` 一个模块 | 平台差异与库生命周期收在一处；设置页模型不需要知道窗口句柄 |

## 6. 降级行为

宿主比插件老、插件被热卸载、驱动/对话框不可用……都必须是**安静降级**，不能让宿主崩：

| 情况 | 行为 |
| --- | --- |
| 宿主没有 `shot.host.plugin_lists`（老宿主） | 配置区显示「宿主不支持列表服务（shot.host.plugin_lists）」降级，插件管理照常渲染 |
| `config_list` 读不到 / 不是合法 JSON | 配置区显示具体降级原因（并记一条 WARN），插件管理照常渲染，不崩 |
| 宿主没有 `shot.host.plugin_registry` | 显示"宿主未提供插件注册表服务"提示，配置表单照常渲染 |
| 宿主没有事件总线 | 记一条 WARN，窗口不会自动弹出（只能靠宿主 `post_to_plugin` 投递 `settings.open`）；配置写不出去，插件启停仍可用 |
| 配置提供者被热卸载 | 收到 `plugins.changed` 后整表重读；消失的节显示成灰色"插件已卸载"一节，下一次刷新消失 |
| 某个配置项写入被拒绝 | 该项标红「写入失败：原因」并保留脏标记，不中断其它项的提交 |
| `lists.updated` / `config.applied` / `plugins.changed` 等不到 | 5 秒超时兜底：结束"应用中…"并显示原因，未应用的修改保留，可以重试 |
| 文件对话框不可用 | PATH 项下方提示原因并降级为手输路径（记一条 WARN） |
| 系统字体全部缺失 | 退回 ImGui 内置字体并记 WARN（中文显示为方块，功能正常） |
| OpenGL < 3.0 或 shader 创建失败 | `init` 返回失败并记 ERROR；宿主按"这个 UI 插件初始化失败"处理，其余功能不受影响 |
| GLFW 初始化 / 建窗口失败 | `init` 返回失败并记 ERROR；宿主照常运行 |
| 所有读取失败 | 不抛异常：`shot::PluginLists` / `shot::PluginRegistry` 在服务缺失时返回空结果 |

## 7. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 多个设置窗口 / 多实例 | 单实例足够；多实例会出现"两处同时改同一项"的冲突 |
| 每帧写回插件（无「应用」按钮） | 拖滑块会打断插件，且无法批量提交与撤销 |
| 回滚已经写入的配置项 | `lists.update` 是逐项语义：失败的项标红并保留脏标记，成功项不回滚——界面上能逐项看到结果，比整批回滚更可解释 |
| 在设置页安装/删除插件 | 只管理**已安装**插件的启停，安装属于安装器层 |
| 自己写各平台窗口层 | GLFW 已经把这层做稳了；再写一份只会多三份平台代码 |
| 生成 `imgui.ini` / `imgui_log.txt` | 窗口几何进宿主配置；日志走宿主日志服务 |
| 链接 spdlog | 插件只能通过 `host->log`（`shot::Logger`）写日志（`STYLE.md` §6.4） |

## 8. 检查清单

- [ ] 新增配置项只改插件自己的 `kConfig[]`，没有改设置页代码？
- [ ] 配置值仍然一律用字符串、没有跨边界传结构体/数字？
- [ ] 不认识的 kind / 读不到的提供者仍然是安静跳过，而不是报错崩溃？
- [ ] 收到 `lists.changed` / `config.applied` 才重建模型，没有在渲染循环里每帧读 `list_json`？
- [ ] `lists.update` / `lists.commit` 都带本插件自增 `tag`，且只接受同 tag 的 `lists.updated`？
- [ ] 未应用的本地修改在 `lists.changed` 刷新后仍然保留（按 owner+id 合并回脏值）？
- [ ] 事件总线回调里只做入队与唤醒，没有直接碰 GLFW / ImGui？
- [ ] `deinit` / `host.shutdown` 返回前退订了事件总线、停了 UI 线程、销毁了窗口？
- [ ] 异常没有越过 `extern "C"` 边界（`create` / `init` / `post` 都有 catch 兜底）？
- [ ] 平台 / 图形库类型仍然只出现在内部头 `settings_window_impl.h` 与窗口相关 `.cpp` 里（公共头不含 `<GLFW/glfw3.h>`）？
- [ ] `metadata.json` 的 `interfaces` 与 `query_interface` 的实际回答一致（只有 `shot.ui_provider`）？
- [ ] 改了 `lists.*` / `plugins.changed` / `settings.open` 的语义？→ 同步 `document/LISTS.md`、`document/SETTINGS.md`、`document/EVENT_BUS.md`
