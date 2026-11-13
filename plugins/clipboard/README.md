# clipboard —— 截屏后把图片写进系统剪贴板

一个**纯消费方**插件：自己不采集、不保存、不碰流水线，只订阅宿主在落盘后发布的
`capture.done`，把保存插件写好的图片文件读回内存，再按平台写系统剪贴板：

- **Windows**：Win32 剪贴板，`CF_DIB`（可选 `CF_HDROP` 文件列表）；
- **Linux**：拥有 X11 的 `CLIPBOARD` selection，按 `SelectionRequest` 提供
  `image/png`（可选 `text/uri-list` 文件列表）。桌面跑 Wayland 时通过 XWayland
  桥接，原生 Wayland 应用通常也能粘贴；**纯 Wayland 且没有 XWayland 时不可用**
  （插件 WARN 一次后跳过）。

用户截完图可以直接 `Ctrl+V` 粘贴到聊天/文档里，不用先去文件夹找文件。

插件 id 是 `clipboard.main`，只实现一个能力：`shot.configurable`（声明并读写两个
配置项）。它没有 `ui_provider`；Windows 上没有窗口和线程，Linux/X11 上有一个
专属的 X11 selection 线程（见 §8）。

## 1. 目录

```
plugins/clipboard/
├── CMakeLists.txt              # 平台源文件/库选择 + shot_add_plugin + stb/nlohmann_json
├── metadata.json               # 构建期贴到 DLL 尾部：id / abi 2.0 / interfaces
├── include/                    # 头文件与 .cpp 一一对应，一个类一个头（STYLE.md §6.8）
│   ├── plugin_entry.h          #   内部头：PluginEntry / PluginInstance / state_of
│   ├── clipboard_plugin.h      #   ClipboardPlugin：capture.done 订阅 + 配置转发
│   ├── clipboard_config.h      #   ClipboardConfig：enabled / also_file_drop 两个配置项
│   ├── clipboard_writer.h      #   ClipboardWriter：读图 → CF_DIB（+ CF_HDROP），仅 Windows
│   ├── clipboard_image.h       #   ClipboardImage：图片文件 → PNG + text/uri-list，仅 Linux
│   ├── clipboard_x11.h         #   ClipboardX11：拥有 CLIPBOARD selection 的公共接口，仅 Linux
│   └── clipboard_x11_impl.h    #   内部头：ClipboardX11::Impl / X11 线程与协议状态，仅 Linux
└── src/
    ├── plugin_entry.cpp        # 唯一导出符号 shot_module_entry + ABI 胶水（含订阅的建立/退订）
    ├── clipboard_plugin.cpp    # 事件订阅、语言表、capture.done 负载解析、调用写入器
    ├── clipboard_config.cpp    # 配置描述符（label/tooltip 写语言表 key）+ get/set
    ├── clipboard_writer.cpp    # stb_image 解码 + Win32 剪贴板写入（Win32 类型只在这个 TU 里）
    ├── clipboard_image.cpp     # stb_image 解码 + stb_image_write 编码 PNG + URI 列表（Linux）
    ├── clipboard_x11.cpp       # X11 线程生命周期 / 事件循环 / 取得所有权 / TARGETS 自检
    └── clipboard_x11_selection.cpp  # SelectionRequest / TARGETS / TIMESTAMP / MULTIPLE 应答
```

## 2. 构建

宿主仓库内：由根 `CMakeLists.txt` 的 `file(GLOB plugins/*/CMakeLists.txt)` 自动
`add_subdirectory`，跟着整个工程一起构建；新增插件不需要改根构建脚本。

```bash
# Windows
cmake --build build/Windows --target clipboard
# Linux（X11）
cmake --build build/Linux --target clipboard
```

离线 SDK 独立构建：

