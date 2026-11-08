# settings_ui —— 设置窗口插件（`shot.ui_provider`）

无主窗口架构下的设置页：窗口、OpenGL 上下文和 ImGui 全部由插件自己创建和管理，
宿主只调 `init` / `deinit` / `post_to_plugin`，既不知道插件开了几个线程，也拿不到 `HWND`。
配置项与插件启停都来自两个宿主服务（`shot.host.plugin_config` / `shot.host.plugin_registry`），
本插件**不直接调用任何其它插件**——所有读写都由宿主转调（契约与线程模型见
`document/SETTINGS.md`、`document/EVENT_BUS.md`）。

## 1. 目录

```
plugins/settings_ui/
├── CMakeLists.txt                  # shot_add_plugin + imgui / nlohmann-json 依赖
├── metadata.json                   # 插件清单（id settings_ui.main，接口 shot.ui_provider）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── settings_window.h           # SettingsWindow 公共接口（PIMPL，不含 Win32 类型）
│   ├── settings_panel.h            # SettingsPanel 公共接口（PIMPL，不含模型细节）
│   ├── settings_window_impl.h      # 内部头：SettingsWindow::Impl 与窗口内部类型
│   └── settings_panel_impl.h       # 内部头：SettingsPanel::Impl 与设置页模型
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + kEntries + ABI 胶水
    ├── settings_window.cpp         # 生命周期 / 窗口线程 / 消息循环 / 事件队列 / WndProc
    ├── settings_graphics.cpp       # WGL 上下文 + ImGui 后端 + 字体/DPI + 每帧渲染
    ├── settings_geometry.cpp       # 窗口几何持久化 + 屏幕内 clamp
    ├── settings_panel.cpp          # SettingsPanel 公共接口转发 + render() 顶层框架
    ├── settings_model.cpp          # 模型刷新 + 跨文件共用的值解析/格式化工具
    ├── settings_form.cpp           # 操作条 + 配置节 + 7 种 kind 的控件渲染
    ├── settings_plugin_table.cpp   # 插件管理表渲染
    └── settings_apply.cpp          # 应用 / 取消 / 恢复默认 / 事件 / 等待超时
```

公共头一个类一个（`SettingsWindow` / `SettingsPanel` 各占一个），`*_impl.h` 是**内部头**：
只被本插件的 `.cpp` 包含，放 PIMPL 定义与模型类型；`.cpp` 按关注点拆分（模型 / 表单 /
插件表 / 提交、生命周期 / 图形 / 几何），每个文件都能一句话说清职责（`STYLE.md` §6.8）。

产物：`<构建树>/bin/plugins/settings_ui.dll`（仓库内整体构建时是
`build/Windows/bin/plugins/settings_ui.dll`；下面示例用自己的 `build/settings_ui` 构建树）。本插件在宿主
仓库内构建时，根 `CMakeLists.txt` 会通过 `plugins/*/CMakeLists.txt` 的 glob 自动纳入，不需要手改根文件。

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

依赖（都由 vcpkg 清单模式提供，插件不链接 spdlog）：

| 依赖 | 用途 | 说明 |
| --- | --- | --- |
| `imgui`（`win32-binding` + `opengl3-binding`） | 表单控件与窗口后端 | vcpkg 的 imgui 是**静态库**，两个后端 `.cpp` 已编进 `imgui.lib`，本工程只链接 `imgui::imgui` |
| `nlohmann-json` | 解析 `plugins.changed` 的 JSON 负载 | header-only，不引入运行库 |
| `opengl32` / `gdi32` / `user32` / `comdlg32` | WGL、像素格式、"浏览…"文件对话框 | 系统库 |

**仅 Windows**：非 Windows 平台本工程直接跳过（`SHOT_PLUGIN_SKIPPED`），因为窗口层是
Win32 + WGL。

## 3. 生命周期与事件

| 时机 | 插件做什么 |
| --- | --- |
| `init(self, host)` | 订阅 `settings.open` / `config.applied` / `plugins.changed`；起 UI 线程，建**隐藏**窗口、WGL 上下文、ImGui 上下文；返回 `0` 时窗口与线程已就绪 |
| `settings.open` | 显示窗口 + 全量刷新模型（窗口已显示时只刷新，不重置位置） |
| `config.applied` | 以插件当前值为新基线重建模型（保留"被拒绝"的本地修改，见 §4） |
| `plugins.changed` | 整表重新枚举配置目录与注册表（节可能增删），把负载里的 `reason` 显示在对应行，结束"应用中…" |
| X / Esc | 只 `ShowWindow(SW_HIDE)`：线程、窗口、GL 上下文全部保留，下次 `settings.open` 直接显示 |
| `host.shutdown` / `deinit` | 先退订事件总线，再让消息循环退出并 `join` UI 线程，最后销毁窗口与 GL 上下文 |

