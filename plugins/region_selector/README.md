# region_selector —— 区域选择覆盖层插件（`shot.ui_provider`）

在**鼠标所在显示器**上弹出一个无边框、置顶、**半透明**、整屏的覆盖层。覆盖层显示后：
鼠标停在某个顶层窗口上时，选框会**吸附到该窗口**并带 100ms 指数平滑地平移/缩放过去；
**单击**（位移 ≤4px、按住 ≤400ms）直接把当前悬停窗口的矩形作为采集区域；**拖动**则退化成
原来的手绘选区（拖动期间严格跟手，不做吸附和动画）。确认后先隐藏覆盖层、等遮罩从屏幕上
消失，再发布带 `region` 的 `capture.request`。

覆盖层是**不透明窗口 + 触发瞬间的桌面快照**（`region_background.cpp`）：先 GDI 截一张该
显示器的快照、上传成 GL 纹理，窗口用这张纹理铺满当背景，遮罩/挖空/十字线/主题色折角都画在
它上面。窗口先隐藏、`glfwSetWindowSize/SetWindowPos` 覆盖整块显示器、**隐藏状态下先渲染两帧**
再 `glfwShowWindow`；刻意**不用** `glfwSetWindowMonitor`（那条路径会切窗口模式，表现为黑屏闪烁）。
窗口尺寸是**显示器尺寸 + 1 像素**（`kOverlayOversize`）：刚好等于显示器尺寸时 Windows 会把它
当成"全屏窗口"走 fullscreen optimization，导致每次显示/隐藏闪一下、进程退出后甚至留下黑色
残影（真机踩过，详见 §6）。

> 为什么不用透明窗口：早期用 `GLFW_TRANSPARENT_FRAMEBUFFER` 让桌面透出来，第一次能显示，
> 但窗口 `hide → show` 一轮之后 DWM 的 blur-behind 合成失效，覆盖层变回不透明黑屏且不再
> 恢复（真机复现：第一次半透明 → 点一下再触发就全黑 + 只剩两条十字线）。快照方案不依赖
> 任何窗口合成特性，连续触发/取消任意多次都稳定；代价是覆盖层里看到的是"按下触发那一刻"
> 的桌面，不是实时的（框选期间桌面本来也不该动）。
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
├── CMakeLists.txt                  # shot_add_plugin + imgui / nlohmann-json / glfw3 / dwmapi
├── metadata.json                   # 插件清单（id region_selector.main，接口 shot.ui_provider）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── region_window.h             # RegionWindow 公共接口（契约层，不含任何图形库类型）
│   ├── region_window_impl.h        # 内部头：Impl 声明 / 事件队列 / 拖拽与悬停吸附状态
│   └── window_target.h             # 内部头：窗口目标结构 + 枚举/命中接口（纯 Win32 查询）
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + kEntries + ABI 胶水 + 语言表
    ├── region_window.cpp           # 生命周期 / 窗口线程 / 事件循环 / 触发解析 / 隐藏后发布
    ├── region_overlay.cpp          # 交互状态机 + 悬停吸附动画 + 主题色读取
    ├── region_overlay_draw.cpp     # ImGui 遮罩 / 选区 / 折角 / 十字线 / 标签绘制
    ├── region_graphics.cpp         # ImGui 后端 / 中文字体 / DPI 字号缩放 / 每帧渲染
    ├── region_background.cpp       # 触发瞬间的桌面快照（GDI）+ 上传 GL 纹理
    ├── region_geometry.cpp         # 显示器矩形 / 光标 / 覆盖层 HWND / 物理坐标换算（Win32 层）
    └── window_target.cpp           # 顶层窗口枚举 / Z 序 / DWM 边界 / 命中测试（纯 Win32 查询）