```bash
cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

产物：`build/Windows/bin/plugins/clipboard.dll` 或
`build/Linux/bin/plugins/clipboard.so`（导入库在 `build/lib`，pdb 在
`build/pdb/plugins`）。

| 平台 | 依赖 | 系统包 |
| --- | --- | --- |
| Windows | `stb`（解码）、`nlohmann_json`（事件负载）、`user32` / `shell32` | 无（系统库） |
| Linux | `stb`（解码 + PNG 编码）、`nlohmann_json`、`libX11` | `libx11-dev`（CMake 用 `find_package(X11 REQUIRED)` 找） |
| macOS | —— | 跳过整个插件（`NSPasteboard` 待补） |

`stb` / `nlohmann_json` 由根工程的 vcpkg 清单提供，插件不新增运行时 DLL。

## 3. 事件驱动时序（为什么依赖保存插件）

Windows 路径：

```
capture.request ─► 采集 ─► 处理 ─► 保存（image_saver 落盘）─► capture.done
                                                              │  {path,width,height,processors}
                                                              ▼
                                             clipboard.main 订阅回调（宿主 main 线程）
                                                              │  1. 读 path 指向的文件
                                                              │  2. stb_image 解码成 RGBA
                                                              │  3. 转 CF_DIB（32bpp BGRA, bottom-up）
                                                              ▼
                                                     OpenClipboard → SetClipboardData
```

Linux 路径：

```
capture.done ─► 订阅回调（宿主 main 线程）：stb 解码 + stb_image_write 编码 image/png
              │                                    （可选生成 text/uri-list）
              ▼
       互斥队列 + 唤醒管道（只保留最新一张）
              ▼
       专属 X11 线程：XSetSelectionOwner(CLIPBOARD) → 自检 TARGETS → 事件循环应答粘贴
              ▼
       其它程序发 SelectionRequest → 写请求方 property + SelectionNotify
