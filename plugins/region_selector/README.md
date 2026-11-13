# region_selector —— 区域选择覆盖层插件（`shot.ui_provider`）

在**鼠标所在显示器**上弹出一个无边框、置顶、**半透明**、整屏的覆盖层。覆盖层显示后：
鼠标停在某个顶层窗口上时，选框会**吸附到该窗口**并带 100ms 指数平滑地平移/缩放过去；
**单击**（位移 ≤4px、按住 ≤400ms）直接把当前悬停窗口的矩形作为采集区域；**拖动**则退化成
原来的手绘选区（拖动期间严格跟手，不做吸附和动画）。确认后先隐藏覆盖层、等遮罩从屏幕上
消失，再发布带 `region` 的 `capture.request`（**Windows 与 Linux/X11 都是这套行为**；
Linux/Wayland 是同一套窗口与交互，但背景来自 xdg-desktop-portal 截图、确认后直接裁剪落盘，
见 §8）。

**平台**：Windows 是完整实现（Win32 物理坐标 + GDI 快照 + 窗口枚举）；Linux 同一份 `.so`
里编了**两套实现、运行期按会话类型二选一**：

- **Wayland 会话**（`WAYLAND_DISPLAY` / `XDG_SESSION_TYPE=wayland`）：走 xdg-desktop-portal
  的 Screenshot 门户 + 自己的覆盖层窗口（定格图 + 拖选，见 §8）；
- **X11 会话**：真正的全屏覆盖层——`XGetImage` 抓整块 X screen 当背景，交互与 Windows
  一致，窗口命中用 `XQueryTree` + EWMH 类型过滤（见 §9）；
- macOS 等其它平台由 CMake 直接跳过（`SHOT_PLUGIN_SKIPPED`）。

覆盖层是**不透明窗口 + 触发瞬间的桌面快照**（Windows 是 `region_background.cpp`，X11 是
`region_background_x11.cpp`）：先截一张桌面快照、上传成 GL 纹理，窗口用这张纹理当背景，
遮罩/挖空/十字线/主题色折角都画在它上面。窗口先隐藏、`glfwSetWindowSize/SetWindowPos`
覆盖整块显示器、**隐藏状态下先渲染两帧**再 `glfwShowWindow`；刻意**不用**
`glfwSetWindowMonitor`（那条路径会切窗口模式，表现为黑屏闪烁）。Windows 的窗口尺寸是
**显示器尺寸 + 1 像素**（`kOverlayOversize`）：刚好等于显示器尺寸时 Windows 会把它当成
"全屏窗口"走 fullscreen optimization，导致每次显示/隐藏闪一下、进程退出后甚至留下黑色
残影（真机踩过，详见 §6）；X11 没有这条规则，铺满即精确覆盖（见 §9）。

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

**平台边界**：Windows 分支的覆盖层几何、光标位置与"物理虚拟桌面像素"依赖 Win32 坐标语义
（`GetCursorPos` / `GetSystemMetrics(SM_*VIRTUALSCREEN)` / `EnumWindows`），代码在
`region_geometry.cpp` / `region_background.cpp` / `window_target.cpp`；Linux/Wayland 分支的
平台层在 `*_wayland.cpp` + `region_portal.cpp` / `region_image.cpp` / `region_output_linux.cpp`；
Linux/X11 分支在 `*_x11.cpp`（Xlib）。两套 Linux 实现在同一个 `.so` 里，启动时按会话类型
运行期选择（`LinuxSessionBackend`）。macOS 与其它平台由 CMake 跳过，不产出伪实现。

## 1. 目录

```
plugins/region_selector/
├── CMakeLists.txt                  # 平台源文件 + imgui / nlohmann-json / glfw3（Linux 另加 libportal / x11 / stb）
├── metadata.json                   # 插件清单（id region_selector.main，接口 shot.ui_provider）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── region_window.h             # RegionWindow 公共接口（契约层，不含任何图形库类型）
│   ├── region_window_impl.h        # 内部头：Impl 声明 / 事件队列 / 拖拽与悬停吸附状态 / Wayland、X11 段
│   ├── window_target.h             # 内部头：窗口目标结构 + 枚举/命中接口（Windows + X11）
│   ├── region_portal.h             # Linux：xdg-desktop-portal Screenshot 门户 + 会话探测
│   ├── region_image.h              # Linux/Wayland：截图解码 / 裁剪 / PNG 编码（stb）
│   └── region_output.h             # Linux/Wayland：输出目录（image_saver 配置 / Pictures）+ 文件名
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + kEntries + ABI 胶水 + 语言表
    ├── region_window.cpp           # 生命周期 / 窗口线程 / 事件循环 / 触发解析 / 提交（平台分派）
    ├── region_overlay.cpp          # 交互状态机 + 悬停吸附动画 + 主题色读取（三平台共用）
    ├── region_overlay_draw.cpp     # ImGui 遮罩 / 选区 / 折角 / 十字线 / 标签绘制
    ├── region_graphics.cpp         # ImGui 后端 / 中文字体 / DPI 字号缩放 / 每帧渲染
    ├── region_background.cpp       # Windows：触发瞬间的桌面快照（GDI）+ 上传 GL 纹理
    ├── region_geometry.cpp         # Windows：显示器矩形 / 光标 / 物理坐标换算 + 实测日志
    ├── window_target.cpp           # Windows：顶层窗口枚举 / Z 序 / DWM 边界 / 命中测试
    ├── region_portal.cpp           # Linux/Wayland：libportal 异步截图 + GMainLoop + URI→路径 + 超时
    ├── region_image.cpp            # Linux/Wayland：stb PNG 解码 / 裁剪 / 编码（唯一定义 stb 实现的 TU）
    ├── region_output_linux.cpp     # Linux/Wayland：读 config_list 的 image_saver 目录 + shot_时间戳.png
    ├── region_geometry_wayland.cpp # Linux/Wayland：显示器几何（无位置/无全局光标）+ 会话日志
    ├── region_background_wayland.cpp # Linux/Wayland：portal 截图 → stb 解码 → 删临时文件 → GL 纹理
    ├── region_overlay_wayland.cpp  # Linux/Wayland：等比适配映射 + 内存裁剪写盘 + 发布 capture.done
    ├── window_target_wayland.cpp   # Linux/Wayland：空实现（协议不允许全局窗口枚举，吸附关闭）
    ├── region_geometry_x11.cpp     # Linux/X11：X screen 几何 / XQueryPointer 光标 / 显示器矩形 + 日志
    ├── region_background_x11.cpp   # Linux/X11：XGetImage 整屏快照 → 像素解包 → GL 纹理
    ├── region_overlay_x11.cpp      # Linux/X11：覆盖层定位 / 实际几何日志 / 确认后发布（Windows 同时序）
    └── window_target_x11.cpp       # Linux/X11：XQueryTree + EWMH 类型过滤 + XTranslateCoordinates 命中
```