```

公共头 `region_window.h` 只声明一个类；内部头 `region_window_impl.h` 与 `window_target.h`
是**内部头**（只被本插件的 `.cpp` 包含，开头注明"不是插件对外接口"）。`.cpp` 按关注点拆成
"生命周期 / 交互 / 绘制 / 图形 / 平台几何 / 窗口枚举 / ABI 胶水"七个文件（`STYLE.md` §6.7/§6.8）。

窗口枚举独立成 `window_target.cpp`：它只回答"光标下有没有窗口、矩形是多少"，不碰 ImGui
和覆盖层状态；覆盖层交互文件负责把结果变成吸附/动画/采集。两者分开后，窗口筛选规则
（DWM cloaked、`WS_EX_TOOLWINDOW`、扩展帧边界等）可以单独审查，不会淹没在绘制代码里。

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
| `nlohmann-json` | 触发负载解析 + 区域结果组包 + 多语言表解析（header-only） |
| `opengl32` / `user32` / `dwmapi` | ImGui OpenGL3 后端 / `GetCursorPos`、`EnumWindows` / 窗口扩展帧边界与 cloaked 检测 |

产物：`<构建树>/bin/plugins/region_selector.dll`；`metadata.json` 由 `shot_add_plugin()`
在链接后贴到 DLL 尾部。插件不链接 spdlog：日志统一走 `shot::Logger`（宿主 `log` 服务）。

## 3. 能力与调用时序

本插件实现一个能力：`shot.ui_provider`（`ShotUIProviderV1`，版本 1）。

| 回调 | 行为 |
| --- | --- |
| `init(self, host)` | 起 UI 线程、订阅 `capture.request`、构造并加载语言表（见 §3.2）；GLFW/窗口/ImGui 延迟到第一次框选才创建（见 §3.1）。返回 0 时线程已就绪 |
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
| 3 | UI 线程 | 取鼠标位置 → 找所在显示器 → 把隐藏窗口搬过去铺满整屏；读主题色、记录覆盖层 HWND、按 Z 序枚举顶层窗口命中光标下的窗口 → 显示并聚焦 |
| 4 | UI 线程 | 空闲：每 30ms 枚举一次顶层窗口，命中窗口后选框用 100ms 指数平滑吸附过去；鼠标移到别的窗口就平滑平移/缩放 |
| 5 | UI 线程 | 单击（位移 ≤4px 且按住 ≤400ms）：直接把当前悬停窗口矩形当 region 确认；拖动（位移 >4px）：选框严格跟手，松开左键确认；右键 / Esc = 取消 |
| 6 | UI 线程 | 确认后先 `glfwHideWindow` + `glfwPollEvents`，再 `sleep 100ms`（`kHideSettleDelayMs`） |
| 7 | UI 线程 | 发布 `capture.request`：`{"mode":"region","region":{"x","y","width","height"}}` |
| 8 | 宿主 worker | 抓整屏 → 按 `region` 裁剪 → 处理 → 保存，发 `capture.done` |

**覆盖层整屏全黑、只剩十字线**：说明背景快照没画出来——要么 `captureBackground()` 失败
（日志有"桌面快照失败"WARN，`GetDC/BitBlt/GetDIBits` 任一步失败），要么纹理上传后没在
`drawOverlay` 里 `AddImage`（检查 `backgroundTexture != 0`）。透明窗口方案已被证实会在
hide/show 后失效，不要再改回 `GLFW_TRANSPARENT_FRAMEBUFFER`。

**为什么第 6 步要先隐藏再等 100ms**：DWM 合成是异步的，`glfwHideWindow` 返回时覆盖层
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

**自触发保护**：第 7 步发布的负载同样带 `mode=region`，也会回到本插件的订阅里。所以
只有"`mode=region` **且没有可用 `region` 对象**"的负载才是"请开始框选"；带
`x/y/width/height`（整数且宽高 > 0）的请求一律忽略，否则会"框选一次 → 又弹一次"无限循环。

**取消**：Esc、右键、拖动时任一边小于 8 物理像素（`kMinimumSelectionSize`，防误点）都只
隐藏覆盖层，**不发布任何事件**——v1 不定义 `capture.cancelled`。单击窗口不受 8px 限制：
矩形是系统给出的真实窗口，再小也是用户明确点中的目标。

**限一屏**：覆盖层铺满"鼠标所在显示器"，选区被夹在这块屏内；跨显示器拖拽不生效
（见 §7）。

### 3.2 多语言（i18n）

覆盖层的用户可见文案跟随宿主当前界面语言；**日志固定中文，不进语言表**（`tools/verify/`
的 e2e 按中文日志断言，翻译日志会让验证失败）。语言表写在 `src/plugin_entry.cpp` 的
文件作用域 `kLangTables[]` 里，每种语言一张 `R"json(...)json"` 表，`@meta` 是 BCP-47
语言标签：

| key | zh-CN | en-US |
| --- | --- | --- |
| `region.hint` | 单击截取窗口 · 拖拽选择区域 · Esc / 右键取消 | Click a window to capture · Drag to select · Esc / right-click to cancel |
| `overlay.size.unit` | 像素 | px |
| `overlay.window_title` | Plugin-shot 区域选择 | Plugin-shot Region Select |

- `ui_init(host)` 里 `std::make_unique<shot::I18n>(host, "region_selector.main")` 并
  `loadTables()`。语言表对象由插件状态持有；`start(host, i18n)` 只把只读指针交给
  覆盖层线程，`deinit` / `destroy` 先 `stop()`（join 线程）再 `reset()`，避免渲染帧
  读到已释放的对象。
- 渲染帧里直接 `i18n->tr(key, fallback)`：`tr()` 查的是插件本地表（O(1)、不查询宿主），
  所以不需要缓存指针。`fallback` 必须是中文原文，老宿主 / 表加载失败时行为与改造前一致。
- 新增文案时两张表同时加 key、两种语言都写全；只写一种语言会落到回退链（可能显示
  另一种语言）。改语言由宿主的 `i18n_list` 驱动，插件不需要自己重载。
- 老宿主没有列表服务时 `shot::I18n` 安静降级，仍用第一张表（zh-CN）显示，`init` 不会
  因此失败；日志与窗口行为完全不变。

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
| 鼠标移动（未拖动） | 每 30ms 枚举一次顶层窗口，命中光标下的窗口后把选框**平滑吸附**到它的矩形（100ms 指数平滑）；命中不到窗口时只显示遮罩与十字线 |
| 单击（位移 ≤4px 且按住 ≤400ms） | 直接用当前悬停窗口的矩形确认采集，不经过拖动 |
| 左键按下并拖动（位移 >4px） | 进入拖动模式：选框**严格跟手**，不做窗口吸附、不做动画；松开确认，任一边 < 8 物理像素按取消处理 |
| 松开左键（未越过阈值但按住 >400ms） | 按取消处理（不发布事件） |
| 右键 / Esc | 取消 |
| 鼠标移动 | 全屏十字线 + 选区尺寸标签（`宽 x 高` + 单位，物理像素） |

视觉：整屏 51% 黑色遮罩 + 十字线 + 选区边框 + **四角主题色折角** + 顶部提示
（`region.hint`，中文为「单击截取窗口 · 拖拽选择区域 · Esc / 右键取消」，英文为
"Click a window to capture · Drag to select · Esc / right-click to cancel"）。背景是触发瞬间
的桌面快照，选区内部保持快照原样（等价于"挖空"），框选结果与快照内容一致。

- **主题色**：边框与四角折角都用宿主配置项 `core/theme_color`（形如 `#4C9AFF`），通过
  `shot.host.config` 读取；每次显示覆盖层读一次，配置变化不实时刷新。读不到或格式非法时
  回退默认 `#4C9AFF`（见 `region_overlay.cpp` 的 `parseThemeColor`）。