```

- 负载格式与流水线语义见 `document/CAPTURE.md` §3：`path` 是保存插件写回的最终
  文件路径，像素本身**不进事件总线**，所以本插件必须从文件读图。
- 因此它是**软依赖**：没有可用的保存插件时根本不会有 `capture.done`，本插件安静
  不工作；`path` 为空（保存成功但插件没写回路径）时只记 WARN 并跳过。
- 时序上 `capture.done` 只在文件落盘**之后**才发布，所以读到的一定是完整文件。
- 两种运行方式都会把事件派发给订阅者：常驻模式由宿主主循环每轮调用
  `dispatchEvents()`；`--capture` 一次性模式在采集完成、退出之前也会主动派发一轮
  事件队列（宿主实现见 `src/main.cpp`）。所以 `--capture` 跑完后剪贴板里同样有图。
- X11 的 selection 是**拉模型**：只有别的程序粘贴时才会来要数据。X11 线程必须活到
  下一次截图替换、其它程序抢走所有权（`SelectionClear`）或插件卸载；这也是 Linux
  唯一需要后台线程的原因。

## 4. 配置项（`shot.configurable`）

| key | kind | 默认值 | 取值 | 说明 |
| --- | --- | --- | --- | --- |
| `enabled` | BOOL | `1` | `1` / `0` | 截屏落盘后是否写进剪贴板；关掉后只记 debug、不做任何剪贴板操作 |
| `also_file_drop` | BOOL | `0` | `1` / `0` | 是否额外提供文件列表：Windows `CF_HDROP`，Linux `text/uri-list`；不勾选则只发图片 |

配置值一律是字符串（契约要求），`"0"` / `"false"` 视为关、其余视为开；不认识的 key
返回负值、插件保持可用。存盘键是 `clipboard.main/<key>`，落盘与启动回灌归宿主
（`document/SETTINGS.md`），插件只负责 get/set。

`label` / `tooltip` 不写中文，写的是语言表 key（文案本体见 §5）：

| 配置 key | label key | tooltip key |
| --- | --- | --- |
| `enabled` | `config.enabled.label` | `config.enabled.tooltip` |
| `also_file_drop` | `config.also_file_drop.label` | `config.also_file_drop.tooltip` |

> 语言表是两端共用的，`also_file_drop.tooltip` 沿用了 Windows 的 `CF_HDROP` 说法；
> 在 Linux 上它实际提供的是 `text/uri-list`，语义都是“把截图当成一个文件拖/贴”。

## 5. 多语言（i18n）

> 语言表同时包含插件自身的 `plugin.name` / `plugin.description`：设置页的插件分区标题按
> `display(插件 id, "plugin.name")` 解析（查不到时回退成 key 本身）。

用户可见文案走 `shot::I18n`（`include/shot_plugin_helpers/shot_i18n.h`）。这条链路
零 ABI 改动：插件用 `lists.add` 把自己的语言表发布到 `i18n_source_list`，宿主汇总后
生成只读的 `i18n_list`，所有消费方按同一条规则解析：

```text
display(owner, key) = i18n_list[(owner, key)] ?? key
```

- **语言表位置**：`src/clipboard_plugin.cpp` 文件作用域的 `kLangTables[]`，每种语言
  一个 JSON 对象，`@meta` 是 BCP-47 语言标签（当前 `zh-CN` / `en-US`）。两张表的
  key 集必须完全一致，每个 key 两种语言都要有文本。
- **加载时机**：`ClipboardPlugin::start(host)` 里构造 `std::unique_ptr<shot::I18n>`
  并调用 `loadTables(kLangTables, std::size(kLangTables))`；`stop()`（`destroy`
  路径）里 `reset()`，`I18n` 析构会退订 `lists.changed`。owner 用插件身份证
  `kClipboard.id`（`clipboard.main`），与 `metadata.json` 一致。
- **key 规范**：
  - 配置项：`config.<配置 key>.label` / `config.<配置 key>.tooltip`；
  - 插件自己的 UI 文案：`ui.*`（本插件没有窗口，因此暂无 `ui.*` key）。
- **回退**：宿主拿不到语言表时（老宿主没有列表服务、语言表解析失败），设置页按
  上面的规则回退显示 key 本身；`tr(key, fallback)` 的 `fallback` 必须写中文原文，
  保证缺表时界面仍是可读的中文而不是空串。
- **不翻译的东西**：日志（`shot::Logger`）保持中文——e2e 按中文日志断言；事件主题
  `capture.done`、配置 key `enabled`、`CF_DIB` / `CF_HDROP` / `image/png` /
  `text/uri-list` 这类技术名词也不翻译。
- **新增一条文案**：在两张语言表里同时加同一个 key，再用 key 引用；只加一种语言时
  `loadTables` 会记 WARN（各语言 key 数不一致），缺文本的语言按回退链取。

本插件没有自己的 UI（没有 `ui_provider`），用户可见文案只有两个配置项的
label/tooltip，所以没有 `i18n.tr(...)` 调用点；剪贴板写入失败的原因只进中文日志。

## 6. 剪贴板格式

### 6.1 CF_DIB（Windows，总是写）

- `BITMAPINFOHEADER` + 像素，`biBitCount = 32`、`biCompression = BI_RGB`；
- 像素顺序 **BGRA**、行序 **bottom-up**（`biHeight` 为正数）：这是 CF_DIB 的通用
  惯例，Office / 聊天软件 / 画图都按它解释；
- 输入格式由 stb_image 决定（保存插件实际落盘的 PNG / JPEG 都支持）。

### 6.2 CF_HDROP（Windows，`also_file_drop = 1` 时写）

- `DROPFILES` 头 + UTF-16 宽字符路径 + 双 NUL 结尾（`fWide = TRUE`）；
- 路径是保存插件报告的**已经存在的文件**；粘贴到资源管理器时系统会复制该文件，
  所以要求截图文件还没有被用户删掉；
- 部分程序粘贴时优先用文件列表而不是图片；不需要文件时把 `also_file_drop` 关掉。

### 6.3 image/png（Linux，总是提供）

- 用 stb_image 把文件解码成 RGBA，再用 stb_image_write 编码成 PNG，整张图的字节
  缓存在插件里；每次粘贴都复用，不在 SelectionRequest 里重新编码；
- 文件读失败、解码失败、编码失败都只影响这一次写入（截图文件已落盘），下一次
  `capture.done` 会重试；
- 文件超过 512 MiB 时拒绝读取，避免事件里的异常路径把内存吃爆。

### 6.4 text/uri-list（Linux，`also_file_drop = 1` 时提供）

- RFC 2483 的 URI 列表；本插件只放一行 `file:///...`，以 `\r\n` 结尾；
- 路径按 RFC 3986 做百分号编码：空格、中文（UTF-8 多字节）、`#`、`?` 等都不会被
  接收方误当成 URI 的分隔符；