公共头 `region_window.h` 只声明一个类；内部头 `region_window_impl.h` 与 `window_target.h`
是**内部头**（只被本插件的 `.cpp` 包含，开头注明"不是插件对外接口"）。`.cpp` 按关注点拆成
"生命周期 / 交互 / 绘制 / 图形 / 平台几何 / 窗口枚举 / ABI 胶水"七个分区（`STYLE.md` §6.7/§6.8）；
平台层再按 Windows / Wayland / X11 各一套文件。

窗口枚举独立成 `window_target*.cpp`：它只回答"光标下有没有窗口、矩形是多少"，不碰 ImGui
和覆盖层状态；覆盖层交互文件负责把结果变成吸附/动画/采集。两者分开后，窗口筛选规则
（DWM cloaked / `WS_EX_TOOLWINDOW`，或 EWMH 类型 / reparenting frame）可以单独审查，
不会淹没在绘制代码里。X11 与 Wayland 的平台函数用**不同的成员函数名**（`x11*`），
让同一个 `.so` 里两套实现并存、不产生重复符号；`region_window.cpp` 只做运行期分派。

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

依赖与 `settings_ui` 同一套，vcpkg 清单里已启用对应特性；Linux 额外依赖系统包
`libportal-dev` 与 `libx11-dev`（见 §8 / §9）：

| 依赖 | 平台 | 用途 |
| --- | --- | --- |
| `imgui`（glfw-binding + opengl3-binding） | 全部 | 覆盖层绘制；后端源码已编进 `imgui.lib`，不抄进本工程 |
| `glfw3` | 全部 | 窗口、事件循环、显示器几何、光标（Linux 需要 GLFW 带 Wayland/X11 后端，见 §8 / §9） |
| `nlohmann-json` | 全部 | 触发负载解析 + 区域结果组包 + config_list 解析 + 多语言表解析（header-only） |
| `opengl32` / `user32` / `dwmapi` | Windows | ImGui OpenGL3 后端 / `GetCursorPos`、`EnumWindows` / 窗口扩展帧边界与 cloaked 检测 |
| `libportal-1.0`（`libportal-dev`） | Linux（Wayland 分支） | xdg-desktop-portal 的 Screenshot 门户（Wayland 唯一的通用整屏截图途径） |
| `x11`（`libx11-dev`） | Linux（X11 分支） | `XGetImage` 快照 / `XQueryTree` 窗口枚举 / `XTranslateCoordinates` / `XQueryPointer`；**不需要 `libxrandr-dev`**（见 §9.1） |
| `stb`（vcpkg，header-only） | Linux/Wayland | 截图像素解码、裁剪与 PNG 编码 |
| `OpenGL::GL` | Linux | ImGui OpenGL3 后端的 GL 1.x 入口 |

产物：`<构建树>/bin/plugins/region_selector.dll`（Linux 为 `.so`）；`metadata.json` 由
`shot_add_plugin()` 在链接后贴到产物尾部。插件不链接 spdlog：日志统一走
`shot::Logger`（宿主 `log` 服务）。

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
| 3 | UI 线程 | 取鼠标位置 → 找所在显示器 → 把隐藏窗口搬过去铺满整屏；读主题色、记录覆盖层原生句柄（HWND / X11 Window）、按 Z 序枚举顶层窗口命中光标下的窗口 → 显示并聚焦 |
| 4 | UI 线程 | 空闲：每 30ms 枚举一次顶层窗口，命中窗口后选框用 100ms 指数平滑吸附过去；鼠标移到别的窗口就平滑平移/缩放 |
| 5 | UI 线程 | 单击（位移 ≤4px 且按住 ≤400ms）：直接把当前悬停窗口矩形当 region 确认；拖动（位移 >4px）：选框严格跟手，松开左键确认；右键 / Esc = 取消 |
| 6 | UI 线程 | 确认后先 `glfwHideWindow` + `glfwPollEvents`，再 `sleep 100ms`（`kHideSettleDelayMs`） |
| 7 | UI 线程 | Windows / X11：发布 `capture.request`：`{"mode":"region","region":{"x","y","width","height"}}`；Wayland：内存裁剪写 PNG 后发 `capture.done` |
| 8 | 宿主 worker | Windows / X11：抓整屏 → 按 `region` 裁剪 → 处理 → 保存，发 `capture.done`；Wayland 不经过流水线 |

