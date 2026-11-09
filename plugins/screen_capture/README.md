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
│   └── screen_capture_impl.h       # 内部头：GdiDesktopCapture（GDI 平台层）
└── src/
    ├── plugin_entry.cpp            # 唯一导出符号 shot_module_entry + kEntries + ABI 胶水
    ├── screen_capture.cpp          # 契约层：入参校验、返回码映射、日志
    └── gdi_desktop_capture.cpp     # 平台层：GetDC / BitBlt / GetDIBits
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
| `user32` | `GetDC` / `ReleaseDC` / `GetSystemMetrics` |
| `gdi32` | `CreateCompatibleDC` / `CreateCompatibleBitmap` / `BitBlt` / `GetDIBits` / GDI 对象 |

产物：`<构建树>/bin/plugins/screen_capture.dll`（仓库内整体构建时是
`build/Windows/bin/plugins/screen_capture.dll`）；`metadata.json` 由 `shot_add_plugin()`
在链接后贴到 DLL 尾部，并先经过 `cmake/ValidatePluginMeta.cmake` 校验。

## 3. 能力与调用时序

本插件实现一个能力：`shot.screen_capture`（`ShotScreenCaptureV1`，版本 1）。采集是
**两段式**的，插件从不分配图像内存：

| 步骤 | 调用方 | 做什么 |
| --- | --- | --- |
| 1 | 宿主 | 调 `describe(self, out)`，`out->struct_size` 已由宿主填好 |
| 2 | 插件 | 填 `width` / `height` / `origin_x` / `origin_y` / `pixel_format = BGRA8` |
| 3 | 宿主 | 按 `width * 4 * height` 分配 BGRA8 缓冲，填好 `ShotImage`（`channels = 4`、`stride = width*4`） |
| 4 | 宿主 | 调 `capture(self, dst)` |
| 5 | 插件 | 只写像素：按 `dst->stride` 逐行写 `width*4` 字节，不碰结构体字段 |
| 6 | 宿主 | 拿图去处理 / 保存；插件不得保存 `dst` 或 `data` |

`describe` 填的几何字段（v1 只描述整个虚拟桌面）：

| 字段 | 含义 |
| --- | --- |
| `width` / `height` | 多显示器合成后的虚拟桌面像素总尺寸，有效时都 >= 1 |
| `origin_x` / `origin_y` | 内容左上角在系统坐标里的位置；主屏左上角不一定是 `(0,0)`，左侧/上方的副屏会让它变成负数 |
| `pixel_format` | 目前只有 `SHOT_PIXEL_FORMAT_BGRA8`（4 字节/像素） |

返回码（两个函数共用，`document/CAPTURE.md` §2.1 有同样的表）：

| 返回码 | 含义 |
| --- | --- |
| `0` | 成功 |
| `1` | 当前没有可采集的屏幕（无显示器 / 远程会话断开） |
| `2` | 参数不合法：`out`/`dst` 为空、`struct_size` 装不下、不是 BGRA8、`stride` 装不下一行、尺寸与当前虚拟桌面不一致 |
| `3` | 系统调用失败（已经通过宿主日志写明是哪一步与 `GetLastError()`） |

宿主对任何非 0 都发布 `capture.failed`，并且不会使用 `dst` 的内容。

## 4. 抓取流程（GDI）

`src/gdi_desktop_capture.cpp` 一次 `capture` 的完整流水线：

| # | 系统调用 | 失败时 |
| --- | --- | --- |
| 1 | `GetSystemMetrics(SM_*VIRTUALSCREEN)` | 宽或高 <= 0 → 返回 1 |
| 2 | `GetDC(NULL)` 拿屏幕 DC | 返回 3 并记日志 |
| 3 | `CreateCompatibleDC` + `CreateCompatibleBitmap` | 逆序释放已拿到的句柄，返回 3 |
| 4 | `SelectObject` 选中位图 | 释放位图与 DC，返回 3 |
| 5 | `BitBlt(SRCCOPY \| CAPTUREBLT)`，源坐标 = `(origin_x, origin_y)` | 仍继续走释放路径，返回 3 |
| 6 | `GetDIBits`（32bpp、`biHeight` 为负 = top-down、`BI_RGB`） | 返回 3 |
| 7 | 按 `dst->stride` 逐行写；`stride != width*4` 时先落临时缓冲再逐行拷 | 分配失败返回 3 |
| 8 | 每行 alpha 字节置 255 | —— |
| 9 | `DeleteObject` / `DeleteDC` / `ReleaseDC` 逆序释放 | —— |

