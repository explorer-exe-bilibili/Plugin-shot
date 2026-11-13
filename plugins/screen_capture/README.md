# screen_capture —— 屏幕采集插件（`shot.screen_capture`）

把屏幕内容写进宿主已经分配好的 BGRA8 缓冲；Windows 与 Linux/X11 上可选地把鼠标
光标一起合成进截图。插件只负责"抓"：不处理、不保存、不分配图像内存。配置项有两项：
`include_cursor`（Windows/X11 光标合成）与 `source`（Wayland 共享输出还是窗口）。
宿主编排的"采集 → 处理 → 保存"流水线见 `document/CAPTURE.md`，能力契约见
`document/ABI_POLICY.md` 的登记表。

本插件实现两个能力：采集 `shot.screen_capture` 与配置 `shot.configurable`。

平台实现由 CMake 按平台选择（源文件互不参与对方编译）；Linux 只有一份 `.so`，
Wayland 与 X11 两条后端在运行期按会话类型二选一：

| 平台 | 实现 | 状态 |
| --- | --- | --- |
| Windows | GDI（`GetDC` + `BitBlt` + `GetDIBits` + `DrawIconEx`），整块虚拟桌面 | ✅ 已可用 |
| Linux/Wayland | `xdg-desktop-portal` 的 ScreenCast 门户 + PipeWire，一次一个源（输出或窗口） | 已实现，**需真机验证** |
| Linux/X11 | `XShmGetImage`（MIT-SHM）优先、`XGetImage` 兜底，XFixes 光标合成，整个 X screen | 已实现，**需真机验证**（见 §6） |
| macOS | ScreenCaptureKit | 尚未实现，构建时跳过（`SHOT_PLUGIN_SKIPPED`） |

## 1. 目录

```
plugins/screen_capture/
├── CMakeLists.txt                  # WIN32=GDI；UNIX=Wayland(portal/PipeWire)+X11(x11/xext/xrandr/xfixes)；APPLE 跳过
├── metadata.json                   # 插件清单（id screen_capture.main，接口 shot.screen_capture + shot.configurable）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── capture_settings.h          # CaptureSettings：include_cursor/source 状态 + shot.configurable 回调声明
│   ├── screen_capture.h            # ScreenCapture 公共接口（契约层；不含 Win32 / Xlib / GLib 类型）
│   ├── screen_capture_state.h      # 内部头：插件实例状态（语言表 + 配置 + 采集对象，两个 .cpp 共享）
│   ├── screen_capture_impl.h       # 内部头：GdiDesktopCapture（仅 Windows）
│   ├── wayland_capture.h           # 内部头：WaylandCapture（环境判断/会话编排/取帧）
│   ├── wayland_portal_session.h    # 内部头：WaylandPortalSession（ScreenCast 门户状态机）
│   ├── wayland_portal_session_impl.h # 内部头：门户 PIMPL 状态（生命周期与回调两个 .cpp 共享）
│   ├── wayland_pipewire_stream.h   # 内部头：WaylandPipeWireStream（PipeWire 接收与协商）
│   ├── wayland_frame_buffer.h      # 内部头：WaylandFrameBuffer（BGRA8 双缓冲）
│   └── x11_capture.h               # 内部头：X11Capture（XShm/XGetImage / BGRA8 转换 / XFixes 光标）
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + kEntries + 语言表 + ABI 胶水
    ├── screen_capture.cpp          # Windows 契约层：入参校验、返回码映射、日志
    ├── screen_capture_linux.cpp    # Linux 契约层：构造时按会话类型分派 Wayland/X11，共用校验与返回码
    ├── capture_settings.cpp        # 配置层：include_cursor/source 声明（label/tooltip 是文案 key）+ get/set 回调
    ├── gdi_desktop_capture.cpp     # Windows 平台层：DPI 上下文 / GetDC / BitBlt / DrawIconEx / GetDIBits
    ├── wayland_capture.cpp         # Linux 编排层：Wayland 环境检测、会话重建、取帧
    ├── wayland_portal_session.cpp  # 门户生命周期：GLib 线程、Ready/Failed 等待、清理与 join
    ├── wayland_portal_callbacks.cpp # 门户异步链：CreateSession/SelectSources/Start/OpenPipeWireRemote
    ├── wayland_pipewire_stream.cpp # 协议层：fd 连接、Bgrx/Rgbx 协商、process 回调
    ├── wayland_frame_buffer.cpp    # 帧层：BGRA8 排列转换、双缓冲、按 stride 写入
    ├── x11_capture.cpp             # X11 平台层：XOpenDisplay / XShm/XGetImage / 显示器布局日志
    ├── x11_image_convert.cpp       # X11 像素层：XImage → BGRA8（24/32bpp、两种字节序、mask 通道）
    └── x11_cursor.cpp              # X11 光标层：XFixesGetCursorImage + 预乘 alpha 合成
```

公共头 `screen_capture.h` 只声明一个类，内部头是**内部头**（只被本插件的 `.cpp`
包含，开头注明"不是插件对外接口"）；`.cpp` 按关注点拆成"契约层 / 配置层 / 平台层 /
ABI 胶水"，Linux 的平台层再拆成 Wayland 的"编排 / 门户 / PipeWire / 帧缓冲"与 X11 的
"抓取 / 像素转换 / 光标"（`STYLE.md` §6.7/§6.8）。实例状态放在 `screen_capture_state.h` 而不是 `plugin_entry.cpp`：
采集回调与配置回调分属两个 `.cpp`，必须操作同一份状态。

## 2. 构建

宿主仓库内：由根 `CMakeLists.txt` 的 `plugins/*/CMakeLists.txt` glob 自动纳入，
跟着整体构建一起产出；也可以只构建本插件：

```bash
cmake -S . -B build/Windows -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows --target screen_capture            # Windows
cmake --build build/Linux  --target screen_capture             # Linux（需先装下面的依赖）
```

离线 SDK 独立构建：

```bash
cmake -S plugins/screen_capture -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

### 2.1 依赖

Windows（系统库，无需 apt/vcpkg 额外包）：

| 依赖 | 用途 |
| --- | --- |
| `user32` | `GetDC` / `ReleaseDC` / `GetSystemMetrics` / `GetCursorInfo` / `GetIconInfo` / `DrawIconEx` / `SetThreadDpiAwarenessContext`（运行期解析） |
| `gdi32` | `CreateCompatibleDC` / `CreateCompatibleBitmap` / `BitBlt` / `GetDIBits` / `DeleteObject` / GDI 对象 |

Linux（Ubuntu 24.04 / Debian 系）：

```bash
sudo apt install libportal-dev libpipewire-0.3-dev \
                 libx11-dev libxrandr-dev libxfixes-dev libxext-dev pkg-config