**覆盖层整屏全黑、只剩十字线**：说明背景快照没画出来——要么 `captureBackground()`（Windows
GDI / X11 `XGetImage`）失败（日志有"桌面快照失败"/"XGetImage"WARN），要么纹理上传后没在
`drawOverlay` 里 `AddImage`（检查 `backgroundTexture != 0`）。透明窗口方案已被证实会在
hide/show 后失效，不要再改回 `GLFW_TRANSPARENT_FRAMEBUFFER`。

**为什么第 6 步要先隐藏再等 100ms**：DWM / X11 合成器的合成是异步的，`glfwHideWindow`
返回时覆盖层可能还没从屏幕上消失；宿主 worker 收到事件就抓屏，中间没有任何同步点。
宁可慢 100ms，也不能把半透明遮罩截进图里。

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
| `region.hint`（Windows / X11） | 单击截取窗口 · 拖拽选择区域 · Esc / 右键取消 | Click a window to capture · Drag to select · Esc / right-click to cancel |
| `region.hint.wayland`（Linux） | 拖拽选择区域 · Esc / 右键取消 | Drag to select · Esc / right-click to cancel |
| `overlay.size.unit` | 像素 | px |
| `overlay.window_title` | Plugin-shot 区域选择 | Plugin-shot Region Select |

Wayland 下单击不拖 = 取消（没有窗口可截），所以用单独的 `region.hint.wayland` 文案，
避免提示里出现"单击截取窗口"这种做不到的说明。

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

### 3.3 选择握手（`region.select.request`）

`long_capture` 这类"自带采集层"的消费者不需要宿主流水线，它只想让用户先选一块区域。
本插件为此提供一个与普通框选**交互完全一致**的选择模式：

| 主题 | 方向 | 负载 |
| --- | --- | --- |
| `region.select.request` | 消费者 → 本插件 | `{"purpose":"long_capture"}` |
| `region.selected` | 本插件 → 消费者 | `{"purpose":"long_capture","region":{x,y,width,height}}` |
| `region.selected` | 本插件 → 消费者 | `{"purpose":"long_capture","cancelled":true}` |

- 有非空 `purpose` 时进入选择模式：窗口悬停吸附、单击选窗口、拖框选区、主题色折角都与
  普通框选一致；**确认时只发 `region.selected`，不发 `capture.request`**，消费者自己采集。
- Esc / 右键 / 选区过小 / 启动失败都会回一条 `cancelled:true`，消费者不用等到超时。
- `purpose` 为空或负载非法时安静忽略；普通 `capture.request` 触发的框选完全不变，仍然
  发带 `region` 的 `capture.request`（`smoke_region_window.ps1` 走的就是这条路径）。
- 已经在框选时收到新的选择请求只记 DEBUG、不重开覆盖层（避免打断用户正在拖的选区）。
- 非 Windows 平台里只有 Wayland 没有 `long_capture` 消费者，purpose 选择按取消回执
  （避免发起方空等）；**X11 与 Windows 一样支持 purpose 选择**，确认时回 `region.selected`。

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

**Linux/X11 的坐标语义**（`region_geometry_x11.cpp` / `region_overlay_x11.cpp`）：

```
region.x = 覆盖窗口在 root 里的位置.x + 窗口内光标坐标.x      // 不减任何原点
region.y = 覆盖窗口在 root 里的位置.y + 窗口内光标坐标.y
region.width / height = 选区在窗口内的宽高
```

X11 只有一块 X screen，root 原点恒为 `(0,0)`，所以没有 Windows 的"虚拟桌面原点"减法；
X 像素就是物理像素（`Xft.dpi` 只影响字体渲染），`content scale` 只放大字号/折角。
窗口位置用 `glfwGetWindowPos`（X11 实现是 `XTranslateCoordinates` 到 root），
即使 WM 没按请求摆放，发布结果仍与用户在屏幕上看到的位置一致。

## 5. 交互与视觉

| 输入 | 行为 |
| --- | --- |
| 鼠标移动（未拖动） | 每 30ms 枚举一次顶层窗口，命中光标下的窗口后把选框**平滑吸附**到它的矩形（100ms 指数平滑）；命中不到窗口时只显示遮罩与十字线 |
| 单击（位移 ≤4px 且按住 ≤400ms） | 直接用当前悬停窗口的矩形确认采集，不经过拖动 |
| 左键按下并拖动（位移 >4px） | 进入拖动模式：选框**严格跟手**，不做窗口吸附、不做动画；松开确认，任一边 < 8 物理像素按取消处理 |
| 松开左键（未越过阈值但按住 >400ms） | 按取消处理（不发布事件） |
| 右键 / Esc | 取消 |
| 鼠标移动 | 全屏十字线 + 选区尺寸标签（`宽 x 高` + 单位，物理像素） |