- 等价于 Windows 的 `CF_HDROP`：文件管理器可以把它当作“拖一个文件”处理。

### 6.5 X11 selection 协议与 TARGETS 自检

ICCCM 的流程（简化）：

1. owner 用 `XSetSelectionOwner(CLIPBOARD, window, timestamp)` 宣称拥有剪贴板；
2. 请求方给自己窗口选一个 property，发 `SelectionRequest` 到 owner；
3. owner 把数据 `XChangeProperty` 到请求方的 property，再回 `SelectionNotify`；
4. 请求方收到 `SelectionNotify` 后读取 property。

本插件提供的 target：

| target | 类型 | 内容 |
| --- | --- | --- |
| `TARGETS` | `XA_ATOM` | 本插件支持的全部 target 列表 |
| `TIMESTAMP` | `XA_INTEGER` | 取得所有权时的服务器时间（不是墙钟） |
| `MULTIPLE` | `XA_ATOM` | 一次请求多个 target 的批量探测（剪贴板管理器会用） |
| `image/png` | `image/png` | PNG 字节 |
| `text/uri-list` | `text/uri-list` | 文件 URI 列表（`also_file_drop=1` 才有） |

**为什么取得所有权后还要等 `SelectionNotify`**：`XSetSelectionOwner` /
`XGetSelectionOwner` 只证明 X server 记下了 owner，不证明数据链路真的可用。
剪贴板管理器（Klipper、clipman、桌面自带的管理器）会在 owner 变化后立刻发
`TARGETS` 或数据请求；如果 owner 还没进事件循环就宣称成功，第一个请求会丢。
所以本插件在 claim 之后向自己发一次 `XConvertSelection(CLIPBOARD → TARGETS)`，
把收到 `SelectionNotify`（property 有效）当作“selection 真正就绪”的日志标志；
自检失败只 WARN，数据仍在内存里，下一次截图会重新 claim。

**SelectionClear**：别的程序（用户复制了文字 / 剪贴板管理器接管）取得所有权时，
X server 给本插件发 `SelectionClear`；插件立刻清空缓存的图片字节并停止应答。
之后粘贴得到的是新 owner 的数据，而不是本插件的旧图。

## 7. 失败降级

剪贴板是进程间共享资源，失败是常态，不能影响截屏本身：

| 情形 | 行为 |
| --- | --- |
| `enabled = 0` | 跳过，只记 DEBUG，不刷 WARN |
| `capture.done` 负载非法 / 缺 `path` / `path` 为空 | WARN 一行后跳过，等下一条事件 |
| 文件打不开、为空、超过 512 MiB | WARN（含路径）后跳过 |
| stb_image 解码失败 | WARN（含 stb 的失败原因）后跳过 |
| Windows：剪贴板被其它程序占用 | `OpenClipboard` 重试 10 次 × 20ms；仍失败记 WARN 后跳过 |
| Windows：`SetClipboardData(CF_DIB)` 失败 | 释放全局内存（所有权没转移）、WARN 后跳过 |
| Windows：图片已写入但 CF_HDROP 失败 | WARN 说明“图片已写入、文件列表失败”，图片粘贴仍可用 |
| Linux：没有 X11 Display（纯 Wayland / `DISPLAY` 未设置） | `start()` 记一条 WARN 后跳过；之后每条 `capture.done` 只记 DEBUG，不读图/不编码 |
| Linux：`XSetSelectionOwner` 失败（被其它程序抢先） | WARN，释放本次负载；下一次 `capture.done` 重试 |
| Linux：TARGETS 自检没收到有效的 `SelectionNotify` | WARN；数据保留，下一次截图重新 claim |
| Linux：请求方窗口中途销毁（BadWindow） | 临时接管 Xlib 错误处理器并 `XSync` 消费错误，不影响宿主（见 §8） |

