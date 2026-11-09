# screen_capture —— 屏幕采集插件（`shot.screen_capture`）

用 Windows GDI 抓**整个虚拟桌面**，把像素写进宿主已经分配好的 BGRA8 缓冲。
插件只负责"抓"：不处理、不保存、不分配图像内存、也没有任何配置项。宿主编排的
"采集 → 处理 → 保存"流水线见 `document/CAPTURE.md`，能力契约见
`document/ABI_POLICY.md` 的登记表。

**仅 Windows**：采集层是 GDI（`GetDC` + `BitBlt` + `GetDIBits`），非 Windows
平台本工程直接跳过（`SHOT_PLUGIN_SKIPPED`），不产出任何伪实现。

## 1. 目录

```
plugins/screen_capture/
├── CMakeLists.txt                  # shot_add_plugin + gdi32 / user32
├── metadata.json                   # 插件清单（id screen_capture.main，接口 shot.screen_capture）
├── README.md
├── include/
│   ├── plugin_entry.h              # PluginEntry / PluginInstance / state_of（插件样板）
│   ├── screen_capture.h            # ScreenCapture 公共接口（契约层，不含 Win32 类型）
│   └── screen_capture_impl.h       # 内部头：GdiDesktopCapture（GDI 平台层 + DPI 上下文）
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + kEntries + ABI 胶水
    ├── screen_capture.cpp          # 契约层：入参校验、返回码映射、日志
    └── gdi_desktop_capture.cpp     # 平台层：DPI 上下文 / GetDC / BitBlt / GetDIBits
```

公共头 `screen_capture.h` 只声明一个类，内部头 `screen_capture_impl.h` 是**内部头**
（只被本插件的 `.cpp` 包含，开头注明"不是插件对外接口"）；`.cpp` 按关注点拆成
"契约层 / 平台层 / ABI 胶水"三个文件（`STYLE.md` §6.7/§6.8）。

## 2. 构建

宿主仓库内：由根 `CMakeLists.txt` 的 `plugins/*/CMakeLists.txt` glob 自动纳入，
跟着整体构建一起产出；也可以只构建本插件：

```bash
cmake -S . -B build/Windows -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows --target screen_capture
```

离线 SDK 独立构建：