Linux/Wayland 下没有悬停吸附与"单击截窗口"（拿不到窗口列表）：上表的单击一行退化为
**取消**，拖动与 Esc/右键完全相同；尺寸标签显示的是换算后的**输出像素**，与落盘文件一致
（见 §8.3）。Linux/X11 的输入行为与 Windows 完全一致（有悬停吸附与单击截窗，尺寸标签
就是选区像素），差异只在窗口枚举的实现（见 §9.3）。

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
| X11 `XGetImage` 快照失败 / 纹理上传失败 | 只记 WARN，覆盖层退化为**纯色底 + 遮罩**；拖框/吸附/发布区域照常工作 |
| X11 `XQueryTree` 失败或拿不到 GLFW 的 X11 Display | 候选窗口为空，悬停吸附关闭；拖动框选仍然可用 |
| X11 窗口没有 `_NET_WM_WINDOW_TYPE`（非 EWMH 应用） | 照常作为候选（宁可多列，不漏真实窗口） |
| X11 WM 对隐藏窗口的位置/全屏请求不生效 | 显示后按**实际窗口位置/尺寸**换算与裁剪，并在日志里打印；选区仍准确，观感需真机确认（§9.5） |
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
| Linux 支持 Wayland 与 X11 两种会话 | 同一份 `.so` 同时编进两套实现，启动时按 `WAYLAND_DISPLAY` / `XDG_SESSION_TYPE` 运行期选择（Wayland 优先）。X11 见 §9，Wayland 见 §8 |
| X11 只支持单 X screen | 覆盖层只铺鼠标所在显示器；XQueryTree 枚举的是默认 X screen 的 root 子窗口，多 X screen（`:0.1` 等）不枚举、也不能跨 screen 覆盖。多显示器在同一 X screen 内是支持的，但一次仍只选一块屏 |
| X11 的覆盖层由窗口管理器管理 | 用的是普通 GLFW 无边框置顶窗口（不是 override-redirect）：WM 可能加装饰/延迟摆放/改动位置。代码按**实际窗口位置**换算与裁剪，选区结果不受影响，但"是否真的全屏、是否无边框"要真机确认（§9.5） |
| X11 下的采集依赖宿主流水线 | 本插件只发布带 `region` 的 `capture.request`（与 Windows 一致），实际抓屏由 `screen_capture` 完成。`screen_capture` 现在也有 X11 分支（XShm/XGetImage），但两边**尚未联调**；X11 会话下普通框选的端到端链路要真机确认（见 §9.4/§9.5） |
| X11 + XWayland 的边界 | Wayland 会话里 XWayland 应用是 X11 窗口，但本插件走 Wayland 分支（门户截图 + 自己的窗口），拿不到、也不枚举这些窗口；不要指望在 Wayland 会话里用 X11 路径去吸 XWayland 窗口 |
| X11 窗口枚举不做子控件命中 | 与 Windows 一样只认顶层窗口；`_NET_WM_WINDOW_TYPE_*` 过滤 DOCK/TOOLTIP/MENU 等，完全透明的点击穿透窗口没有额外过滤 |
| Wayland 无法悬停吸附 / 单击截窗 | 协议不允许枚举全局窗口、也拿不到别的窗口矩形，所以只有"拖框"一种选择方式；单击不拖 = 取消（见 §8） |
| Wayland 多屏是一张缩放图 | portal 的 Screenshot 门户一次返回**整个桌面合成图**，覆盖层把它等比缩放显示，不能按输出分开选；坐标换算基于图像像素，因此选中的内容仍然准确 |
| Wayland 覆盖层位置/全屏由合成器决定 | 客户端不能定位窗口；代码请求全屏并记录实际尺寸，带标题栏/未全屏时框选仍可用（坐标按实际窗口尺寸算），观感需要真机确认 |

## 8. Linux / Wayland

Linux 默认目标是 Wayland（Ubuntu 24.04 GNOME）。Wayland 下应用**不能**读屏幕、不能覆盖
所有输出、不能枚举窗口，所以采用"**门户截一次图 + 自己的窗口在上面框选**"的方案：

1. 收到 `capture.request`（`{"mode":"region"}`）后，调 `xdg-desktop-portal` 的
   **Screenshot** 门户做一次整屏截图（`xdp_portal_take_screenshot()` +
   `XDP_SCREENSHOT_FLAG_NONE`，非交互模式）。首次会弹**系统授权框**；取消/拒绝/超时都按
   取消处理：不弹窗、不发布事件。等授权最多 **30s**（`kPortalTimeoutSeconds`），等待期间每
   5s 写一条 info（「仍在等待 portal 授权…」）——真机上"按了没反应"多数就是这段等待，
   没有这几行日志时看起来像插件完全没工作。
2. 门户返回临时 PNG：`stb_image` 解码成 RGBA8 → **立刻删除临时文件** → 上传 GL 纹理，
   用自己的 GLFW/ImGui 覆盖层把整张图**等比缩放、居中**显示。
3. 用户拖框（或 Esc / 右键取消；单击不拖 = 取消）。确认后**先隐藏覆盖层并立刻
   `glfwPollEvents`**（见 §8.3 的时序说明），然后在内存里的截图像素上裁剪 →
   `stb_image_write` 写 PNG → 发布 `capture.done {path,width,height,processors:0}`。
   **不发布 `capture.request`**：不再第二次采集、不再弹第二次授权。

与 Windows 分支的对应关系：窗口/交互/绘制/主题色/语言表**完全复用**；只有平台层不同
（`*_wayland.cpp` 代替 `region_geometry.cpp` / `region_background.cpp` / `window_target.cpp`）。

### 8.1 依赖与构建

```bash
sudo apt-get install -y libportal-dev libgl1-mesa-dev
cmake -S . -B build/Linux -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Linux --target region_selector
```

- `pkg_check_modules(LIBPORTAL REQUIRED libportal-1.0)`：缺 `libportal-dev` 时**配置阶段
  直接失败**（不是运行期静默降级）。头文件是 `<libportal/portal.h>`（`xdp_portal_new()`
  在 `<libportal/portal-helpers.h>`，两个都包含），pkg-config 会带出 GLib/GIO 的包含目录。
  Ubuntu 24.04 的 `libportal-dev` 是 **0.7.1**；代码用的 `xdp_portal_take_screenshot_finish`
  从 0.6 起就是"返回 `file://` URI"，0.7.1 与 0.9.x 签名一致。