几个容易踩的点，代码里都有对应注释：

- **负高度 DIB**：让 `GetDIBits` 读回的第一行就是屏幕最上面一行，和 `ShotImage`
  的行序一致，正向 `stride` 不用再翻转。
- **stride 不能假设连续**：`stride` 可以为负（自底向上，`data` 指向首行但地址往下走），
  也可以大于 `width*4`（行尾填充）。只有 `stride == width*4` 才让 `GetDIBits` 直接写
  `dst->data`；其它情况先落临时缓冲再逐行拷，行尾填充保持宿主原值。
- **alpha 必须置 255**：屏幕 DC 没有 alpha 通道，`GetDIBits` 的 32bpp 输出第 4 字节
  通常是 0；不置 255 保存成 PNG/WebP 会整张透明。
- **`CAPTUREBLT`**：把分层窗口（部分工具/游戏/提示框）一起抓进来；鼠标光标不在其中，
  v1 不做光标合成。
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
| 进程 DPI-unaware | `GetSystemMetrics` 与 `BitBlt` 在同一套 DWM 虚拟化坐标里，像素对得上，只是内容是放大过的逻辑像素 |
| 多显示器且主屏不在最左上 | `origin_x/origin_y` 为负，`BitBlt` 用它们当源坐标，整张虚拟桌面照常抓取 |

## 6. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 分配图像内存 | 契约规定宿主按 `describe` 的尺寸分配，插件只往里写：没有跨 DLL 的所有权问题 |
| 处理 / 保存像素 | 那是 `shot.image_processor` / `shot.image_saver` 插件的职责，换算法或换格式不需要改本插件 |
| 光标采集 | 光标是否入画因平台/场景而异，v1 先不定义，避免把错误规则固化进 ABI |
| 区域 / 单显示器采集 | v1 契约只描述整个虚拟桌面；以后用新字段或新结构体扩展，老的 `ShotCaptureInfo` 字段不动 |
| 配置项 | v1 没有可配置项：采集目标与像素格式由契约固定，加配置属于新能力 |
| 后台线程 / 异步采集 | 契约规定两个函数在宿主 main 线程同步执行；GDI 抓屏不要求 UI 线程，没必要自己开线程 |
| 非 Windows 平台实现 | 当前没有验证环境，不写未验证的编译分支；CMake 在非 Windows 直接跳过整个工程 |
| 链接 spdlog | 插件只能通过 `host->log`（`shot::Logger`）写日志（`STYLE.md` §6.4） |

## 7. 检查清单

- [ ] `capture` 按 `dst->stride` 逐行写、没有假设 `data` 连续？
- [ ] `stride` 为负、或大于 `width*4` 时仍然写对行？
- [ ] 每行 alpha 字节都置成 255 了？
- [ ] `describe` 与 `capture` 的尺寸不一致时返回 2、没有硬写？
- [ ] 返回码只用契约规定的 0 / 1 / 2 / 3，没有发明新码值？
- [ ] 每条失败路径都逆序释放了 GDI 句柄？
- [ ] Win32 类型只出现在内部头 `screen_capture_impl.h` 与平台 `.cpp` 里，公共头不含 `<windows.h>`？
- [ ] `metadata.json` 的 `interfaces` 与 `kIfaces[]` 一致（只有 `shot.screen_capture`）？
- [ ] 非 Windows 平台是"CMake 跳过"，没有未验证的实现分支？