- **折角**：四个角各画 L 形两段，长度 = `clamp(min(宽,高) * 0.2, 12, 40)` 逻辑像素，
  线宽 2 逻辑像素；两者都乘显示器 content scale 变成物理像素（150% 屏上是 18~60 / 3）。
  极小的拖动选框会把折角长度夹到半边长以内，避免四个 L 叠在一起。
- **边框/折角主题色**：选区边框同样用主题色，粗细与折角一致（2 逻辑像素）。

窗口检测算法（`window_target.cpp`，**刻意不用 Win32 的逐点窗口命中 API**）：

1. `EnumWindows` 枚举顶层窗口（不遍历子控件；本版本明确不做 UIA / 子控件命中）；
2. 排除：覆盖层自身 HWND、不可见、最小化、零尺寸、DWM cloaked（`DWMWA_CLOAKED`）、
   `WS_EX_TOOLWINDOW`（工具窗口/提示/输入法候选）；
3. 矩形优先 `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)`（去掉 Win10/11 不可见
   边框），失败退回 `GetWindowRect`；再裁剪到虚拟桌面内；
4. `EnumWindows` 的顺序不保证是 Z 序，代码用 `GetTopWindow + GW_HWNDNEXT` 建立真实的
   Z 序排名后稳定排序，命中测试取第一个包含光标的窗口；