- GLFW 必须带 **Wayland 后端**（vcpkg 的 glfw3 端口在 Linux 会按系统依赖构建；系统包
  可装 `libwayland-dev libxkbcommon-dev`）。只带 X11 后端的 GLFW 在 Wayland 会话里建不出
  窗口：本插件记 WARN、放弃本次框选，不会崩溃。
- 运行期依赖：`xdg-desktop-portal` 与桌面后端（GNOME 是 `xdg-desktop-portal-gnome`）。
  没装/没运行 → 门户不可用，记 WARN，不弹窗、不发布事件。
- stb 来自 vcpkg 清单（已是依赖），不需要额外系统包。

### 8.2 触发方式

和其它平台一样由事件总线主题 `capture.request` 触发（负载 `{"mode":"region"}`）。Linux 上
没有托盘/全局热键插件，入口来自其它通用插件（例如工作区里的 `session_control` 草稿把
D-Bus `CaptureRegion()` 翻译成这条事件）或桌面自定义快捷键调用对应入口。本插件不自己
注册全局快捷键。

### 8.3 窗口、坐标与产出

- **窗口**：复用同一套 GLFW/ImGui 窗口（隐藏态延迟创建）。Wayland 客户端不能定位窗口，
  所以选"当前/主显示器"作为初始尺寸，窗口 map 后再请求全屏；是否真的全屏、是否带标题栏
  由合成器决定，日志会打印实际窗口尺寸。
- **坐标换算**（`region_overlay_wayland.cpp`）：

  ```
  scale   = min(窗口内容宽 / 图像宽, 窗口内容高 / 图像高)     // 等比适配
  offsetX = (窗口内容宽 - 图像宽 * scale) / 2                  // 居中
  图像 x  = round((窗口 x - offsetX) / scale)                  // 再夹进 [0, 图像宽]
  图像 y  = round((窗口 y - offsetY) / scale)                  // 再夹进 [0, 图像高]
  ```

  窗口坐标 ↔ 图像像素**每帧按实际窗口尺寸重算**，不假设"窗口 == 显示器"；落在黑边上的
  选区会被裁剪时夹掉。尺寸标签显示的是换算后的**输出像素**，与落盘文件一致。
- **输出目录**：读 `config_list` 里 `image_saver.main/directory`（与宿主流水线保存同一套
  配置）；为空/读不到 → 系统图片目录（`$XDG_PICTURES_DIR`，缺省 `$HOME/Pictures`）。
  目录不存在会尝试创建。
- **文件名**：固定 `shot_YYYYMMDD_HHMMSS.png`（本地时间）。同一秒内连续保存会覆盖同名文件。
- **事件**：只发 `capture.done`，负载与宿主流水线完全同格式；`processors` 恒为 0（没有经过
  `image_processor` 链）。不发 `capture.failed`（框选取消/保存失败只记日志）。
- **退出**：等授权框期间每 100ms 检查一次宿主是否要求退出；宿主退出/热卸载时立即取消门户
  请求（`stop()` 的 join 不会被授权框卡住）。已落盘但宿主正在退出时不再发事件。
- **确认/取消时的隐藏时序（真机踩过，别"优化"掉）**：`glfwHideWindow` 在 Wayland 上只是把
  `xdg_toplevel` 销毁 + `attach(NULL)` 写进 `wl_display` 的**发送缓冲**，请求要等下一次
  `glfwPollEvents` / `glfwWaitEventsTimeout` 里的 `wl_display_flush` 才发给合成器。所以
  `hideOverlay()` 之后必须立刻 poll（`commitWaylandSelection` 里连 poll 两次 + 20ms
  超时等待，`hideOverlay()` 里也统一补一次）。不 poll 的两个真机症状：① 覆盖层画面留在
  屏幕上不消失；② 隐藏后 UI 线程阻塞在条件变量上不再处理 Wayland 事件，合成器的 ping
  没人应答，GNOME 弹"应用没有响应"。
- **编码线程**：确认后的"裁剪 + PNG 编码 + 写盘 + 发布 `capture.done`"在**工作线程**
  （`runWaylandCommit`）里做，UI 线程只负责隐藏、算布局、释放 GL 纹理与起线程。整屏 4K
  的 stb PNG 编码要几百毫秒到数秒，留在 UI 线程会让"隐藏之后"的那段时间完全不处理事件。
  GL 纹理必须在 UI 线程释放（GL 上下文只在那里有效），整屏像素按 move 语义交给工作线程；
  `startSelection` / `stop()` 会先 join 上一次的提交线程，不会有两个写盘任务重叠。

### 8.4 与 Windows 的行为差异

| 行为 | Windows | Linux / Wayland |
| --- | --- | --- |
| 悬停吸附 / 单击截窗口 | 有 | **没有**（Wayland 不允许全局窗口枚举）；单击不拖 = 取消 |
| 背景 | 触发瞬间的当前显示器 GDI 快照 | 门户返回的**整个桌面**合成图，等比缩放居中 |
| 确认后 | 隐藏 → 等 100ms → 发带 region 的 `capture.request` | 隐藏 → `glfwPollEvents`×2 → 工作线程裁剪/编码/写 PNG → 发 `capture.done` |
| 图片经过 `image_processor` | 是 | 否（`processors:0`） |
| 多显示器 | 只覆盖一块屏；悬停窗口可跨屏 | 一张缩放图，整张桌面都能选 |
| 主题色 / 语言 / 选择框样式 | 相同 | 相同 |

### 8.5 需要真机验证（本机没有 Linux 桌面）