窗口几何在每次隐藏（以及宿主退出时窗口仍开着）时写进**宿主配置**的
`settings_ui.window`（字符串 `"x,y,w,h"`），下次显示前恢复，并按显示器工作区做屏幕内
clamp——换分辨率、拔掉副显示器后窗口不会跑到屏幕外。

事件总线回调只做两件事：拷进队列 + `PostMessage` 唤醒 UI 线程。所有 ImGui 调用
都只在插件自己的 UI 线程上发生。

## 4. 表单与编辑模型

顶部固定操作条（`应用` / `取消` / `恢复全部默认` / 状态文本），下面是一页按提供者分
`CollapsingHeader` 的滚动内容，宿主 `core` 固定在最前，插件管理表紧随其后。

| `ShotConfigDesc::kind` | 控件 | 值格式 |
| --- | --- | --- |
| `BOOL` | Checkbox | `"1"` / `"0"` |
| `INT` / `FLOAT` | Slider + Input（用 `min/max/step`，越界 clamp，非法输入回退） | 十进制字符串 |
| `ENUM` | Combo（`options` 按 `\|` 切） | 选项字符串 |
| `STRING` | InputText | 原样 |
| `COLOR` | ColorEdit4 + hex 输入 | 统一存 `#RRGGBBAA`，读取兼容 `#RRGGBB` / `R,G,B,A` |
| `PATH` | InputText + `浏览…`（`GetOpenFileNameW`） | 原样 |
| 未知 kind | 灰色"不支持的配置类型"，不崩 | —— |

每项都有 `label` + 悬停 tooltip（含配置键）+ `恢复默认` 小按钮。编辑采用**本地副本 +
应用/取消**（`document/SETTINGS.md` §4）：

1. 打开时把值读进本地缓冲，拖滑块只改缓冲（不会每帧 `set_config` 打断插件）；
2. 点「应用」：脏配置项逐条 `set_value` → 变化的启停逐条 `set_enabled` → `save()` 落盘；
3. `set_value` 返回负值 → 该项标红"插件拒绝"，值留在本地供用户修改；
4. 有启停变更时进入"应用中…"，等 `plugins.changed` 到达后刷新并结束等待（5 秒超时兜底）；
5. 点「取消」丢弃全部未应用修改；「恢复全部默认」只把值改回 `default_value`，仍要点「应用」。

插件管理表列：启用复选框 / 插件名(id) / 版本 / 状态（已加载、已禁用、加载失败：原因）。
勾选只改本地 pending，应用时才 `set_enabled`；入口插件（`settings_ui.main` 自己）的
复选框置灰——禁用入口插件等于把当前设置页一起卸载，`document/SETTINGS.md` §3.1 明确
这一行由设置页负责置灰，宿主不加特判。

## 5. 为什么这么设计