```

| 依赖 | 用途 |
| --- | --- |
| `libportal-1.0`（`libportal-dev`） | xdg-desktop-portal 的 ScreenCast 门户封装（异步 GMainLoop） |
| `libpipewire-0.3`（`libpipewire-0.3-dev`） | 接收门户打开的视频流（Bgrx/Rgbx，MemFd buffer） |
| `glib-2.0` / `gio-2.0` | libportal 的传递依赖（`libportal-dev` 会带上 `libglib2.0-dev`） |
| `x11` / `xext`（`libx11-dev` / `libxext-dev`） | Xlib 连接与 `XGetImage`；MIT-SHM 的 `XShmGetImage`（`XShm.h` 在 `libxext-dev`） |
| `xrandr`（`libxrandr-dev`） | 只在 debug 日志里列显示器布局（`XRRGetMonitors`，RandR 1.5+） |
| `xfixes`（`libxfixes-dev`） | `XFixesGetCursorImage` 取光标位图 + 热点 |
| `pkg-config` | CMake 用 `pkg_check_modules` 找上面的模块 |

CMake 在 `UNIX AND NOT APPLE` 分支里**同时强制要求** Wayland（libportal/PipeWire）与
X11（x11/xext/xrandr/xfixes）依赖：**缺一个就 FATAL_ERROR，并打印上面的 apt 命令**，
不会产出运行期静默不可用的插件。`libxext-dev` 通常随 `libxrandr-dev` 一起装，但
`-lXext` 是 XShm 的硬需求，CMake 也单独检查 `xext`。

两个平台都需要 header-only 的 `nlohmann-json`（根 vcpkg 清单已有；只用于 i18n）。
插件不链接 spdlog（日志走宿主服务）。

产物：`<构建树>/bin/plugins/screen_capture.dll` / `libscreen_capture.so`；
`metadata.json` 由 `shot_add_plugin()` 在链接后贴到库尾部，并先经过
`cmake/ValidatePluginMeta.cmake` 校验。

## 3. 能力与调用时序

本插件实现两个能力：采集 `shot.screen_capture`（`ShotScreenCaptureV1`，版本 1）与
配置 `shot.configurable`（`ShotConfigurableV1`，版本 1，声明 `include_cursor` 与
`source` 两项）。

### 3.1 屏幕采集（`shot.screen_capture`）

采集是**两段式**的，插件从不分配图像内存：

| 步骤 | 调用方 | 做什么 |
| --- | --- | --- |
| 1 | 宿主 | 调 `describe(self, out)`，`out->struct_size` 已由宿主填好 |
| 2 | 插件 | 填 `width` / `height` / `origin_x` / `origin_y` / `pixel_format = BGRA8` |
| 3 | 宿主 | 按 `width * 4 * height` 分配 BGRA8 缓冲，填好 `ShotImage`（`channels = 4`、`stride = width*4`） |
| 4 | 宿主 | 调 `capture(self, dst)` |
| 5 | 插件 | 只写像素：按 `dst->stride` 逐行写 `width*4` 字节，不碰结构体字段 |
| 6 | 宿主 | 拿图去处理 / 保存；插件不得保存 `dst` 或 `data` |

`describe` 填的几何字段：

| 字段 | Windows（GDI） | Linux（Wayland） | Linux（X11） |
| --- | --- | --- | --- |
| `width` / `height` | 多显示器合成后的虚拟桌面**物理像素**总尺寸 | **一个源**（输出或窗口）经 PipeWire 实际协商出的帧尺寸（不是门户报的 compositor 尺寸） | 默认 X screen 的 `DisplayWidth` × `DisplayHeight`（XRandR/Xinerama 把多显示器合并成一个 screen） |
| `origin_x` / `origin_y` | 内容左上角在系统坐标里的位置；主屏左上角不一定是 `(0,0)`，左侧/上方的副屏会让它变成负数 | 恒为 `(0,0)`：Wayland 门户一次只共享一个源，没有虚拟桌面合成坐标系 | 恒为 `(0,0)`：root window 的坐标原点就是 `(0,0)` |
| `pixel_format` | 目前只有 `SHOT_PIXEL_FORMAT_BGRA8`（4 字节/像素） | 同左 | 同左 |

**DPI 保证（Windows）**：几何与像素都是**物理像素**，不随 DPI 缩放。插件在每次
`describe` / `capture` 进入平台层时，都显式把调用线程的 DPI 感知上下文临时提升到
`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`（Win10 1607+，运行期解析；设置失败只
`warn`、按原上下文继续），返回前恢复原上下文。所以**不依赖宿主进程的 DPI 感知声明**。
实测本机混合 DPI 双屏（主屏 150% 2560x1600 @0,0、副屏 100% 1024x768 @2560,0）：
`--capture` 产出 `3584x1600`，两个色块标记窗口的像素与独立抓屏逐像素一致。

**DPI 保证（Wayland）**：靠 PipeWire 协商。Wayland 下 `describe` 报的是 PipeWire
buffer 的真实像素尺寸；门户的 stream `size` 属性属于 compositor 坐标（HiDPI/分数
缩放下可能小于像素尺寸），只作为协商默认值，不能直接当 `describe` 的尺寸。

**DPI 保证（X11）**：root window 的像素就是物理像素。X11 没有 Windows 的 DPI
虚拟化，插件不调用任何缩放 API，也不提升线程 DPI 感知上下文。

返回码（两个函数共用，`document/CAPTURE.md` §2.1 有同样的表）：

| 返回码 | 含义 |
| --- | --- |
| `0` | 成功 |
| `1` | 当前没有可采集的屏幕（无显示器 / 远程会话断开 / X11 screen 尺寸非法） |
| `2` | 参数不合法：`out`/`dst` 为空、`struct_size` 装不下、不是 BGRA8、`stride` 装不下一行、尺寸与当前屏幕/流不一致 |
| `3` | 采集通道不可用（系统调用失败 / 门户不可用 / 用户取消 / 超时 / PipeWire 失败 / X11 连不上 X server；已通过宿主日志写明原因） |

宿主对任何非 0 都发布 `capture.failed`，并且不会使用 `dst` 的内容。

### 3.2 可配置能力（`shot.configurable`）

本插件自己声明配置项，宿主负责生成设置页、存盘与启动回灌（`document/SETTINGS.md`）；
插件只实现 `get_config_count` / `get_config_desc` / `get_config` / `set_config` 四个回调。
不认识的 key 一律返回负值——宿主升级后多灌一项配置时，老插件不能因此坏掉。

| key | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `include_cursor` | BOOL | `"1"` | Windows/X11：抓屏时是否把当前鼠标光标一起合成进截图。Wayland 下没有可执行语义（光标由门户/合成器决定），见 §5.4 与 §6.4 |
| `source` | ENUM | `source.monitor` | Wayland：共享整个输出（`source.monitor`）还是单个窗口（`source.window`）；改变后下一次建立/重建会话生效。Windows 与 X11 忽略。候选值本身就是文案 key，旧配置里的 `monitor` / `window` 仍然接受 |

`ShotConfigDesc::label/tooltip` 存的是**文案 key**，不是最终显示文本；设置页按
`display(owner, key)` 到 `i18n_list` 里取当前语言（owner 是 `screen_capture.main`）：

| 配置 key | label key | tooltip key |
| --- | --- | --- |
| `include_cursor` | `config.include_cursor.label` | `config.include_cursor.tooltip` |
| `source` | `config.source.label` | `config.source.tooltip` |

key 规范、语言表格式与回退行为见 §3.3。

- 值一律用字符串表达；布尔只接受 `"1"`/`"0"`/`"true"`/`"false"`，ENUM 只接受
  描述符里声明的值（`source.monitor` / `source.window`；旧配置里的 `monitor` /
  `window` 仍然接受并归一成新键），其它写法按非法值拒绝并保留原值
  （不把 `"yes"`/`"2"`/`"Monitor"` 静默当成合法值）。
- **线程模型**：`capture` 在流水线 worker 线程、`set_config` 在宿主 main 线程，
  两项配置都用 `std::atomic` 保存；一次抓屏只在开头读一次快照。`source` 的变更
  由采集层在下一次 describe/capture 时比较并重建门户会话。
- 抓屏期间改配置只影响**下一张**图。
- 存盘键是 `screen_capture.main/include_cursor` 与 `screen_capture.main/source`（宿主按
  "插件 id/key" 拼），可以手改 `config.json`，验证脚本也用它把开关固定成 `"1"`。

### 3.3 多语言（i18n）

> 语言表同时包含插件自身的 `plugin.name` / `plugin.description`：设置页的插件分区标题按
> `display(插件 id, "plugin.name")` 解析（查不到时回退成 key 本身）。

用户可见文案走 `shot::I18n`（`include/shot_plugin_helpers/shot_i18n.h`）。这条链路
零 ABI 改动：插件用 `lists.add` 把自己的语言表发布到 `i18n_source_list`，宿主汇总后
生成只读的 `i18n_list`，所有消费方按同一条规则解析：

```text
display(owner, key) = i18n_list[(owner, key)] ?? key
```

- **语言表位置**：`src/plugin_entry.cpp` 文件作用域的 `kLangTables[]`，每种语言一个
  JSON 对象，`@meta` 是 BCP-47 语言标签（当前 `zh-CN` / `en-US`）。两张表的 key 集
  必须完全一致，每个 key 两种语言都要有文本；JSON 内不能有裸换行（单表超过 120 列，
  所以拆成相邻字符串字面量，编译期拼成一条 JSON）。
- **加载时机**：本插件没有 `ui_provider` 的 init/deinit 生命周期，`create(index, host)`
  是唯一拿得到宿主服务表的时机，所以在 `src/plugin_entry.cpp` 的 `create_instance()`
  里构造 `std::unique_ptr<shot::I18n>` 并调用
  `loadTables(kLangTables, std::size(kLangTables))`；`destroy_state()` 里 `reset()`，
  `I18n` 析构会退订 `lists.changed`。
- **owner**：`screen_capture.main`（插件身份证 `kScreenCapture.id`，与
  `metadata.json` 一致）；`label/tooltip` 的 owner 由宿主按贡献方 id 填。
- **key 规范**：配置项用 `config.<配置 key>.label` / `config.<配置 key>.tooltip`；
  插件自己的 UI 文案用 `ui.*`——本插件没有自绘 UI，因此没有 `ui.*` key。
- **回退**：`tr(key, fallback)` 的 `fallback` 必须写**改造前的原中文**；老宿主没有
  列表服务 / 事件总线时 `I18n` 安静降级，设置页按上面的规则显示 key 本身。
- **不翻译的东西**：日志（`shot::Logger`）保持中文——e2e 按中文日志断言；事件主题、
  配置 key、`BGRA8` / `GDI` / `DPI` 这类技术名词也不翻译。

## 4. 抓取流程（Windows GDI）

`src/gdi_desktop_capture.cpp` 一次完整采集的流水线（`describe` 只做第 1、2 步，第 12 步的恢复照样执行）：

| # | 系统调用 | 失败时 |
| --- | --- | --- |
| 1 | `SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2)` | 运行期解析；不可用只 `warn`，按原上下文继续 |
| 2 | `GetSystemMetrics(SM_*VIRTUALSCREEN)` | 宽或高 <= 0 → 返回 1 |
| 3 | `GetDC(NULL)` 拿屏幕 DC | 返回 3 并记日志 |
| 4 | `CreateCompatibleDC` + `CreateCompatibleBitmap` | 逆序释放已拿到的句柄，返回 3 |
| 5 | `SelectObject` 选中位图 | 释放位图与 DC，返回 3 |
| 6 | `BitBlt(SRCCOPY \| CAPTUREBLT)`，源坐标 = `(origin_x, origin_y)` | 仍继续走释放路径，返回 3 |
| 7 | `include_cursor = 1` 时：`GetCursorInfo` → `GetIconInfo` → `DrawIconEx(DI_NORMAL)` | 只 `warn`，不影响抓屏结果（光标是锦上添花） |
| 8 | `GetDIBits`（32bpp、`biHeight` 为负 = top-down、`BI_RGB`） | 返回 3 |
| 9 | 按 `dst->stride` 逐行写；`stride != width*4` 时先落临时缓冲再逐行拷 | 分配失败返回 3 |
| 10 | 每行 alpha 字节置 255 | —— |
| 11 | `DeleteObject` / `DeleteDC` / `ReleaseDC` 逆序释放 | —— |
| 12 | 恢复调用前的线程 DPI 上下文（RAII，成功/失败所有返回路径都恢复） | —— |

几个容易踩的点，代码里都有对应注释：

- **DPI 不能靠宿主**：`GetSystemMetrics` 与 `BitBlt` 必须落在同一套坐标系里，而 DPI
  awareness 是线程/进程属性。宿主可能是 DPI-unaware（没有清单、被别的宿主加载、
  以后换入口），也可能被 UI 插件顺手改成别的模式。插件自己提升、自己恢复，才能保证
  "不管谁调我，报的尺寸和抓的像素都是物理值"。
- **负高度 DIB**：让 `GetDIBits` 读回的第一行就是屏幕最上面一行，和 `ShotImage`
  的行序一致，正向 `stride` 不用再翻转。
- **stride 不能假设连续**：`stride` 可以为负（自底向上，`data` 指向首行但地址往下走），
  也可以大于 `width*4`（行尾填充）。只有 `stride == width*4` 才让 `GetDIBits` 直接写
  `dst->data`；其它情况先落临时缓冲再逐行拷，行尾填充保持宿主原值。
- **alpha 必须置 255**：屏幕 DC 没有 alpha 通道，`GetDIBits` 的 32bpp 输出第 4 字节
  通常是 0；不置 255 保存成 PNG/WebP 会整张透明。
- **`CAPTUREBLT` 不含光标**：它把分层窗口（部分工具/游戏/提示框）一起抓进来，但
  `BitBlt` 系从来不含鼠标光标，所以光标要在 BitBlt 之后单独合成。
- **光标合成坐标**：`GetCursorInfo` 的 `ptScreenPos` 是系统坐标，先减虚拟桌面 `origin`
  得到图像坐标，再减 `GetIconInfo` 的热点才是 `DrawIconEx` 的左上角（箭头热点是
  `(0,0)`，I 型/十字光标不是，不减会整体画偏）。中间用 `int64` 计算并夹进 `int`
  范围。`GetIconInfo` 返回的 `hbmMask`/`hbmColor` 必须 `DeleteObject`（成功、失败
  路径都要），否则常驻截图会按次累积 GDI 对象；`hCursor` 是系统光标，不归插件释放。
  `CURSOR_SHOWING` 未置位（全屏视频 / 锁屏 / 远程会话 / 程序主动隐藏）时什么都不画。
- **负 origin 全程有符号**：`origin_x/origin_y` 来自 `GetSystemMetrics` 的有符号返回值，
  原样存进契约的 `int32_t`，再原样作为 `BitBlt` 的 `int` 源坐标；行寻址用 `int64` 算
  `y * stride`。所以副屏在主屏左侧/上方时这条链不会把负号丢掉或当成无符号大数。
  **这套路径未实机验证**（本机两块屏都在主屏右侧，见 §7.1）。
- **句柄生命周期**：任何一步失败都走同一条逆序释放路径，不靠 GC、也不把句柄挂到
  实例上——每次 `capture` 都是完整的一次生命周期。

## 5. Linux（Wayland）实现

Wayland 不允许应用直接读屏幕：唯一通用路径是 `xdg-desktop-portal` 的 **ScreenCast
门户**（用户授权 + 选择共享哪个输出），画面经 **PipeWire** 以视频流交给应用。本插件
的实现分四层：`wayland_capture`（编排）、`wayland_portal_session`（门户）、
`wayland_pipewire_stream`（PipeWire）、`wayland_frame_buffer`（BGRA8 双缓冲）。

### 5.1 会话建立流程（首次截屏会弹系统授权框）

1. **会话类型在契约层已分流**：`ScreenCapture` 构造函数按 `WAYLAND_DISPLAY` /
   `XDG_SESSION_TYPE` 选择后端（规则见 §6.2）；只有 Wayland 会话才会创建本后端的
   对象。`WaylandCapture::ensureReady` 仍保留一次环境自检（防御构造后环境被改），
   不通过时返回 3 —— 这是防御路径，正常 X11 会话不会走到这里。
2. **懒建立**：插件加载时不建会话、不弹框；第一次 `describe` 才建立。
3. **门户线程**：插件创建自己的 GLib 线程，推入线程默认 `GMainContext` 后调用
   `xdp_portal_new()`——libportal 的异步回调投递到发起调用时的 thread-default 上下文，
   所以门户对象与回调必须留在同一个线程。宿主线程只阻塞等待结果，不跑 GLib 主循环。
4. **状态机**：`Idle → Starting →（CreateSession + SelectSources）→（Start，弹授权/
   选择框）→ Ready`，失败进 `Failed`。到达 `Ready` 后主循环继续运行，以接收
   `closed` 信号（用户撤销共享 / 输出被移除）。
5. **参数**：`types = XDP_OUTPUT_MONITOR`（配置 `source` = `source.monitor`，共享整个输出）
   或 `XDP_OUTPUT_WINDOW`（`source` = `source.window`，共享单个窗口），一次只选一个源
   （`multiple=false`）；`cursor_mode = XDP_CURSOR_MODE_EMBEDDED`、
   `persist_mode = XDP_PERSIST_MODE_NONE`（不记住授权，重建会话会重新弹框）。
   `source` 改动后下一次 describe/capture 会先结束旧会话再重建（会再次弹框）。
6. **授权/取消/超时**：等待上限 120 秒。用户取消 → 返回 3 + "用户取消了共享"；
   超时 → 取消挂起的 D-Bus 请求并返回 3 + "等待 Wayland 门户授权超时（120 秒）"；
   门户不可用（没装/没跑/没有后端）→ 返回 3 并提示 Ubuntu 包名。
   授权期间宿主采集 worker 线程会一直等待（流水线整段持生命周期锁），这是系统
   对话框的固有代价；插件不把"还没授权"伪造成成功帧。
7. **拿流**：`xdp_session_get_streams()` 的 `a(ua{sv})` 里取第一个带 `size` 的
   `node_id` / `(width, height)`，再 `xdp_session_open_pipewire_remote()` 拿 fd。
   Ubuntu 24.04 的 libportal 0.7.x 是会话级接口；新版 libportal 的
   `xdp_portal_open_pipewire_remote_for_screencast()` 是同一件事的改名/搬家。
8. **失败重试**：失败后不自动重试；下一次 `describe`（新的一次截屏请求）会重建会话
   并再次弹框。环境不是 Wayland 时不重试（状态不会自己变）。

### 5.2 PipeWire 帧路径

```text
门户 fd → pw_context_connect_fd（PipeWire 接管 fd）
        → pw_stream_new/add_listener → pw_stream_connect(node_id, INPUT, AUTOCONNECT|MAP_BUFFERS)
        → param_changed：协商 BGRx / RGBx（BGRA 兜底），请求 MemFd（MemPtr 兜底）
        → process：dequeue → 校验 stride/offset/maxsize → 转 BGRA8 写入后台帧缓冲
                    → swap 前台 + 唤醒采集线程 → queue_buffer
        → describe/capture 线程：WaylandFrameBuffer::fetch 等最多 200ms 取一帧
