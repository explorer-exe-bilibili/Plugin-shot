# region_selector —— 区域选择覆盖层插件（`shot.ui_provider`）

在**鼠标所在显示器**上弹出一个无边框、置顶、**半透明**、整屏的覆盖层，让用户拖拽框出一块矩形；
确认后先隐藏覆盖层、等遮罩从屏幕上消失，再发布带 `region` 的 `capture.request`。

覆盖层是 **GLFW 透明帧缓冲窗口**（`GLFW_TRANSPARENT_FRAMEBUFFER`）：窗口先隐藏、
`glfwSetWindowSize/SetWindowPos` 铺满整块显示器、**隐藏状态下先渲染两帧**再 `glfwShowWindow`。
刻意**不用** `glfwSetWindowMonitor`：那条路径会触发窗口模式切换与 DWM 重新合成，用户看到
的是类似切全屏的黑屏闪烁。透明帧缓冲让桌面透出来，选区内部是真正的"挖空"，所见即所截。
宿主流水线收到后按区域裁剪并落盘（坐标语义见 §4）。取消（Esc / 右键 / 选区过小）
不发布任何事件。

插件只有一个能力 `shot.ui_provider`：窗口、线程、GLFW 事件循环全部由插件自己管理，
宿主只调 `init / deinit / post_to_plugin`，从不接触窗口句柄。触发与结果都走事件总线
（主题与负载见 `document/CAPTURE.md`、`document/EVENT_BUS.md`）。

**仅 Windows**：覆盖层几何、光标位置与"物理虚拟桌面像素"依赖 Win32 坐标语义
（`GetCursorPos` / `GetSystemMetrics(SM_*VIRTUALSCREEN)`），非 Windows 平台本工程直接
跳过（`SHOT_PLUGIN_SKIPPED`），不产出伪实现。

## 1. 目录

```
plugins/region_selector/
├── CMakeLists.txt                  # shot_add_plugin + imgui / nlohmann-json / glfw3
├── metadata.json                   # 插件清单（id region_selector.main，接口 shot.ui_provider）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── region_window.h             # RegionWindow 公共接口（契约层，不含任何图形库类型）
│   └── region_window_impl.h        # 内部头：Impl 声明 / 事件队列 / 拖拽状态
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + kEntries + ABI 胶水
    ├── region_window.cpp           # 生命周期 / 窗口线程 / 事件循环 / 触发解析 / 隐藏后发布
    ├── region_overlay.cpp          # 框选交互状态机 + ImGui 遮罩/十字线/选区绘制
    ├── region_graphics.cpp         # ImGui 后端 / 中文字体 / DPI 字号缩放 / 每帧渲染
    └── region_geometry.cpp         # 显示器矩形 / 光标 / 物理虚拟桌面坐标换算（Win32 层）
```

公共头 `region_window.h` 只声明一个类；内部头 `region_window_impl.h` 是**内部头**
（只被本插件的 `.cpp` 包含，开头注明"不是插件对外接口"）。`.cpp` 按关注点拆成
"生命周期 / 交互绘制 / 图形 / 平台几何 / ABI 胶水"五个文件（`STYLE.md` §6.7/§6.8）。

## 2. 构建

宿主仓库内：由根 `CMakeLists.txt` 的 `plugins/*/CMakeLists.txt` glob 自动纳入，
跟着整体构建一起产出；也可以只构建本插件：

```bash
cmake -S . -B build/Windows -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows --target region_selector
```

离线 SDK 独立构建：