| 决定 | 理由 |
| --- | --- |
| 窗口/线程/GL 上下文全在 `Impl` 里（PIMPL） | Win32 类型不可能顺着契约漏给宿主；宿主连 `<windows.h>` 都不需要包含 |
| 一个类一个头文件，`.cpp` 按关注点拆 | 头文件是类的完整契约（`STYLE.md` §6.7/§6.8）：模型 / 表单 / 插件表 / 提交各自一个文件，改渲染不用碰模型，改窗口不用碰设置页 |
| 事件回调只入队 + `PostMessage` | 总线回调跑在宿主分发线程上，而 ImGui 不是线程安全的；唤醒后统一在 UI 线程 drain |
| 本地编辑 + 应用/取消 | 避免每帧 `set_config`；批量中间态由 `config.applied` 兜底（`SETTINGS.md` §4） |
| 描述符驱动而不是硬编码配置项 | 插件新增配置项只改自己的 `kConfig[]`，设置页代码一行不动 |
| 优先 3.3 core，失败回退基础上下文 | core profile 在 ImGui 1.92 的 OpenGL3 后端上最省心；老驱动/受限环境还能退回兼容上下文 |
| GLSL 版本按 `glGetString(GL_VERSION)` 选 150 / 130 | 3.2+ 用 `#version 150`，3.0/3.1 用 `#version 130`；低于 3.0 让 `init` 干净失败并记 ERROR——`NewFrame` 里建 shader 失败是 `IM_ASSERT`，会断言掉 Debug 宿主 |
| 中文字体按 微软雅黑 → 黑体 → 宋体 → 西文 回退 | 只是显示效果降级；全部缺失时退回 ImGui 内置字体，功能不受影响 |
| DPI 用 `GetDpiForWindow` + `style.ScaleAllSizes` + `style.FontScaleMain` | ImGui 1.92 把 `io.FontGlobalScale` 移到了 `style.FontScaleMain`（旧字段大于 1 会触发断言）；**不**调 `ImGui_ImplWin32_EnableDpiAwareness`（那是改整个进程） |
| 窗口几何存宿主配置，`io.IniFilename = nullptr` | 不在插件目录/工作目录旁边生成 `imgui.ini`；几何属于用户界面状态，不占用任何插件配置节 |
| 几何恢复做工作区 clamp | 上次的位置可能来自另一块显示器；夹进当前工作区才不会"打开就看不见" |

## 6. 降级行为

宿主比插件老、插件被热卸载、驱动不支持……都必须是**安静降级**，不能让宿主崩：

| 情况 | 行为 |
| --- | --- |
| 宿主没有 `shot.host.plugin_config` | 显示"宿主未提供设置目录服务"提示，插件管理照常渲染 |
| 宿主没有 `shot.host.plugin_registry` | 显示"宿主未提供插件注册表服务"提示，配置表单照常渲染 |
| 宿主没有事件总线 | 记一条 WARN，窗口不会自动弹出（只能靠宿主 `post_to_plugin` 投递 `settings.open`） |
| 配置提供者被热卸载 | 收到 `plugins.changed` 后整表重读；消失的节显示成灰色"插件已卸载"一节，下一次刷新消失 |
| 某个配置项被插件拒绝 | 该项标红"插件拒绝：原因"，不中断其它项的提交 |
| 系统字体全部缺失 | 退回 ImGui 内置字体并记 WARN（中文显示为方块，功能正常） |
| OpenGL < 3.0 或 shader 创建失败 | `init` 返回失败并记 ERROR；宿主按"这个 UI 插件初始化失败"处理，其余功能不受影响 |
| 所有读取失败 | 不抛异常：`shot::PluginConfig` / `shot::PluginRegistry` 在服务缺失时返回空模型 |

## 7. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 多个设置窗口 / 多实例 | 单实例足够；多实例会出现"两处同时改同一项"的冲突 |
| 每帧写回插件（无「应用」按钮） | 拖滑块会打断插件，且无法批量提交与撤销 |
| begin/commit 事务 | v1 用 `config.applied` 兜底批量通知（`SETTINGS.md` §7） |
| 在设置页安装/删除插件 | 只管理**已安装**插件的启停，安装属于安装器层 |
| 改进程 DPI 感知 | `ImGui_ImplWin32_EnableDpiAwareness` 影响整个进程，插件只缩放自己的窗口与样式 |
| 生成 `imgui.ini` / `imgui_log.txt` | 窗口几何进宿主配置；日志走宿主日志服务 |
| 链接 spdlog | 插件只能通过 `host->log`（`shot::Logger`）写日志（`STYLE.md` §6.4） |

## 8. 检查清单

- [ ] 新增配置项只改插件自己的 `kConfig[]`，没有改设置页代码？
- [ ] 配置值仍然一律用字符串、没有跨边界传结构体/数字？
- [ ] 不认识的 kind / 读不到的提供者仍然是安静跳过，而不是报错崩溃？
- [ ] 事件总线回调里只做入队与唤醒，没有直接碰 ImGui？
- [ ] `deinit` / `host.shutdown` 返回前退订了事件总线、停了 UI 线程、销毁了窗口？
- [ ] 异常没有越过 `extern "C"` 边界（`create` / `init` / `post` 都有 catch 兜底）？
- [ ] Win32 / WGL 类型仍然只出现在内部头 `settings_window_impl.h` 与窗口相关 `.cpp` 里（公共头不含 `<windows.h>`）？
- [ ] `metadata.json` 的 `interfaces` 与 `query_interface` 的实际回答一致（只有 `shot.ui_provider`）？