```

- **线程**：`pw_thread_loop` 自带一个线程，`state_changed` / `param_changed` /
  `process` 都在该线程执行（PipeWire 保证回调期间持有 loop 锁）。它和门户的 GLib
  线程都由插件创建、由 `destroy` 路径停掉并 join；宿主线程不被拉进任何库的调度循环。
- **为什么是两个线程**：libportal 与 PipeWire 各有一个阻塞式事件循环，把两者塞进
  同一个线程需要互相注册 fd/超时并且很难保证两边的线程亲和性；两个专属线程各自
  遵守各自库的线程规则，帧数据只经互斥的 `WaylandFrameBuffer` 跨线程。
- **字节排列**：`BGRx` 整行拷贝后把 alpha 置 255；`RGBx` 逐像素交换 R/B 后 alpha
  置 255；`BGRA` 拷贝后同样置 255（截图应当不透明，避免保存成 PNG 时整张透明）。
- **stride/offset 防护**：`chunk->stride` 必须 >= `width*4`，且
  `offset + (height-1)*stride + width*4` 不得超过 buffer `maxsize`；不满足就丢弃该帧，
  不让坏 buffer 读越界。
- **取帧**：`capture` 最多等 200ms；等到新帧用新帧，超时但有旧帧就用上一帧并 WARN，
  一帧都没有则返回 3（"PipeWire 流还没有产出首帧"）。帧尺寸与目标图不一致返回 2。
- **性能**：当前 process 回调对每一帧都做 BGRA8 转换（4K@60 下是整帧拷贝）；截图是
  低频操作，先保证正确性，后续可按需降频（见代码里的 TODO）。

### 5.3 describe / capture 的尺寸来源

`describe` 的 `width/height` **不是**门户报的尺寸，而是 PipeWire 实际协商出的
buffer 尺寸（`WaylandPipeWireStream::negotiatedSize`）。原因：门户 stream 的 `size`
属性属于 compositor 坐标，HiDPI/分数缩放下可能与像素 buffer 不同；契约要求
`describe` 与 `capture` 使用同一块缓冲，所以以真正收到的帧尺寸为准。PipeWire 协商
完成前 `start()` 会等最多 5 秒（区分"连接/协商失败"与"5 秒内没进入 STREAMING"）；
"还没有首帧"由第一次 `capture` 的 200ms 等待发现。

### 5.4 光标限制

Wayland 不允许应用读光标位图，也没有跨合成器的"把光标画进这个流"接口。本插件按
`XDP_CURSOR_MODE_EMBEDDED` 请求光标：**合成器可能把光标画进画面，也可能不画**，应用
无法检测、补画或去掉。所以：

- `include_cursor = 1`（默认）只表示"按 embedded 模式请求"，不保证画面里有光标；
- `include_cursor = 0` 在 Wayland 上**无法生效**（会话建立后改不了光标模式，插件也
  不能从帧里移除光标）；第一次 `capture` 会 WARN 一次，如实说明，不假装支持；
- 需要确定的光标行为时，请使用 Windows 或 X11 版。

### 5.5 失败分类

所有失败都返回契约的 3（参数/尺寸问题仍返回 2），**详细原因写在宿主日志里**；
`capture.failed` 的 `reason` 只带流水线按返回码生成的通用文案（`document/CAPTURE.md`
§3 的约定），这几种分类需要看日志：

| 类别 | 典型日志 | 处理建议 |
| --- | --- | --- |
| Wayland 环境自检失败（防御路径） | `当前不是 Wayland 会话…` | 正常 X11 会话已在构造时分派到 §6 的 X11 后端，不会走到这里；只有构造后环境被改才会出现 |
| 门户不可用 | `xdg-desktop-portal 服务不可用（没有运行，或桌面没有门户后端）` / `无法连接 xdg-desktop-portal` | 装/启动 `xdg-desktop-portal` 与 `xdg-desktop-portal-gnome` |
| 未授权 | `未授权（门户拒绝共享屏幕）` | 在系统弹框里允许共享 |
| 用户取消 | `用户取消了共享（门户返回取消）` | 重新发起截屏，再次弹框 |
| 超时 | `等待 Wayland 门户授权超时（120 秒）` | 重新发起并确认弹框 |
| PipeWire 失败 | `无法通过门户 fd 连接 PipeWire` / `PipeWire 流进入错误状态` / `还没有产出首帧` | 查 PipeWire 与桌面门户后端；重试 |
| 尺寸不一致 | `目标尺寸与当前 Wayland 流不一致` | 重新 describe 后再 capture（通常说明输出/缩放变了） |

### 5.6 已知限制与验证状态

| 限制 / 状态 | 说明 |
| --- | --- |
| **需真机验证** | 本实现只做代码级审查，**尚未在真实 Ubuntu 24.04 Wayland 桌面跑过**；首屏授权、点击选择、落盘尺寸、光标表现都需要维护者真机确认后再标"已验证" |
| 一次只共享一个源 | 门户默认 `multiple=false`，用户选一个输出或一个窗口；本插件不做跨输出拼接，`origin` 恒为 `(0,0)` |
| `source` 改动会重弹框 | 会话已建立后改 `source` 不能热切换：下一次截屏先结束旧会话、重新建立（`persist_mode = NONE`，必然重新弹框） |
| 光标不可控 | 见 §5.4；能否把光标画进画面取决于合成器 |
| 每次重建都会弹框 | `persist_mode = NONE`，不持久化授权；用户取消/超时后下一次截屏会重新弹 |
| 没有父窗口句柄 | 门户 `Start` 的 parent 传 `NULL`（跨平台 ABI 不带窗口），GNOME 的选择框可能不置顶到某个窗口前面 |
| 分辨率/输出切换 | 会话建立后不重新协商；帧尺寸变化时 `capture` 返回 2，下一次截屏请求会重建会话 |
| 合成器兼容性 | GNOME（mutter）与 wlroots 系是主要目标；其它门户后端的格式/尺寸行为未验证 |

## 6. Linux（X11）实现

X11 允许客户端直接读 root window：非 Wayland 会话（`WAYLAND_DISPLAY` 为空且
`XDG_SESSION_TYPE` 不是 `wayland`）由契约层在构造时分派到本后端。X11 与 Wayland
共用同一份 `libscreen_capture.so`、同一套 `ShotScreenCaptureV1` 校验与返回码；
Wayland 的门户/PipeWire 实现原样保留，两条后端只在 `ScreenCapture` 里分派。

### 6.1 依赖

```bash
sudo apt install libx11-dev libxrandr-dev libxfixes-dev libxext-dev pkg-config
```

| 依赖 | 用途 |
| --- | --- |
| `x11`（`libx11-dev`） | `XOpenDisplay` / `DisplayWidth` / `DisplayHeight` / `XGetImage` / `XDestroyImage` |
| `xext`（`libxext-dev`） | MIT-SHM 的 `XShmQueryExtension` / `XShmAttach` / `XShmGetImage`；`XShm.h` 也由它提供 |
| `xrandr`（`libxrandr-dev`） | 只在 debug 日志里用 `XRRGetMonitors` 列显示器布局（RandR 1.5+，老 server 跳过）；采集尺寸仍来自 `DisplayWidth/Height` |
| `xfixes`（`libxfixes-dev`） | `XFixesGetCursorImage` 取光标位图 + 热点，用于光标合成 |
| `pkg-config` | CMake 用 `pkg_check_modules` 找上面四个模块 |

Wayland 与 X11 的依赖都在 `UNIX AND NOT APPLE` 分支里**强制要求**：Linux 只出一份
`.so`，两种会话都要能跑；缺任何一个都在 configure 阶段 FATAL_ERROR 并打印 apt 包名。
`libxext-dev` 通常随 `libxrandr-dev` 的依赖链一起安装，但链接 `-lXext` 是 XShm 的
硬需求，所以 CMake 也单独检查 `xext`。

### 6.2 运行期选择

| 条件（任一命中） | 后端 |
| --- | --- |
| `WAYLAND_DISPLAY` 非空 | Wayland（门户 + PipeWire，见 §5） |
| `XDG_SESSION_TYPE` 大小写不敏感等于 `wayland` | Wayland |
| 其余（包括两个变量都没设置的无头环境） | X11；连不上 X server 时 `describe`/`capture` 返回 3 |

- 选择在 `ScreenCapture` 构造时做一次：会话类型在进程生命周期内不会变，之后不再
  看环境变量。
- 判定规则与 `WaylandCapture::isWaylandSession` 保持一致，改一处必须同步另一处。
- `source` 配置（Wayland 专用）在 X11 分支是空操作；`include_cursor` 在 X11 下生效。

### 6.3 describe / capture

- **describe**：`DisplayWidth(display, DefaultScreen(display))` × `DisplayHeight(...)`，
  `origin` 恒为 `(0,0)`，`pixel_format = BGRA8`。XRandR/Xinerama 把多显示器合并成
  一个 X screen，所以描述的是整个 screen；插件不做单屏/区域描述（取景由宿主裁剪）。
- **capture**：`XOpenDisplay` → `XShmGetImage`（MIT-SHM 可用且 `DISPLAY` 是本机时）
  → 失败回退 `XGetImage` → 转 BGRA8 → 可选 XFixes 光标合成 → `XDestroyImage` /
  `XCloseDisplay`。每次调用独立开/关连接，见 §6.5。
- **XShm 的异步错误**：`XShmAttach` 的错误要 `XSync` 才知道；远程 `DISPLAY`、容器
  禁用 SysV shm、权限不足都可能失败。插件在 attach + `XSync` 的短窗口里临时接管
  Xlib 错误处理器，失败后清理共享内存并回退 `XGetImage`；处理器只拦截本连接的错误，
  其它连接的错误交给上一个处理器。
- **共享内存清理**：`XShmAttach` 成功后立刻 `IPC_RMID`（attach 失败时在清理路径
  标记），`XShmDetach` + `shmdt` 后把 `XImage::data` 置空再 `XDestroyImage`
  （避免 `free()` 共享内存 / double free）。
- **像素转换**（`x11_image_convert.cpp`）：只认 `ZPixmap`；支持 24/32bpp 的
  LSBFirst/MSBFirst，按 XImage 的 red/green/blue mask 做通道映射（16bpp 与 30-bit
  深色也能覆盖）。常见 32/24bpp LSB 走整行快路径。alpha 一律置 255。每行按 XImage
  的 `bytes_per_line` 走，目标按 `dst->stride` 行寻址；8 位伪彩色没有 TrueColor
  mask，写 error 后返回 3。
- **失败语义**：与 Windows 一致 —— X11 通道失败返回 3；`describe`/`capture` 之间
  分辨率变化导致尺寸不一致返回 2；X screen 尺寸 <= 0 返回 1。
- **DPI**：X11 的 root window 像素就是物理像素，插件不调用任何缩放 API，也不像
  Windows 那样提升线程 DPI 感知上下文。

### 6.4 光标合成

`include_cursor` 默认开。`XFixesGetCursorImage` 一次拿回位图、热点与光标位置：

- 左上角 = `(cursor->x - xhot, cursor->y - yhot)`；`x/y` 是热点在 root window 上的
  位置，`xhot/yhot` 是热点在位图内的偏移（箭头光标热点是 `(0,0)`，I 型/手型不是，
  不减会整体画偏）。
- 位图是**预乘 alpha** 的 ARGB：合成公式 `out = src + dst * (1 - alpha)`；全透明
  像素跳过，全不透明像素直接覆盖；越界像素按像素裁剪。
- XFixes 扩展不可用（Xvfb、精简 server）或 `XFixesGetCursorImage` 失败时只写一条
  WARN 并跳过，抓屏仍然成功（与 Windows 取不到光标信息时一致）。

已知限制：

- X11 没有可靠的“光标当前是否可见”查询：应用或合成器隐藏了光标（`XFixesHideCursor`、
  全屏视频、屏保），XFixes 仍然返回图像，截图里可能多出一个光标。Windows 用
  `CURSOR_SHOWING` 能判断，X11 不能。
- 合成的是 `capture` 那一刻的位置；抓屏期间光标移动/变形不会追帧。

### 6.5 线程模型

`X11Capture` 没有成员状态，所有函数都是静态的；`queryGeometry` / `captureTo` 各自
`XOpenDisplay`、`XCloseDisplay`，`Display*` 绝不跨函数或跨线程保存。原因：Xlib 的
`Display` 不是线程安全的，而宿主进程里 settings_ui（GLFW）与 clipboard（X11
selection）也可能各开自己的连接；插件不共享连接，就不需要插件内的锁，也不会在插件
卸载时留下后台线程或悬挂连接。宿主采集流水线本身就串行调用同一个采集实例。Xlib 的
进程级错误处理器只在 `XShmAttach` 的短窗口内临时接管（见 §6.3），与其它 Xlib 调用
无关。插件也不调用 `XInitThreads`：它必须是进程里第一个 Xlib 调用，而 settings_ui /
clipboard 可能已经先用过 Xlib；本插件用“一次调用一个连接”规避自身并发，进程级的
Xlib 线程安全由各组件各自负责。

### 6.6 未验证与限制

| 项 | 说明 |
| --- | --- |
| **真机未验证** | 本实现只做代码级审查与语法检查，**没有在真实 X11 桌面跑过**；首次真机要确认 XShm/XGetImage 两条路径、光标位置、落盘尺寸与多帧稳定性 |
| 多屏 / 负坐标 | `DisplayWidth/Height` 是合并后的整个 X screen；多显示器布局与“负坐标”没有实测。X11 root 坐标通常从 `(0,0)` 起，负 origin 是 Windows 虚拟桌面的概念，X11 不应出现 |
| XWayland | Wayland 会话里本插件走 Wayland 后端；若环境被强制成 X11 而 `DISPLAY` 指向 XWayland，XShm/XGetImage 只能看到 **X 客户端**的窗口，Wayland 原生窗口不会进入 root pixmap（画面可能缺块/黑块） |
| 光标可见性 | 见 §6.4；X11 没有可靠的“光标当前是否显示”查询 |
| 硬件光标 | 部分驱动/全屏应用用硬件光标，XFixes 仍返回图像但屏幕上可能不显示；插件按 XFixes 图像合成 |
| 分辨率/输出热插拔 | 不监听 XRandR 事件；`describe`/`capture` 之间变化会让尺寸校验返回 2，下一次请求重新 describe |
| 多 X screen | 只抓 `DefaultScreen`；`DISPLAY=:0.1` 这类非默认 screen 未处理 |

## 7. 降级与边界情况

| 情况 | 行为 |
| --- | --- |
| 宿主没有 `log` 服务（老宿主） | `shot::Logger` 静默丢弃日志，采集照常 |
| 宿主没有列表服务 / 事件总线（老宿主） | `shot::I18n` 安静降级：语言表发布不了，配置项显示 key 本身，采集照常 |
| 语言表 JSON 非法 / 两张表 key 数不一致 | `loadTables` 只记 WARN，能解析的部分照常加载，插件不因此失败 |
| 宿主给的 `ShotCaptureInfo` 太短 | `describe` 返回 2，不越界写 |
| 宿主给的 `ShotImage` 没有 `pixel_format` 字段 | 按当年的约定当 BGRA8 处理（`STYLE.md` §6.5） |
| `pixel_format` 不是 BGRA8、`channels != 4` | `capture` 返回 2 |
| `stride` 为负 / 大于 `width*4` | 按行寻址，正确写入（Windows GDI、Wayland 帧缓冲与 X11 转换都是） |
| `describe` 与 `capture` 之间换分辨率 / 拔显示器 | 尺寸对不上 → `capture` 返回 2，绝不按错的尺寸写 |
| 没有显示器 / 远程会话断开（Windows） | 返回 1（"没有可采集的屏幕"，不是失败） |
| 宿主线程原本不是 `PER_MONITOR_AWARE_V2`（Windows） | 插件在入口临时提升，返回前恢复 |
| 系统没有 `SetThreadDpiAwarenessContext`（Windows） | 只写一条 `warn`，按线程原上下文采集 |
| 光标此刻不可见（Windows） | 不合成光标，照常返回 0 |
| `GetCursorInfo` / `GetIconInfo` / `DrawIconEx` 失败（Windows） | 只写 `warn`，抓屏照常成功 |
| X11 会话（Linux） | 走 §6 的 X11 后端；X screen 正常时采集成功 |
| 无法连接 X server / `DISPLAY` 未设置（Linux X11） | `describe`/`capture` 返回 3 + "无法打开 X11 Display…" |
| XShm 不可用 / `XShmAttach` 失败（Linux X11） | WARN 后回退 `XGetImage`，抓屏照常成功 |
| XFixes 不可用 / 取不到光标（Linux X11） | WARN 后跳过光标合成，抓屏照常成功（返回 0） |
| 用户取消 / 授权超时（Linux Wayland） | 返回 3，日志区分两种原因；下一次截屏重建会话 |
| 门户会话被撤销（Linux Wayland） | 下一次调用返回 3 并记录原因；下一次截屏重建会话 |
| PipeWire 断流 / 协商失败（Linux Wayland） | 返回 3，原因见日志；下一次截屏重建会话 |
| 200ms 内没有新帧（Linux Wayland） | 用上一帧 + 一条 WARN，返回 0 |
| 还没有任何帧（Linux Wayland） | 返回 3（"PipeWire 流还没有产出首帧"） |
| 宿主没声明 `shot.configurable`（老宿主） | 忽略该能力，`include_cursor` 按插件初值（`true`）、`source` 按 `monitor` 工作 |

### 7.1 已知限制

| 限制 | 说明 |
| --- | --- |
| **负 origin 未实机验证（Windows）** | 本机两块屏都在主屏右侧，虚拟桌面 origin 是 `(0,0)`；把副屏摆到主屏左侧/上方要改显示设置，验证时刻意不动。 |
| 虚拟桌面矩形里没有显示器覆盖的区域是黑色 | 副屏比主屏矮时它下方那块矩形没有像素来源（本机 x 2560..3584、y 768..1600）。 |
| 区域 / 单显示器采集不在本插件 | 插件永远描述并抓"整个画面"（Windows 是虚拟桌面，Wayland 是一个输出/窗口，X11 是整个 X screen）；取景由宿主负责（先整屏采集、再按 region 裁剪）。 |
| 光标只在抓屏瞬间合成（Windows/X11） | 合成的是 `capture` 调用那一刻 `GetCursorInfo` / `XFixesGetCursorImage` 报告的光标；抓屏期间光标移动/变形不会被追帧。 |
| 光标按系统默认图标尺寸绘制（Windows） | `DrawIconEx` 的宽高传 0 = 系统默认图标尺寸；用户改过超大的光标尺寸时可能略有差异。 |
| 硬件光标 / 自绘光标不能合成（Windows） | 这两类光标的形状不在系统光标位图里（`GetIconInfo` 拿不到或内容为空），只能退化为不含光标。 |
| 光标可见性（X11） | 见 §6.4：X11 没有"光标当前是否显示"的查询，被隐藏的光标仍可能被合成进截图。 |
| X11 未真机验证 | 见 §6.6；先按"需真机验证"对待，不要据此宣称 X11 的 XShm 路径、光标位置或落盘尺寸已验证。 |
| Wayland 未真机验证 | 见 §5.6；先按"需真机验证"对待，不要据此宣称 Wayland 光标/尺寸行为已验证。 |

负 origin 的复核结论（Windows）：`origin_x/origin_y` 从 `GetSystemMetrics` 到契约
`int32_t` 再到 `BitBlt` 的 `int` 源坐标全程有符号；`GetDIBits` 只认位图、与 origin
无关；行寻址用 `int64` 算 `y * stride`，负 `stride` 已通过给插件传负 stride 缓冲实测
覆盖。负坐标本身用桩测试跑过（临时验证脚本，不提交进仓库），**但没有在真实负坐标
显示器上跑过**——所以这里按未实机验证处理。

## 8. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 分配图像内存 | 契约规定宿主按 `describe` 的尺寸分配，插件只往里写：没有跨 DLL 的所有权问题 |
| 处理 / 保存像素 | 那是 `shot.image_processor` / `shot.image_saver` 插件的职责，换算法或换格式不需要改本插件 |
| 强制光标入画 | 光标是可选叠加：Windows/X11 上可关、光标隐藏或取不到图像时安静退化；Wayland 上由合成器决定，插件不越权假装能控制 |
| 区域 / 单显示器采集 | v1 契约只描述"整块画面"；取景由宿主抓整屏后裁剪，插件不感知 |
| 跨输出拼接（Linux） | Wayland 门户一次只共享一个源（输出或窗口）；X11 的 root window 本身就是多屏合并后的坐标系，不需要按输出拼接 |
| 录制 / 多路流 | v1 只需要"截一张图"；门户每次会话只开一条流，多次截屏复用同一会话 |
| 后台线程 / 异步采集（契约层） | 契约规定两个函数在宿主调用线程同步执行；Wayland 的两个后台线程只服务门户/PipeWire 自己的事件循环，X11 分支不创建线程，都不改变契约的同步语义 |
| macOS 实现 | 无真机与验证环境；CMake 在 macOS 直接跳过整个工程 |
| 链接 spdlog | 插件只能通过 `host->log`（`shot::Logger`）写日志（`STYLE.md` §6.4） |

## 9. 检查清单

- [ ] 采集入口显式 `SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2)` 了、`GetSystemMetrics` 在它之后？（Windows）
- [ ] DPI 上下文提升失败只 `warn`、没有阻止采集？返回前恢复成调用前的值了？（Windows）
- [ ] `capture` 按 `dst->stride` 逐行写、没有假设 `data` 连续？
- [ ] `stride` 为负、或大于 `width*4` 时仍然写对行？
- [ ] 每行 alpha 字节都置成 255 了？
- [ ] `describe` 与 `capture` 的尺寸不一致时返回 2、没有硬写？
- [ ] 返回码只用契约规定的 0 / 1 / 2 / 3，没有发明新码值？
- [ ] Windows：每条失败路径都逆序释放了 GDI 句柄？`GetIconInfo` 的 `hbmMask`/`hbmColor` 都 `DeleteObject` 了？
- [ ] Linux：非 Wayland 会话走 X11 后端（不是返回 3）？`DISPLAY` 打不开时返回 3 + "无法打开 X11 Display"？
- [ ] Linux/X11：`describe` 用 `DisplayWidth/DisplayHeight`、origin 恒为 `(0,0)`？
- [ ] Linux/X11：`XShmAttach` 的异步错误被 `XSync` + 临时错误处理器收住，没有让默认处理器 `exit`？
- [ ] Linux/X11：SHM 路径 `shmdt` / `XDestroyImage` 顺序正确，`image->data` 置空后才销毁？
- [ ] Linux/X11：XShm 失败或扩展不可用时回退 `XGetImage`，两条路径都能成功？
- [ ] Linux/X11：XImage 按 `bits_per_pixel` / `byte_order` / mask 转换，alpha 置 255？
- [ ] Linux/X11：光标左上角减了 `xhot/yhot`、按预乘 alpha 合成、越界裁剪？
- [ ] Linux/X11：每次 `XOpenDisplay` 都在同一次调用内 `XCloseDisplay`，Display 没有跨线程共享？
- [ ] Linux/Wayland：门户会话失败/撤销后下一次 `describe` 会重建会话、不会无限等？
- [ ] Linux/Wayland：`describe` 用的是 PipeWire 协商尺寸而不是门户 compositor 尺寸？
- [ ] Linux/Wayland：`destroy` 路径先停 PipeWire 流、再结束门户会话、最后 join 两个线程？
- [ ] Linux/Wayland：process 回调里的异常都就地吞掉（不越过 C 回调边界）、坏 buffer 有 stride/offset 防护？
- [ ] Linux/Wayland：光标限制只 WARN 一次、没有假装 `include_cursor` 生效？
- [ ] Linux/Wayland：`source` 只接受 `monitor` / `window`，改变后下一次截屏会重建会话？
- [ ] `metadata.json` 的 `interfaces` 与 `kIfaces[]` 一致（`shot.screen_capture` + `shot.configurable`）？
- [ ] 新增用户可见文案已进 `kLangTables`（`config.*` / `ui.*`），key 与 `capture_settings.cpp` 一致？
- [ ] macOS 平台是"CMake 跳过"，没有未验证的实现分支？