- 覆盖层是否真的全屏、是否被合成器加上标题栏（GNOME 不支持 `xdg-decoration` 时无边框请求
  会被忽略）；以及带标题栏时坐标是否仍然正确（按实际窗口尺寸算，理论上不受影响）。
- 多屏 + 分数缩放（125% / 150% / 200%）下的窗口尺寸、`content_scale` 与坐标换算：映射用的是
  窗口内容坐标与图像像素，理论上与缩放无关，但没有实机结论。
- 首次授权框的观感与超时（当前等待 **30s**，等待期间每 5s 一条「仍在等待 portal 授权…」）；
  授权框出现时是否抢焦点/是否遮住覆盖层
  （覆盖层是在拿到截图**之后**才显示的，设计上不会挡授权框）。
- portal 后端差异（GNOME / KDE / wlroots）：`CaptureRegion` 的触发入口、授权拒绝时门户
  返回的错误文本（取消/拒绝靠错误码 + 文本识别，不同版本可能不同）。
- 图像编码：超大整屏图（8K+）时 GL 纹理上传可能超过 GPU 上限（有 WARN，裁剪与落盘不受影响）。
- X11 会话：见 §9（同一份 `.so` 里另一套实现，与 Wayland 路径互不影响）。

### 8.6 真机诊断日志（按这个顺序看，一眼定位卡在哪一步）

一次正常的 Wayland 框选，`host.log`（Linux 是 `~/.plugin-shot/logs/host.log`，Windows 是
`%LOCALAPPDATA%\plugin-shot\logs\host.log`）里 region_selector 的行应该依次出现：

```text
region_selector：Linux 会话 = Wayland（门户定格图 + 拖选）          # 插件 init
region_selector：开始框选（后端=wayland，portal 定格图 + 自有覆盖层）  # 事件到了插件
region_selector：图形栈就绪（覆盖层窗口 WxH，GLFW_VISIBLE=0）        # GLFW/窗口/ImGui 建好
region_selector：开始请求 portal 截图（等待授权，最多 30s）
region_selector：仍在等待 portal 授权…（已等待 5s，最多 30s）        # 每 5s 一条
region_selector：portal 截图返回 status=ok（耗时 N ms）             # ← 分水岭
region_selector：已取得 portal 截图 WxH（图像像素，将等比缩放显示）
region_selector：准备显示覆盖层 WxH（显示器矩形 WxH）
region_selector：glfwShowWindow 完成（GLFW_VISIBLE=1，GLFW_FOCUSED=1，实际窗口 WxH，合成器决定尺寸）
region_selector：开始框选 显示器矩形=...                            # 覆盖层已经在屏幕上
-- 用户确认后 --
region_selector：覆盖层已请求隐藏（GLFW_VISIBLE=0），开始编码
region_selector：开始编码 PNG（工作线程）：裁剪 WxH @ (x,y) → <路径>
region_selector：已保存框选 PNG <路径> WxH（编码+写盘 N ms，工作线程）
region_selector：框选结果已保存 ...，已发布 capture.done
```

定位规则：

| 最后一行停在哪 | 结论 |
| --- | --- |
| 「开始框选」之前 | 事件没到插件（插件没加载 / 没订阅总线），看宿主日志里的插件加载结果 |
| 「开始框选」与「图形栈就绪」之间 | 卡在 GLFW 初始化 / 建窗口（Wayland 后端或 EGL 不可用），同段会有 `GLFW 错误 N：…`（ERROR） |
| 「开始请求 portal 截图」之后、没有「portal 截图返回」 | 卡在门户授权框；超过 30s 会有一条 `portal 截图超时` WARN |
| 有 `portal 截图返回 status=...` 但不是 `ok` | 门户拒绝/不可用/超时；紧跟的 ERROR 行「区域框选未开始：portal 截图失败」是最终结论 |
| 「准备显示覆盖层」之后没有「glfwShowWindow 完成」 | 卡在显示/全屏请求 |
| `覆盖层没有被合成器显示（GLFW_VISIBLE=0）` | 窗口没被 map：合成器拒绝，覆盖层不会出现（ERROR 已记录） |
| 「覆盖层已请求隐藏」之后没有「已保存」/「写盘失败」 | 工作线程异常退出（不应发生，`capture.done` 也不会发） |

## 9. Linux（X11）

X11 会话下本插件走**真正的全屏覆盖层**，交互与 Windows 一致：悬停窗口吸附、单击截窗、
拖框、主题色折角、Esc/右键取消。平台层全部在 `*_x11.cpp`，共享的交互/绘制代码
（`region_overlay.cpp` / `region_overlay_draw.cpp` / `region_graphics.cpp`）一行不用改。

同一份 `.so` 同时编进 Wayland（门户定格图）与 X11（Xlib）两套实现，**运行期按会话类型
二选一**——不是编译期开关，用户不需要为不同会话装两份插件。

### 9.1 依赖与构建

```bash
sudo apt-get install -y libx11-dev libportal-dev libgl1-mesa-dev
cmake -S . -B build/Linux -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Linux --target region_selector
```

- `pkg_check_modules(SHOT_X11 REQUIRED x11)`：缺 `libx11-dev` 时**配置阶段直接失败**，
  CMake 的 `FATAL_ERROR` 里写明 Ubuntu 包名。用到的是 `XGetImage` / `XQueryTree` /
  `XTranslateCoordinates` / `XQueryPointer` / `XGetWindowProperty` / `XInternAtom`。
- **不需要 `libxrandr-dev`**：显示器布局交给 GLFW（内部用 RandR），本插件只包含
  `<X11/Xlib.h>`；`glfw3native.h` 用 `GLFW_NATIVE_INCLUDE_NONE` 关掉了它默认带出的
  `<X11/extensions/Xrandr.h>`。运行期 GLFW / X 服务器仍然需要 RandR 扩展（正常桌面都有）。