任何失败都只影响这一次的剪贴板复制：截图文件已经在盘上，下一次 `capture.done`
会重新尝试；宿主不会因为本插件失败而报错或退出。

## 8. 线程与生命周期

**Windows**：没有自己的线程、窗口和后台任务。`capture.done` 的订阅回调由宿主事件
分发线程（main）执行，stb 解码与剪贴板写入也在这个回调里同步完成。

**Linux/X11**：订阅回调仍在宿主 main 线程，只做读图 + PNG/URI 编码；X11 的
selection 必须常驻应答，所以 `ClipboardX11` 起一个专属线程：

```
start(host)
  └─ pipe() 唤醒管道 + std::thread
       └─ XOpenDisplay → 创建隐藏 owner 窗口 → 注册 atom
            → 握手（ready/failed）→ poll(X 连接 + 唤醒管道) 事件循环
capture.done
  └─ 主线程：ClipboardImage::build（stb 解码/编码）
       └─ commandMutex 下覆盖 pending（只保留最新一张）→ 写唤醒管道
X11 线程
  └─ 取 pending → serverTime() → XSetSelectionOwner → 自检 TARGETS
       └─ SelectionRequest → 写 property + SelectionNotify
       └─ SelectionClear → 清空 payload
stop()
  └─ 拒绝新负载 → 唤醒 → join → 线程里 releaseSelection + 关 Display
```

- **为什么 Xlib 只在一个线程里调用**：Xlib 的 `Display` 不是可重入对象；宿主里还
  可能有 GLFW/settings_ui 在使用 X11。把本插件的连接、窗口、atom 全部关在 X11
  线程里，至少保证插件内部没有并发 Xlib 调用（跨组件的风险见 §9）。
- **为什么用唤醒管道而不是条件变量**：X11 线程要同时等“X 事件”和“主线程投递的
  新图”，`poll()` 能一次等两个 fd，空闲不占 CPU；管道满说明还有未处理的唤醒，
  不必再写。
- **stop 顺序**：先拒绝新负载 → `running=false` → 写唤醒管道 → join。线程退出前
  主动 `XSetSelectionOwner(..., None, CurrentTime)` 放弃所有权，再销毁 owner 窗口
  和 `XCloseDisplay`。`destroy` 返回后宿主 FreeLibrary，不会留下指向本 DLL 的
  Xlib 回调。
- **BadWindow 保护**：请求方可能在应答前关闭窗口；Xlib 默认错误处理器遇到
  `BadWindow` 会 `exit(1)`。本插件只在处理一条 `SelectionRequest` 的短临界区里
  临时接管错误处理器，并用 `XSync` 把错误消费掉再恢复，不能因此杀掉宿主（也不会
  长期替换进程级处理器，避免和 GLFW/settings_ui 抢）。
- **配置线程**：`set_config` 可能来自设置页线程，而读取发生在事件分发线程；两个
  开关用 `std::atomic<bool>` 保存，读取侧不加锁、不会读到撕裂的值。
- **语言表**：`start()` 里构造 `shot::I18n` 并发布，`stop()` 里销毁（析构自动退订
  `lists.changed`），`stop()` 重复调用是空操作。

## 9. 平台限制

### 9.1 Windows

- 用 `OpenClipboard` / `SetClipboardData` / `CF_DIB` / `CF_HDROP` 这套 Win32 API；
- 32 位/64 位进程都能写剪贴板，但 32 位进程写的 CF_DIB 与 64 位进程之间由系统
  做格式转换；本插件只按标准 CF_DIB 写，不依赖位深。

### 9.2 Linux（X11 / XWayland）

- 本插件是 **X11 客户端**，不是 Wayland 原生客户端：它打开 `$DISPLAY` 指向的
  X server，在 X11 协议里拥有 `CLIPBOARD`。在 Wayland 桌面（GNOME/KDE）上这个
  X server 通常是 **XWayland**，它负责把 X11 selection 桥接给合成器；原生 Wayland
  应用粘贴时，最终由合成器向 XWayland 要数据，再转给 `image/png`。
- **纯 Wayland 且没有 XWayland**（例如精简的 Sway 配置、`DISPLAY` 未设置）：
  `XOpenDisplay` 返回空，插件记一条 WARN 后跳过剪贴板功能，截屏与保存不受影响。