5. 枚举节流：最多每 30ms 一次（`kWindowScanIntervalMs`），120Hz 渲染循环里不会每帧枚举。

覆盖层显示时鼠标换成十字光标；DPI 变化只影响字号与折角/边框尺寸（100% 屏 18px 字号、
150% 屏 27px 物理像素），坐标不受影响。

## 6. 降级与边界情况

**覆盖层显示/隐藏闪一下、退出后屏幕残留黑块**：这是 Windows 把"矩形恰好等于显示器的窗口"
判定为全屏窗口、走 fullscreen optimization（direct flip）造成的。修法是**窗口比显示器大
1 像素**（`kOverlayOversize = 1`），不满足全屏判定后走普通合成，闪屏与残影都消失；多出的
1px 在屏幕外，用户看不到。另外不要显式 `glfwFocusWindow`（hint 里 `GLFW_FOCUS_ON_SHOW`
已经给焦点，再聚焦一次会多闪一下），也不要为了"不闪"改成窗口永不移除（hide/show 必须保留，
否则 DWM 可能留下残影）。

| 情况 | 行为 |
| --- | --- |
| 宿主没有 `log` 服务（老宿主） | `shot::Logger` 静默丢弃日志，覆盖层照常工作 |
| 宿主没有事件总线（旧版宿主） | `init` 照常成功；只记一条 WARN，覆盖层改由 `post_to_plugin` 触发 |
| `capture.request` 负载不是合法 JSON / 不是对象 | 安静忽略（共享总线上收到坏负载不是错误） |
| `capture.request` 的 `mode` 不是 `"region"`（含空负载 = 整屏采集） | 安静忽略，不弹覆盖层 |
| `capture.request` 已带合法 `region`（本插件自己发布的那条 / 别处选好区域的请求） | 安静忽略，避免自触发循环 |
| 光标不在任何显示器矩形里（显示器缝隙 / 远程会话刚断开） | 退回主显示器并记 WARN |
| 框选过程中再次收到触发 | 忽略重复触发，不覆盖用户正在拖的选区 |
| 光标下没有候选窗口（桌面空白 / 枚举到的最小化或 cloaked 窗口） | 不显示悬停框；单击只取消，不发布事件 |
| 光标移出覆盖层所在显示器 | 悬停框隐藏（不在另一块屏上"盲吸附"）；覆盖层本身仍只覆盖原显示器 |
| 悬停目标在枚举间隔内被关闭 / 移到别处 | 下一次枚举（≤30ms）后自动换成新目标；选框平滑过去 |
| 按下后位移 ≤4px、按住 >400ms 才松开 | 不算单击也不算拖动，按取消处理（与旧版的 0x0 选区一致） |
| 单击窗口时窗口宽度/高度小于 8 物理像素 | 仍然采集：窗口矩形由系统给出，不受拖动最小选区限制 |
| `core/theme_color` 缺失 / 不是 `#RRGGBB` | 使用默认 `#4C9AFF`；只在 debug 日志里记一句，不反复刷屏 |
| `DwmGetWindowAttribute` 失败（合成关闭等） | 扩展帧边界退回 `GetWindowRect`；cloaked 检测失败按"未 cloaked"处理 |
| `EnumWindows` 枚举不到任何窗口 | 悬停框为空，拖动框选仍然可用（拖动不依赖窗口枚举） |
| 框选过程中收到 `host.shutdown` / 宿主热卸载 | 事件循环退出，覆盖层按取消处理，不发布事件 |
| 显示器几何在框选期间变化（拔屏/改分辨率） | 本次框选仍按开始时的窗口坐标结束；下次触发重新取显示器 |
| 第一次框选时 GLFW/窗口/ImGui 初始化失败 | 放弃本次框选并记 ERROR，不发布事件；`init` 本身不会因此失败（它不碰图形栈） |
| OpenGL 版本 < 3.0 / 驱动不支持 shader | `ensureGraphics()` 干净失败，同上一行 |

## 7. 已知限制