```bash
cmake -S plugins/region_selector -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

依赖与 `settings_ui` 同一套，vcpkg 清单里已启用对应特性：

| 依赖 | 用途 |
| --- | --- |
| `imgui`（glfw-binding + opengl3-binding） | 覆盖层绘制；后端源码已编进 `imgui.lib`，不抄进本工程 |
| `glfw3` | 窗口、事件循环、显示器几何、光标 |
| `nlohmann-json` | 触发负载解析 + 区域结果组包（header-only） |
| `opengl32` / `user32` | ImGui OpenGL3 后端 / `GetCursorPos`、`GetSystemMetrics` |

产物：`<构建树>/bin/plugins/region_selector.dll`；`metadata.json` 由 `shot_add_plugin()`
在链接后贴到 DLL 尾部。插件不链接 spdlog：日志统一走 `shot::Logger`（宿主 `log` 服务）。

## 3. 能力与调用时序

本插件实现一个能力：`shot.ui_provider`（`ShotUIProviderV1`，版本 1）。

| 回调 | 行为 |
| --- | --- |
| `init(self, host)` | 起 UI 线程、订阅 `capture.request`；GLFW/窗口/ImGui 延迟到第一次框选才创建（见 §3.1）。返回 0 时线程已就绪 |
| `deinit(self)` | 退订事件总线 → 通知事件循环退出 → join → 销毁窗口/ImGui/光标。进行中的框选按取消处理，不发布事件。可重复调用 |
| `post_to_plugin(self, ev)` | 只做"拷进队列 + 唤醒 UI 线程"；`host.shutdown` 让线程退出，`capture.request` 与总线事件同样处理 |

事件回调（总线分发线程）里**只入队 + 唤醒 UI 线程**，不做解析、不开窗口；所有 GLFW /
ImGui 调用都在插件的 UI 线程上执行（GLFW 和 ImGui 都不是线程安全的）。隐藏态等待用
`std::condition_variable`，框选态用 `glfwWaitEventsTimeout` 超时轮询（原因见 §3.1）。

一次框选的完整时序：

| # | 发生在 | 做什么 |
| --- | --- | --- |
| 1 | 任意线程 | 发布 `capture.request`，负载 `{"mode":"region"}`（**没有** `region` 对象） |
| 2 | 总线分发线程 | 插件回调只把事件入队并唤醒 UI 线程 |
| 3 | UI 线程 | 取鼠标位置 → 找所在显示器 → 把隐藏窗口搬过去铺满整屏 → 显示并聚焦 |
| 4 | UI 线程 | 拖拽：左键按下记起点，移动更新光标，**松开左键 = 确认**；右键 / Esc = 取消 |
| 5 | UI 线程 | 确认后先 `glfwHideWindow` + `glfwPollEvents`，再 `sleep 100ms`（`kHideSettleDelayMs`） |
| 6 | UI 线程 | 发布 `capture.request`：`{"mode":"region","region":{"x","y","width","height"}}` |
| 7 | 宿主 worker | 抓整屏 → 按 `region` 裁剪 → 处理 → 保存，发 `capture.done` |

**覆盖层整屏全黑、只剩十字线**：窗口没声明透明帧缓冲时，OpenGL 的默认清屏是不透明黑，
遮罩画在黑底上就是整屏黑。必须 `GLFW_TRANSPARENT_FRAMEBUFFER` + `glClearColor(..., 0)`，
并且把遮罩画成"选区外四块"，选区内部留空（透明）。

**为什么第 5 步要先隐藏再等 100ms**：DWM 合成是异步的，`glfwHideWindow` 返回时覆盖层
可能还没从屏幕上消失；宿主 worker 收到事件就抓屏，中间没有任何同步点。宁可慢 100ms，
也不能把半透明遮罩截进图里。

### 3.1 为什么 GLFW / 窗口延迟到第一次框选才创建

GLFW 在 Windows 上只有**一个全局 helper window**，`glfwPostEmptyEvent()` 实际是向它
`PostMessage(WM_NULL)`：消息只会派发给**创建 helper window 的线程**，而 helper window
属于进程里第一个调用 `glfwInit` 的线程。

本插件目录名 `region_selector` 的加载顺序在 `settings_ui` 之前。如果 `init` 里就
`glfwInit`，helper window 会归本插件的 UI 线程；而 `settings_ui.stop()` 依赖
"`glfwPostEmptyEvent` 唤醒自己的窗口线程"，它就会**永远等不到唤醒，宿主卡在
settings_ui 的 deinit**（实测：本插件 deinit 14ms 返回，宿主 30s+ 退不掉，日志停在
本插件的"收到 host.shutdown"之后）。所以本插件：

- 启动阶段只起线程、订阅事件，不碰 GLFW；第一次框选才 `ensureGraphics()`——那时
  `settings_ui` 已经初始化过 GLFW，helper window 归它的线程；
- 覆盖层隐藏时用条件变量唤醒 UI 线程，不依赖 `glfwPostEmptyEvent`；框选态本来就按
  120Hz 超时轮询，也不依赖它。

**自触发保护**：第 6 步发布的负载同样带 `mode=region`，也会回到本插件的订阅里。所以
只有"`mode=region` **且没有可用 `region` 对象**"的负载才是"请开始框选"；带
`x/y/width/height`（整数且宽高 > 0）的请求一律忽略，否则会"框选一次 → 又弹一次"无限循环。

**取消**：Esc、右键、任一边小于 8 物理像素（`kMinimumSelectionSize`，防误点）都只隐藏
覆盖层，**不发布任何事件**——v1 不定义 `capture.cancelled`。

**限一屏**：覆盖层铺满"鼠标所在显示器"，选区被夹在这块屏内；跨显示器拖拽不生效
（见 §7）。

## 4. 坐标语义与 DPI 实测

发布出去的区域是**物理虚拟桌面像素**，原点 = 虚拟桌面左上角——与
`shot.screen_capture` 的 `describe` 报的 `origin_x/origin_y` + `width/height` 同一坐标系
（宿主按这个坐标从整屏图里裁剪）。

换算公式（代码在 `region_geometry.cpp` / `region_window.cpp`）：

```
region.x = 覆盖窗口屏幕位置.x + 窗口内光标坐标.x - 虚拟桌面原点.x
region.y = 覆盖窗口屏幕位置.y + 窗口内光标坐标.y - 虚拟桌面原点.y
region.width / height = 选区在窗口内的宽高
虚拟桌面原点 = 所有显示器矩形的最小 x / 最小 y（= GetSystemMetrics(SM_XVIRTUALSCREEN/Y)）
```

### 4.1 实测环境（本机，混合 DPI）

用临时 GLFW 探针在这台机器上逐项打过表（`glfwGetMonitorPos` / `glfwGetMonitorWorkarea` /
`glfwGetVideoMode` / `glfwGetWindowContentScale` / `glfwGetFramebufferSize` / `glfwGetCursorPos`
+ Win32 `GetCursorPos` / `GetSystemMetrics`）。宿主 `src/main.cpp` 在**任何窗口/采集之前**
就把进程声明成 `PER_MONITOR_AWARE_V2`，探针按同样顺序声明后测得：

| 项 | 主显示器 | 副显示器 |
| --- | --- | --- |
| 名称 | CSW PnP Monitor（primary） | Generic Non-PnP Monitor |
| `glfwGetMonitorPos` | `(0, 0)` | `(2560, 0)` |
| `glfwGetMonitorWorkarea` | `(0, 0) 2560x1528` | `(2560, 0) 1024x720` |
| `glfwGetVideoMode` | `2560x1600 @180Hz` | `1024x768 @60Hz` |
| `glfwGetMonitorContentScale` | `1.5 x 1.5`（150%） | `1.0 x 1.0`（100%） |
| `GetDpiForWindow`（整屏窗口） | `144` | `96` |
| 窗口 pos / size | `(0,0)` / `2560x1600` | `(2560,0)` / `1024x768` |
| framebuffer size | `2560x1600` | `1024x768` |

DPI 声明前后的进程级数值（解释"为什么必须 PMv2"）：

| 时刻 | 线程 DPI 感知 | `GetDpiForSystem` | `SM_*VIRTUALSCREEN` |
| --- | --- | --- | --- |
| 声明前（未感知） | `UNAWARE` | `96` | `origin (0,0) size 3584x1067`（逻辑像素） |
| 声明 PMv2 后 | `PER_MONITOR_AWARE`（V2 上下文） | `144` | `origin (0,0) size 3584x1600`（物理像素） |

### 4.2 结论（代码里按这个写）

1. **PMv2 进程里 GLFW 的屏幕坐标 == Win32 物理像素**。在 150% 屏上整屏窗口是
   `2560x1600` 物理像素，不是 `1707x1067`；`glfwGetCursorPos(窗口) + glfwGetWindowPos`
   与 `GetCursorPos` 逐点相等（两块屏上各测了 3 个点，误差 0）。
2. **content scale 只用于字号，绝不用于坐标**。`content scale = 1.5` 表示 DPI 是 150%，
   坐标已经是物理像素；乘 1.5 会得到 1.5 倍的错误区域。代码里只把它传给
   `applyMonitorScale()` 放大 ImGui 字体。
3. **窗口 size == framebuffer size**（两块屏实测都相等），所以窗口内容坐标、ImGui 绘制
   坐标、物理像素三者一致；绘制里仍保留 `framebuffer/window` 的比例换算，只是不把
   "两者相等"写死成假设。
4. **region 要减虚拟桌面原点**。本机两块屏都在 `(0,0)` 的右/下方，减法结果为 0；但左侧/
   上方的显示器会让 `SM_XVIRTUALSCREEN/Y` 变成负数，代码统一按"绝对坐标 − 最小显示器
   坐标"计算，不依赖本机布局（负原点布局本机无法验证，见 §7）。

## 5. 交互与视觉

| 输入 | 行为 |
| --- | --- |
| 左键按下并拖动 | 画选区（选区内部**透明挖空**、四周压暗 + 亮蓝色 2px 边框 + 实时尺寸标签） |
| 松开左键 | 确认；任一边 < 8 物理像素按取消处理 |
| 右键 | 取消 |
| Esc | 取消 |
| 鼠标移动 | 全屏十字线 + 选区尺寸标签（`宽 x 高 px`，物理像素） |

视觉：整屏 51% 黑色遮罩 + 十字线 + 选区高亮/边框 + 顶部提示
「拖拽选择区域（物理像素） · Esc / 右键取消」。没有截屏背景可"挖空"时，用"选区内部提亮 +
亮色边框"表达选中区域（覆盖层是不透明窗口，不能真的透出桌面）。

覆盖层显示时鼠标换成十字光标；DPI 变化只影响字号（100% 屏 18px、150% 屏 27px 物理像素），
坐标不受影响。

## 6. 降级与边界情况

| 情况 | 行为 |
| --- | --- |
| 宿主没有 `log` 服务（老宿主） | `shot::Logger` 静默丢弃日志，覆盖层照常工作 |
| 宿主没有事件总线（旧版宿主） | `init` 照常成功；只记一条 WARN，覆盖层改由 `post_to_plugin` 触发 |
| `capture.request` 负载不是合法 JSON / 不是对象 | 安静忽略（共享总线上收到坏负载不是错误） |
| `capture.request` 的 `mode` 不是 `"region"`（含空负载 = 整屏采集） | 安静忽略，不弹覆盖层 |
| `capture.request` 已带合法 `region`（本插件自己发布的那条 / 别处选好区域的请求） | 安静忽略，避免自触发循环 |
| 光标不在任何显示器矩形里（显示器缝隙 / 远程会话刚断开） | 退回主显示器并记 WARN |
| 框选过程中再次收到触发 | 忽略重复触发，不覆盖用户正在拖的选区 |
| 框选过程中收到 `host.shutdown` / 宿主热卸载 | 事件循环退出，覆盖层按取消处理，不发布事件 |
| 显示器几何在框选期间变化（拔屏/改分辨率） | 本次框选仍按开始时的窗口坐标结束；下次触发重新取显示器 |
| 第一次框选时 GLFW/窗口/ImGui 初始化失败 | 放弃本次框选并记 ERROR，不发布事件；`init` 本身不会因此失败（它不碰图形栈） |
| OpenGL 版本 < 3.0 / 驱动不支持 shader | `ensureGraphics()` 干净失败，同上一行 |

## 7. 已知限制

| 限制 | 说明 |
| --- | --- |
| 只支持单显示器框选 | 覆盖层铺满鼠标所在显示器，选区被夹在该屏内；跨屏选择需要在虚拟桌面矩形上开一个覆盖所有显示器的窗口（代码里有 `// TODO:` 说明） |
| 负虚拟桌面原点未实测 | 本机虚拟桌面原点 `(0,0)`，左侧/上方有显示器时的负 origin 布局无法在这台机器上验证；换算公式按"绝对坐标 − 最小显示器坐标"实现，理论上正确 |
| 触发请求也会被流水线看到 | `{"mode":"region"}`（无 region）是"开始框选"约定；宿主流水线会**忽略**这条请求（不抓屏、不落盘、不发 `capture.*` 事件），只有本插件消费它来弹覆盖层。"框选触发顺带存一张全屏图"是旧行为，已由宿主改掉 |
| 与其它插件共用 GLFW 全局运行时 | 见 §3.1：本插件延迟初始化、**不调用 `glfwTerminate`**。若 `settings_ui` 启动时被禁用、本插件先初始化过 GLFW 后它才被启用，它的退出唤醒可能失效（GLFW 单 helper window 的固有限制） |
| 不提供 `shot.configurable` | 目前没有需要用户配置的项：最小选区、遮罩透明度、颜色都是实现细节。要加配置项时再声明 `shot.configurable` |

## 8. 检查清单

- [ ] `region` 的 x/y/width/height 都是**物理虚拟桌面像素**，且减过虚拟桌面原点？
- [ ] content scale 只用于字号、没有乘进坐标？
- [ ] 确认时序是"隐藏 → 等 100ms → 发布"，而不是隐藏后立即发布？
- [ ] 取消（Esc / 右键 / 过小选区）真的没有发布任何事件？
- [ ] 带 `region` 的 `capture.request` 不会再次触发框选（防自触发循环）？
- [ ] 事件总线回调里只有入队 + 唤醒，没有 GLFW/ImGui 调用？
- [ ] `deinit` 返回前已退订事件总线、join 线程、销毁窗口？
- [ ] 事件 `deinit` 后不会再有回调落到本插件（先退订再停线程）？
- [ ] 所有 GLFW / Win32 / ImGui 类型都关在 Impl 与 `.cpp` 里，公共头不含平台头？
- [ ] `metadata.json` 的 `interfaces` 与 `kIfaces[]` 一致（只有 `shot.ui_provider`）？
- [ ] 非 Windows 平台是"CMake 跳过"，没有未验证的实现分支？