- 没有实现 Wayland 原生的 `wl_data_device` / `wlr-data-control` 协议；后台进程写
  剪贴板需要合成器/扩展授权，不是所有桌面都提供。
- **数据随 owner 存活**：X11 selection 的字节在插件进程里；宿主退出或插件卸载后
  数据就没了。剪贴板管理器如果已经请求并缓存了内容，用户仍可能粘贴到旧图；否则
  剪贴板会变空。这不是 bug，是 X11 selection 的语义。
- 剪贴板管理器（Klipper/clipman 等）可能在同一时刻也 claim CLIPBOARD；本插件
  用 `XGetSelectionOwner` 确认自己仍是 owner，抢不到就等下一次截图。
- 其它 X11 程序的 `TARGETS` 探测会走本插件的 `TARGETS` 应答；`MULTIPLE` 也支持，
  避免被当成“不完整的 owner”。

### 9.3 macOS

CMake 直接跳过整个插件，没有 `NSPasteboard` 实现；补齐位置在 `CMakeLists.txt` 的
`// TODO:` 注释里。

## 10. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 自己采集/保存/编码 | 那是 `screen_capture` / `image_saver` 的职责；本插件只消费 `capture.done` |
| 订阅 `capture.request` 触发采集 | 会变成第二个“截屏入口”，和托盘/命令行抢职责，也违反事件总线单向消费的约定 |
| 从事件负载拿像素 | 像素不进事件总线（`document/CAPTURE.md` §3），只传路径 |
| 长期替换进程级 Xlib 错误处理器 | 会和 GLFW/settings_ui 的 X11 使用互相影响；只在单条请求的短临界区内临时接管 |
| 自己实现 Wayland 原生剪贴板 | 普通客户端只能在自身窗口有焦点时写 selection；后台写剪贴板依赖合成器扩展，本插件不假设它存在 |
| 把图片转成 HTML / RTF / BMP 等多种剪贴板格式 | v1 只承诺“能粘贴图片”和“能当文件”；加格式需要额外的编码器和兼容性验证，等有明确需求再说 |
| 把配置项写进 `metadata.json` | 真实 schema 在 `shot.configurable` 里，写两遍必然漂移（`document/METADATA.md` §3） |
| 自己写配置文件 | 存盘键是 `<plugin_id>/<key>`，落盘与启动回灌归宿主（`document/SETTINGS.md`） |

## 11. 检查清单

- [ ] 改配置项只改 `clipboard_config.cpp` 的 `kConfig[]` 与 `ClipboardConfig`，没有改宿主代码？
- [ ] `metadata.json` 的 `plugins[]` 顺序仍与 `kEntries` 一致、`interfaces` 与代码一致？
- [ ] Windows 剪贴板操作前 `OpenClipboard` 有重试、每条失败路径都 `CloseClipboard` / `GlobalFree` 了吗？
- [ ] CF_DIB 是 32bpp BGRA + bottom-up 吗？`biSizeImage` / `biSize` 填对了吗？
- [ ] Linux 的 TARGETS 列表包含 `TARGETS` / `TIMESTAMP` / `MULTIPLE` / `image/png`（以及按需的 `text/uri-list`）吗？
- [ ] `SelectionRequest` 的 property 写到了**请求方**窗口，并回了 `SelectionNotify` 吗？
- [ ] `SelectionClear` 后清空 payload、停止应答了吗？
- [ ] 无 X11 Display 时只有一条 WARN，之后事件直接跳过、不读图/不编码吗？
- [ ] 失败路径都收敛成返回码 / WARN 了吗？异常有没有跨出 `extern "C"`？
- [ ] `destroy` 前退订了 `capture.done`、停了 X11 线程、销毁了语言表吗？插件有没有留下线程或窗口？
- [ ] 改配置项文案时两张语言表的 key 集仍一致、每 key 两种语言都有文本、fallback 是中文原文吗？
- [ ] 公共头里的类/函数有 Doxygen 注释吗？未完成项写成代码里的 `// TODO:` 了吗？