| 限制 | 说明 |
| --- | --- |
| 只支持单显示器框选 | 覆盖层铺满鼠标所在显示器，拖动选区被夹在该屏内；跨屏选择需要在虚拟桌面矩形上开一个覆盖所有显示器的窗口（代码里有 `// TODO:` 说明）。悬停窗口可以横跨多屏：矩形会裁剪到虚拟桌面，绘制只显示落在覆盖层里的部分 |
| 窗口枚举依赖 Z 序假设 | Z 序用 `GetTopWindow + GW_HWNDNEXT` 显式排序，但仍有一些特殊窗口可能不符合用户直觉：置顶工具窗口（任务栏、自绘浮层）会按 Z 序优先命中；完全透明的点击穿透窗口（`WS_EX_TRANSPARENT` + layered）没有被额外过滤 |
| 不做子控件检测 | 用户明确取消：不实现 UIA / 子控件命中，命中单位是顶层窗口（浏览器内部分栏、IDE 内部编辑器都算整个窗口） |
| 主题色只在显示覆盖层时读取 | 覆盖层显示期间改设置不会实时生效；下次弹出覆盖层才用新颜色（需求约定） |
| 负虚拟桌面原点未实测 | 本机虚拟桌面原点 `(0,0)`，左侧/上方有显示器时的负 origin 布局无法在这台机器上验证；换算公式按"绝对坐标 − 最小显示器坐标"实现，理论上正确 |
| 触发请求也会被流水线看到 | `{"mode":"region"}`（无 region）是"开始框选"约定；宿主流水线会**忽略**这条请求（不抓屏、不落盘、不发 `capture.*` 事件），只有本插件消费它来弹覆盖层。"框选触发顺带存一张全屏图"是旧行为，已由宿主改掉 |
| 与其它插件共用 GLFW 全局运行时 | 见 §3.1：本插件延迟初始化、**不调用 `glfwTerminate`**。若 `settings_ui` 启动时被禁用、本插件先初始化过 GLFW 后它才被启用，它的退出唤醒可能失效（GLFW 单 helper window 的固有限制） |
| 不提供 `shot.configurable` | 本插件没有自己的配置项：最小选区、遮罩透明度是内部常量；主题色来自**宿主**配置 `core/theme_color`（不是插件配置项）。要加插件自己的配置时再声明 `shot.configurable` |

## 8. 检查清单

- [ ] `region` 的 x/y/width/height 都是**物理虚拟桌面像素**，且减过虚拟桌面原点？
- [ ] content scale 只用于字号/折角/边框，没有乘进区域坐标？
- [ ] 窗口枚举排除了覆盖层自己的 HWND（否则永远命中自己）？
- [ ] 窗口矩形优先用 `DWMWA_EXTENDED_FRAME_BOUNDS`，失败才退回 `GetWindowRect`？
- [ ] 拖动时真的没有走窗口枚举/动画（选框严格跟手）？
- [ ] 单击阈值是位移 ≤4px 且按住 ≤400ms，用的是悬停**目标**矩形而不是动画中间值？
- [ ] 每次显示覆盖层只读一次 `core/theme_color`，非法值回退 `#4C9AFF`？
- [ ] 枚举有 30ms 节流，没有在 120Hz 渲染循环里每帧 `EnumWindows`？
- [ ] 确认时序是"隐藏 → 等 100ms → 发布"，而不是隐藏后立即发布？
- [ ] 取消（Esc / 右键 / 过小选区 / 无窗口时单击）真的没有发布任何事件？
- [ ] 带 `region` 的 `capture.request` 不会再次触发框选（防自触发循环）？
- [ ] 事件总线回调里只有入队 + 唤醒，没有 GLFW/ImGui 调用？
- [ ] `deinit` 返回前已退订事件总线、join 线程、销毁窗口？
- [ ] 事件 `deinit` 后不会再有回调落到本插件（先退订再停线程）？
- [ ] 所有 GLFW / Win32 / ImGui 类型都关在 Impl 与 `.cpp` 里，公共头不含平台头？
- [ ] `metadata.json` 的 `interfaces` 与 `kIfaces[]` 一致（只有 `shot.ui_provider`）？
- [ ] 用户可见文案都走 `i18n.tr()`、两种语言齐全，且 fallback 是中文原文？
- [ ] 日志仍是中文（e2e 断言依赖它），没有被误加进语言表？
- [ ] 非 Windows 平台是"CMake 跳过"，没有未验证的实现分支？