- `libportal-dev` 仍然要装：Wayland 实现在同一个 `.so` 里，configure 阶段就要求它
  （见 §8.1）。**即使只用 X11 会话也不能省**——否则 Wayland 用户装同一份包时缺依赖。
- GLFW 必须带 **X11 后端**（vcpkg 的 glfw3 端口在 Linux 默认同时编 X11/Wayland）。
  只带 Wayland 后端的 GLFW 在 X11 会话里 `glfwInit` 失败：本插件记 ERROR、放弃本次框选，
  不会崩溃。
- 运行期需要 `DISPLAY` 指向可用的 X 服务器；纯无显示会话（headless）第一次框选会失败并
  记日志，插件加载本身不受影响。

### 9.2 X11 vs Wayland 的运行期选择

`RegionWindow::start()` 里判定一次，整个进程生命周期不变（显示服务器不会在运行中切换）：

```
WAYLAND_DISPLAY 非空  或  XDG_SESSION_TYPE == "wayland"（不区分大小写）  → Wayland 分支
其它情况                                                               → X11 分支
```

- **Wayland 优先**：Wayland 会话里 XWayland 也可能导出 `DISPLAY`，但能覆盖所有输出、
  读全局光标的是 Wayland 门户路径，不能因为 `DISPLAY` 存在就误判成 X11。
- 判定结果与后端会写进日志（`region_selector：Linux 会话 = X11/...`），真机排查
  "为什么没弹覆盖层"先看这一行。
- 判定只在插件 `init` 时做一次；`WAYLAND_DISPLAY` 之后被修改不会切换（与显示服务器
  生命周期一致）。

### 9.3 覆盖层、背景快照与窗口命中

**覆盖层窗口**（`region_window.cpp` + `region_overlay_x11.cpp`）：

1. 与其它平台一样**延迟到第一次框选**才 `glfwInit` / 建窗口（原因见 §3.1）；
2. 窗口 hint：无边框、`GLFW_FLOATING`（`_NET_WM_STATE_ABOVE`）、`GLFW_FOCUS_ON_SHOW`
   （Esc 需要键盘焦点）、`GLFW_RESIZABLE=FALSE`；**不透明**，不用
   `GLFW_TRANSPARENT_FRAMEBUFFER`（AGENTS §7 坑 #2）；
3. 光标所在显示器：`XQueryPointer` 取 root 坐标（X 像素 = 物理像素），再在 GLFW 的
   显示器矩形里找包含它的那块（GLFW 在 X11 下用 RandR 拿输出布局）；
4. 隐藏状态下 `glfwSetWindowSize/glfwSetWindowPos` 铺满整块显示器（含任务栏）。
   **不做 Windows 的 +1px**：那是 Windows 全屏判定的专有坑；也不用
   `glfwSetWindowMonitor`（会走 WM 的全屏状态切换）；
5. 隐藏状态下先渲染两帧再 `glfwShowWindow`（与 Windows 同一原因：避免第一帧未渲染内容
   被合成上去）；显示后把**实际**窗口几何写进日志。

**背景快照**（`region_background_x11.cpp`）：

- 用 `glfwGetX11Display()` 拿 GLFW 的 X11 `Display`，对默认 screen 的 root 窗口做
  `XGetImage(display, root, 0, 0, DisplayWidth, DisplayHeight, AllPlanes, ZPixmap)`，
  抓**整个 X screen**（不是只抓当前显示器）；
- 32bpp 且字节序与客户端一致时按行用 32 位字直接解包，通道按 `red_mask/green_mask/
  blue_mask` 归一化；16/24bpp 或大端等少见格式退回 `XGetPixel`（慢但正确）；
- 上传成 GL 纹理后立即释放临时像素（不常驻）。绘制时按覆盖层窗口在 root 里的**实际
  位置/尺寸**取 UV 子矩形，所以 WM 把窗口挪几像素也不会和桌面错位；
- 失败（`XGetImage` 返回空 / 纹理超上限）只 WARN，覆盖层退化为**纯色底 + 遮罩**，
  拖框/吸附/发布区域都不受影响（需求要求）。

**窗口命中**（`window_target_x11.cpp`）：

1. `XQueryTree(root)` 取 root 的直接子窗口；协议规定顺序是**底 → 顶**，代码反向遍历，
   得到与 Windows 一致的"最上层优先"列表；
2. reparenting WM（Mutter / KWin / Xfwm）会把客户端包进 frame：先按 `WM_STATE` 属性
   找到 client（找不到就用 root 子窗口自己），据此：
   - 排除覆盖层自己（frame 与 client 都排除，否则永远命中自己的全屏矩形）；
   - 用 client 上的 `_NET_WM_WINDOW_TYPE` 做类型过滤；
3. 排除：覆盖层自身、`map_state != IsViewable`（最小化/其它虚拟桌面）、`InputOnly`、
   零尺寸，以及 `_NET_WM_WINDOW_TYPE` 里的 DESKTOP / DOCK / TOOLBAR / MENU / UTILITY /
   SPLASH / DROPDOWN_MENU / POPUP_MENU / TOOLTIP / NOTIFICATION / COMBO / DND
   （与 Windows 的 `WS_EX_TOOLWINDOW` / cloaked 过滤对应）；
4. 矩形用 `XTranslateCoordinates(frame → root)` 换算到 root 坐标，再裁剪到 X screen；
   命中测试取第一个包含光标的窗口。枚举节流与其他平台相同（30ms）。

### 9.4 产出事件