```bash
cmake -S plugins/screen_capture -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

依赖只有两个系统库，没有第三方包，插件不链接 spdlog（日志走宿主服务）：

| 依赖 | 用途 |
| --- | --- |
| `user32` | `GetDC` / `ReleaseDC` / `GetSystemMetrics` / `SetThreadDpiAwarenessContext`（运行期解析） |
| `gdi32` | `CreateCompatibleDC` / `CreateCompatibleBitmap` / `BitBlt` / `GetDIBits` / GDI 对象 |

`SetThreadDpiAwarenessContext` 用 `GetProcAddress` 运行期解析，**不静态导入**：静态导入
会让插件在 Win10 1607 之前的系统上连 DLL 都加载不起来（`STYLE.md` §6.5 的降级思路）。

产物：`<构建树>/bin/plugins/screen_capture.dll`（仓库内整体构建时是
`build/Windows/bin/plugins/screen_capture.dll`）；`metadata.json` 由 `shot_add_plugin()`
在链接后贴到 DLL 尾部，并先经过 `cmake/ValidatePluginMeta.cmake` 校验。

## 3. 能力与调用时序

本插件实现一个能力：`shot.screen_capture`（`ShotScreenCaptureV1`，版本 1）。采集是
**两段式**的，插件从不分配图像内存：

| 步骤 | 调用方 | 做什么 |
| --- | --- | --- |
| 1 | 宿主 | 调 `describe(self, out)`，`out->struct_size` 已由宿主填好 |
| 2 | 插件 | 填 `width` / `height` / `origin_x` / `origin_y` / `pixel_format = BGRA8`（物理像素） |
| 3 | 宿主 | 按 `width * 4 * height` 分配 BGRA8 缓冲，填好 `ShotImage`（`channels = 4`、`stride = width*4`） |
| 4 | 宿主 | 调 `capture(self, dst)` |
| 5 | 插件 | 只写像素：按 `dst->stride` 逐行写 `width*4` 字节，不碰结构体字段 |
| 6 | 宿主 | 拿图去处理 / 保存；插件不得保存 `dst` 或 `data` |

`describe` 填的几何字段（v1 只描述整个虚拟桌面）：

| 字段 | 含义 |
| --- | --- |
| `width` / `height` | 多显示器合成后的虚拟桌面**物理像素**总尺寸，有效时都 >= 1 |
| `origin_x` / `origin_y` | 内容左上角在系统坐标里的位置；主屏左上角不一定是 `(0,0)`，左侧/上方的副屏会让它变成负数 |
| `pixel_format` | 目前只有 `SHOT_PIXEL_FORMAT_BGRA8`（4 字节/像素） |

**DPI 保证**：几何与像素都是**物理像素**，不随 DPI 缩放。插件在每次 `describe` /
`capture` 进入平台层时，都显式把调用线程的 DPI 感知上下文临时提升到
`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`（Win10 1607+，运行期解析；设置失败只
`warn`、按原上下文继续），返回前恢复原上下文。所以**不依赖宿主进程的 DPI 感知声明**，
也不改变宿主线程后续的 DPI 语义。实测本机混合 DPI 双屏（主屏 150% 2560x1600 @0,0、
副屏 100% 1024x768 @2560,0）：DPI-unaware 线程看到的虚拟桌面是 `3584x1067`，插件报的
是物理值 `3584x1600`、origin `(0,0)`。`--capture` 实测产出 `3584x1600`：把两个色块
标记窗口分别摆到主屏 `(300,300)` 与副屏 `(2980,130)`，截图里标记的位置、尺寸、像素与
独立抓屏逐像素一致（整图差异 < 0.1%，右半与副屏差异 < 0.001%）。

返回码（两个函数共用，`document/CAPTURE.md` §2.1 有同样的表）：

| 返回码 | 含义 |
| --- | --- |
| `0` | 成功 |
| `1` | 当前没有可采集的屏幕（无显示器 / 远程会话断开） |
| `2` | 参数不合法：`out`/`dst` 为空、`struct_size` 装不下、不是 BGRA8、`stride` 装不下一行、尺寸与当前虚拟桌面不一致 |
| `3` | 系统调用失败（已经通过宿主日志写明是哪一步与 `GetLastError()`） |

宿主对任何非 0 都发布 `capture.failed`，并且不会使用 `dst` 的内容。

## 4. 抓取流程（GDI）

`src/gdi_desktop_capture.cpp` 一次完整采集的流水线（`describe` 只做第 1、2 步，第 11 步的恢复照样执行）：

| # | 系统调用 | 失败时 |
| --- | --- | --- |
| 1 | `SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2)` | 运行期解析；不可用只 `warn`，按原上下文继续 |
| 2 | `GetSystemMetrics(SM_*VIRTUALSCREEN)` | 宽或高 <= 0 → 返回 1 |
| 3 | `GetDC(NULL)` 拿屏幕 DC | 返回 3 并记日志 |
| 4 | `CreateCompatibleDC` + `CreateCompatibleBitmap` | 逆序释放已拿到的句柄，返回 3 |
| 5 | `SelectObject` 选中位图 | 释放位图与 DC，返回 3 |
| 6 | `BitBlt(SRCCOPY \| CAPTUREBLT)`，源坐标 = `(origin_x, origin_y)` | 仍继续走释放路径，返回 3 |
| 7 | `GetDIBits`（32bpp、`biHeight` 为负 = top-down、`BI_RGB`） | 返回 3 |
| 8 | 按 `dst->stride` 逐行写；`stride != width*4` 时先落临时缓冲再逐行拷 | 分配失败返回 3 |
| 9 | 每行 alpha 字节置 255 | —— |
| 10 | `DeleteObject` / `DeleteDC` / `ReleaseDC` 逆序释放 | —— |
| 11 | 恢复调用前的线程 DPI 上下文（RAII，成功/失败所有返回路径都恢复） | —— |

几个容易踩的点，代码里都有对应注释：

- **DPI 不能靠宿主**：`GetSystemMetrics` 与 `BitBlt` 必须落在同一套坐标系里，而 DPI
  awareness 是线程/进程属性。宿主可能是 DPI-unaware（没有清单、被别的宿主加载、
  以后换入口），也可能被 UI 插件顺手改成别的模式。插件自己提升、自己恢复，才能保证
  "不管谁调我，报的尺寸和抓的像素都是物理值"。混合 DPI 下 DWM 按每块屏各自的缩放
  虚拟化，DPI-unaware 线程看到的尺寸不一定是物理值（实测 `3584x1067` 对 `3584x1600`）。
- **负高度 DIB**：让 `GetDIBits` 读回的第一行就是屏幕最上面一行，和 `ShotImage`
  的行序一致，正向 `stride` 不用再翻转。
- **stride 不能假设连续**：`stride` 可以为负（自底向上，`data` 指向首行但地址往下走），
  也可以大于 `width*4`（行尾填充）。只有 `stride == width*4` 才让 `GetDIBits` 直接写
  `dst->data`；其它情况先落临时缓冲再逐行拷，行尾填充保持宿主原值。
- **alpha 必须置 255**：屏幕 DC 没有 alpha 通道，`GetDIBits` 的 32bpp 输出第 4 字节
  通常是 0；不置 255 保存成 PNG/WebP 会整张透明。
- **`CAPTUREBLT`**：把分层窗口（部分工具/游戏/提示框）一起抓进来；鼠标光标不在其中，
  v1 不做光标合成。
- **负 origin 全程有符号**：`origin_x/origin_y` 来自 `GetSystemMetrics` 的有符号返回值，
  原样存进契约的 `int32_t`，再原样作为 `BitBlt` 的 `int` 源坐标；`GetDIBits` 只认位图、
  与 origin 无关；行寻址用 `int64` 算 `y * stride`。所以副屏在主屏左侧/上方时这条链
  不会把负号丢掉或当成无符号大数。**这套路径未实机验证**（本机两块屏都在主屏右侧，
  见 §5.1）。
- **句柄生命周期**：任何一步失败都走同一条逆序释放路径，不靠 GC、也不把句柄挂到
  实例上——每次 `capture` 都是完整的一次生命周期。

## 5. 降级与边界情况

| 情况 | 行为 |
| --- | --- |
| 宿主没有 `log` 服务（老宿主） | `shot::Logger` 静默丢弃日志，采集照常 |
| 宿主给的 `ShotCaptureInfo` 太短 | `describe` 返回 2，不越界写 |
| 宿主给的 `ShotImage` 没有 `pixel_format` 字段 | 按当年的约定当 BGRA8 处理（`STYLE.md` §6.5） |
| `pixel_format` 不是 BGRA8、`channels != 4` | `capture` 返回 2 |
| `stride` 为负 / 大于 `width*4` | 按行寻址，正确写入（见 §4） |
| `describe` 与 `capture` 之间换分辨率 / 拔显示器 | 尺寸对不上 → `capture` 返回 2，绝不按错的尺寸写 |
| 远程会话断开 / 没有显示器 | 返回 1（"没有可采集的屏幕"，不是失败） |
| 宿主线程原本不是 `PER_MONITOR_AWARE_V2` | 插件在入口临时提升，返回前恢复；宿主线程后续的 `GetSystemMetrics` / 建窗口语义不变 |
| 系统没有 `SetThreadDpiAwarenessContext` / PMv2 不被接受 | 只写一条 `warn`，按线程原上下文采集；混合 DPI 下尺寸可能退化到虚拟化坐标 |
| 多显示器且主屏不在最左上（`origin` 为负） | 代码按有符号处理（见 §4）；**未实机验证**（见 §5.1） |

### 5.1 已知限制

| 限制 | 说明 |
| --- | --- |
| **负 origin 未实机验证** | 本机两块屏都在主屏右侧，虚拟桌面 origin 是 `(0,0)`；把副屏摆到主屏左侧/上方要改显示设置，验证时刻意不动。 |
| 虚拟桌面矩形里没有显示器覆盖的区域是黑色 | 副屏比主屏矮时它下方那块矩形没有像素来源（本机 x 2560..3584、y 768..1600）。 |
| 区域 / 单显示器采集不在本插件 | 插件永远描述并抓整个虚拟桌面；取景由宿主负责（先整屏采集、再按 region 裁剪）。 |
| 光标不合成 | `BitBlt` / `CAPTUREBLT` 都不含鼠标光标；v1 契约不描述光标位置。 |

负 origin 的复核结论：`origin_x/origin_y` 从 `GetSystemMetrics` 到契约 `int32_t` 再到
`BitBlt` 的 `int` 源坐标全程有符号；`GetDIBits` 只认位图、与 origin 无关；行寻址用
`int64` 算 `y * stride`，负 `stride` 已通过给插件传负 stride 缓冲实测覆盖。负坐标本身用"建模虚拟桌面
surface + 负 viewport origin"的桩测试跑过（临时验证脚本，不提交进仓库），**但没有在
真实负坐标显示器上跑过**——所以这里按未实机验证处理。

## 6. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 分配图像内存 | 契约规定宿主按 `describe` 的尺寸分配，插件只往里写：没有跨 DLL 的所有权问题 |
| 处理 / 保存像素 | 那是 `shot.image_processor` / `shot.image_saver` 插件的职责，换算法或换格式不需要改本插件 |
| 光标采集 | 光标是否入画因平台/场景而异，v1 先不定义，避免把错误规则固化进 ABI |
| 区域 / 单显示器采集 | v1 契约只描述整个虚拟桌面；取景由宿主抓整屏后裁剪，插件不感知（以后要原生加速再用新字段或新结构体扩展，老的 `ShotCaptureInfo` 字段不动） |
| 配置项 | v1 没有可配置项：采集目标与像素格式由契约固定，加配置属于新能力 |
| 后台线程 / 异步采集 | 契约规定两个函数在宿主调用线程同步执行；GDI 抓屏不要求 UI 线程，没必要自己开线程 |
| 非 Windows 平台实现 | 当前没有验证环境，不写未验证的编译分支；CMake 在非 Windows 直接跳过整个工程 |
| 链接 spdlog | 插件只能通过 `host->log`（`shot::Logger`）写日志（`STYLE.md` §6.4） |

## 7. 检查清单

- [ ] 采集入口显式 `SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2)` 了、`GetSystemMetrics` 在它之后？
- [ ] DPI 上下文提升失败只 `warn`、没有阻止采集？返回前恢复成调用前的值了？
- [ ] `capture` 按 `dst->stride` 逐行写、没有假设 `data` 连续？
- [ ] `stride` 为负、或大于 `width*4` 时仍然写对行？
- [ ] 每行 alpha 字节都置成 255 了？
- [ ] `describe` 与 `capture` 的尺寸不一致时返回 2、没有硬写？
- [ ] `origin_x/origin_y` 全程保持有符号（`int32_t` → `BitBlt` 的 `int`），没有转成无符号？
- [ ] 返回码只用契约规定的 0 / 1 / 2 / 3，没有发明新码值？
- [ ] 每条失败路径都逆序释放了 GDI 句柄？
- [ ] Win32 类型只出现在内部头 `screen_capture_impl.h` 与平台 `.cpp` 里，公共头不含 `<windows.h>`？
- [ ] `metadata.json` 的 `interfaces` 与 `kIfaces[]` 一致（只有 `shot.screen_capture`）？
- [ ] 非 Windows 平台是"CMake 跳过"，没有未验证的实现分支？