| 触发 | 确认后发布 | 负载 |
| --- | --- | --- |
| 普通 `capture.request`（`{"mode":"region"}`） | `capture.request` | `{"mode":"region","region":{x,y,width,height}}`（X screen 物理像素） |
| `region.select.request`（`{"purpose":"long_capture"}`） | `region.selected` | `{"purpose":"long_capture","region":{...}}`，取消时 `{"purpose":...,"cancelled":true}` |

- 与 Windows 相同：先隐藏覆盖层 + `glfwPollEvents`，等 100ms（`kHideSettleDelayMs`）
  让 X11 合成器把遮罩从画面上拿掉，再发布。
- 取消（Esc / 右键 / 拖动选区任一边 < 8px / 无窗口时单击）不发布普通框选事件；
  purpose 选择回 `cancelled:true`。
- **注意**：普通框选发布的是 `capture.request`，真正的抓屏由宿主的 `screen_capture`
  完成。`screen_capture` 的 X11 分支已经实现（XShm 优先、XGetImage 回退，同一轮随
  Linux 全量构建通过），但两者**尚未真机联调**；X11 会话下选区 → 抓屏 → 落盘这条
  链路需要真机确认。本插件的区域选择、坐标与事件发布按 Windows 同款实现（purpose
  选择由消费者自己采集，不受此限制）。
- 本插件不自己写 PNG（不像 Wayland 的 B 方案）：X11 有真正的屏幕访问能力，交给宿主
  流水线才能统一经过 `image_processor` 链。

### 9.5 需要真机验证（本机没有 X11 桌面）

> 下面的条目**都还没有在真实 X11 会话跑过**。代码按 X11 协议与 EWMH 写，失败路径只记
> WARN、不影响框选；真机结果出来后再更新本节（不要在注释/文档里写"已验证"）。

- 覆盖层在 GNOME/KDE/Xfwm 等 WM 下是否真的无边框、铺满、置顶并拿到键盘焦点
  （`GLFW_FOCUS_ON_SHOW` 可能被焦点窃取保护忽略，Esc 需要焦点）；
- WM 对隐藏窗口 `setSize/setPos` 的处理（可能延迟到 map 时才摆放）：代码按显示后的
  实际窗口位置/尺寸换算，理论上选区准确，但背景 UV 裁剪与选框对齐需实测；
- `XGetImage` 在不同 depth/visual（24/30bpp、compositing 开关、Xvfb）下的像素解包；
- reparenting frame 的 Z 序与 `_NET_WM_WINDOW_TYPE` 过滤在 GNOME/KDE 下的实际效果
  （桌面窗口、面板、通知、工具提示是否都正确跳过）；
- 多显示器在同一 X screen 内时，只覆盖鼠标所在显示器；跨屏拖选仍受 §7 的"单显示器"
  限制。**多 X screen 不支持**；
- 分数缩放 / 大字体：`content scale` 只影响字号，X 像素即物理像素；150%/200% 下的
  观感与 `glfwGetMonitorContentScale` 的返回值需实测；
- 与 `screen_capture` 的 X11 分支联调（见 §9.4）；
- XWayland 应用在**X11 会话**里是普通 X11 窗口，可正常命中；在 **Wayland 会话**里
  XWayland 窗口不会被枚举（走 Wayland 分支），这是平台边界，不是 bug。

## 10. 检查清单

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
- [ ] macOS 等无实现平台仍是"CMake 跳过"，没有未验证的实现分支？
- [ ] Wayland：确认后只发 `capture.done`（不发 `capture.request`，不第二次采集/授权）？
- [ ] Wayland：裁剪用的是内存里的 portal 截图，临时文件读完后已删除？
- [ ] Wayland：窗口坐标 → 图像像素用的是"实际窗口尺寸 + 等比缩放 + 居中偏移"，并夹进图像范围？
- [ ] Wayland：没有调用任何全局窗口枚举（悬停吸附关闭、单击=取消、Wayland 的错误只 WARN/静默）？
- [ ] Wayland：门户不可用 WARN 且不弹窗不发事件；用户取消/拒绝/超时安静放弃？
- [ ] Wayland：输出目录读的是 `image_saver.main/directory`，为空回退 Pictures，文件名 `shot_YYYYMMDD_HHMMSS.png`？
- [ ] X11：`WAYLAND_DISPLAY` / `XDG_SESSION_TYPE=wayland` 判定优先于 `DISPLAY`（XWayland 不会被误判成 X11 会话）？
- [ ] X11：覆盖层用的是**不透明** GLFW 窗口（没有 `GLFW_TRANSPARENT_FRAMEBUFFER`），背景是 `XGetImage` 快照？
- [ ] X11：快照失败只 WARN，覆盖层退化为纯色底 + 遮罩，框选仍可用？
- [ ] X11：窗口枚举用 `XQueryTree(root)` 且**反向**遍历（底 → 顶），矩形用 `XTranslateCoordinates` 到 root？
- [ ] X11：排除了覆盖层自己（含 reparenting WM 的 frame）与 `_NET_WM_WINDOW_TYPE_TOOLTIP/DOCK/...` 类型？
- [ ] X11：`region` 是 root 坐标（原点 (0,0)），没有减任何"虚拟桌面原点"、没有乘 content scale？
- [ ] X11：普通框选发 `capture.request`，purpose 选择发 `region.selected`（取消带 `cancelled:true`）？
- [ ] X11：确认时序仍是"隐藏 → 等 100ms → 发布"，与 Windows 一致？
- [ ] X11：Wayland 实现文件（`*_wayland.cpp` / `region_portal.cpp`）没有改动（两套平台函数用不同名字，避免重复符号）？
