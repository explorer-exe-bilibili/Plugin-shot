# clipboard —— 截屏后把图片写进系统剪贴板

一个**纯消费方**插件：自己不采集、不保存、不碰流水线，只订阅宿主在落盘后发布的
`capture.done`，把保存插件写好的图片文件读回内存，转成 Win32 剪贴板格式放进系统
剪贴板。用户截完图可以直接 `Ctrl+V` 粘贴到聊天/文档里，不用先去文件夹找文件。

插件 id 是 `clipboard.main`，只实现一个能力：`shot.configurable`（声明并读写两个
配置项）。它没有 `ui_provider`，也没有窗口、线程和后台任务。

## 1. 目录

```
plugins/clipboard/
├── CMakeLists.txt              # 非 Windows 跳过 + shot_add_plugin + stb/nlohmann_json/user32/shell32
├── metadata.json               # 构建期贴到 DLL 尾部：id / abi 2.0 / interfaces
├── include/                    # 头文件与 .cpp 一一对应，一个类一个头（STYLE.md §6.8）
│   ├── plugin_entry.h          #   内部头：PluginEntry / PluginInstance / state_of
│   ├── clipboard_plugin.h      #   ClipboardPlugin：capture.done 订阅 + 配置转发
│   ├── clipboard_config.h      #   ClipboardConfig：enabled / also_file_drop 两个配置项
│   └── clipboard_writer.h      #   ClipboardWriter：读图 → CF_DIB（+ CF_HDROP）
└── src/
    ├── plugin_entry.cpp        # 唯一导出符号 shot_module_entry + ABI 胶水（含订阅的建立/退订）
    ├── clipboard_plugin.cpp    # 事件订阅、语言表、capture.done 负载解析、调用写入器
    ├── clipboard_config.cpp    # 配置描述符（label/tooltip 写语言表 key）+ get/set
    └── clipboard_writer.cpp    # stb_image 解实现 + Win32 剪贴板写入（Win32 类型只在这个 TU 里）
```

## 2. 构建

宿主仓库内：由根 `CMakeLists.txt` 的 `file(GLOB plugins/*/CMakeLists.txt)` 自动
`add_subdirectory`，跟着整个工程一起构建；新增插件不需要改根构建脚本。

```bash
cmake --build build/Windows --target clipboard
```

离线 SDK 独立构建：

```bash
cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

产物：`build/Windows/bin/plugins/clipboard.dll`（导入库在 `build/lib`，pdb 在
`build/pdb/plugins`）。依赖都是 header-only 或系统库：`stb`（解码）、
`nlohmann_json`（解析事件负载）、`user32` / `shell32`（剪贴板 API），不新增运行时
依赖。

## 3. 事件驱动时序（为什么依赖保存插件）

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

- 负载格式与流水线语义见 `document/CAPTURE.md` §3：`path` 是保存插件写回的最终
  文件路径，像素本身**不进事件总线**，所以本插件必须从文件读图。
- 因此它是**软依赖**：没有可用的保存插件时根本不会有 `capture.done`，本插件安静
  不工作；`path` 为空（保存成功但插件没写回路径）时只记 WARN 并跳过。
- 时序上 `capture.done` 只在文件落盘**之后**才发布，所以读到的一定是完整文件。
- 两种运行方式都会把事件派发给订阅者：常驻模式由宿主主循环每轮调用
  `dispatchEvents()`；`--capture` 一次性模式在采集完成、退出之前也会主动派发一轮
  事件队列（宿主实现见 `src/main.cpp`）。所以 `--capture` 跑完后剪贴板里同样有图。

## 4. 配置项（`shot.configurable`）

| key | kind | 默认值 | 取值 | 说明 |
| --- | --- | --- | --- | --- |
| `enabled` | BOOL | `1` | `1` / `0` | 截屏落盘后是否写进剪贴板；关掉后只记 debug、不做任何剪贴板操作 |
| `also_file_drop` | BOOL | `0` | `1` / `0` | 是否额外提供 CF_HDROP 文件列表；勾选后资源管理器里可粘贴成文件 |

配置值一律是字符串（契约要求），`"0"` / `"false"` 视为关、其余视为开；不认识的 key
返回负值、插件保持可用。存盘键是 `clipboard.main/<key>`，落盘与启动回灌归宿主
（`document/SETTINGS.md`），插件只负责 get/set。

`label` / `tooltip` 不写中文，写的是语言表 key（文案本体见 §5）：

| 配置 key | label key | tooltip key |
| --- | --- | --- |
| `enabled` | `config.enabled.label` | `config.enabled.tooltip` |
| `also_file_drop` | `config.also_file_drop.label` | `config.also_file_drop.tooltip` |

## 5. 多语言（i18n）

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
  `capture.done`、配置 key `enabled`、`CF_DIB` / `CF_HDROP` 这类技术名词也不翻译。
- **新增一条文案**：在两张语言表里同时加同一个 key，再用 key 引用；只加一种语言时
  `loadTables` 会记 WARN（各语言 key 数不一致），缺文本的语言按回退链取。

本插件没有自己的 UI（没有 `ui_provider`），用户可见文案只有两个配置项的
label/tooltip，所以没有 `i18n.tr(...)` 调用点；剪贴板写入失败的原因只进中文日志。

## 6. 剪贴板格式

### 6.1 CF_DIB（总是写）

- `BITMAPINFOHEADER` + 像素，`biBitCount = 32`、`biCompression = BI_RGB`；
- 像素顺序 **BGRA**、行序 **bottom-up**（`biHeight` 为正数）：这是 CF_DIB 的通用
  惯例，Office / 聊天软件 / 画图都按它解释；
- 输入格式由 stb_image 决定（保存插件实际落盘的 PNG / JPEG 都支持）。

### 6.2 CF_HDROP（`also_file_drop = 1` 时写）

- `DROPFILES` 头 + UTF-16 宽字符路径 + 双 NUL 结尾（`fWide = TRUE`）；
- 路径是保存插件报告的**已经存在的文件**；粘贴到资源管理器时系统会复制该文件，
  所以要求截图文件还没有被用户删掉；
- 部分程序粘贴时优先用文件列表而不是图片；不需要文件时把 `also_file_drop` 关掉。

## 7. 失败降级

剪贴板是进程间共享资源，失败是常态，不能影响截屏本身：

| 情形 | 行为 |
| --- | --- |
| `enabled = 0` | 跳过，只记 DEBUG，不刷 WARN |
| `capture.done` 负载非法 / 缺 `path` / `path` 为空 | WARN 一行后跳过，等下一条事件 |
| 文件打不开、为空、超过 512 MiB | WARN（含路径）后跳过 |
| stb_image 解码失败 | WARN（含 stb 的失败原因）后跳过 |
| 剪贴板被其它程序占用 | `OpenClipboard` 重试 10 次 × 20ms；仍失败记 WARN 后跳过 |
| `SetClipboardData(CF_DIB)` 失败 | 释放全局内存（所有权没转移）、WARN 后跳过 |
| 图片已写入但 CF_HDROP 失败 | WARN 说明"图片已写入、文件列表失败"，图片粘贴仍可用 |

任何失败都只影响这一次的剪贴板复制：截图文件已经在盘上，下一次 `capture.done`
会重新尝试；宿主不会因为本插件失败而报错或退出。

## 8. 线程与生命周期

- 本插件**没有自己的线程、窗口和后台任务**：`capture.done` 的订阅回调由宿主事件
  分发线程（main）执行（`document/EVENT_BUS.md` §3），stb 解码与剪贴板写入也在
  这个回调里同步完成。
- 订阅在实例创建路径（`plugin_entry.cpp` 的 `create_instance`）建立——本插件没有
  `ui_provider` 的 init/deinit 生命周期，`create(index, host)` 是唯一能拿到宿主服务
  表的时机；退订在 `destroy_state` 里、实例释放之前完成。`destroy` 返回后宿主不会再
  回调本实例，也不会留下任何后台任务。
- `set_config` 可能来自设置页线程，而读取发生在事件分发线程：两个开关用
  `std::atomic<bool>` 保存，读取侧不加锁、不会读到撕裂的值。
- 语言表同样按这个生命周期走：`start()` 里构造 `shot::I18n` 并发布，`stop()` 里
  销毁（析构自动退订 `lists.changed`），`stop()` 重复调用是空操作——`destroy` 与
  析构各兜一次，退订之后宿主不会再回调本实例。

## 9. 平台限制

- **仅 Windows**：剪贴板写入用 `OpenClipboard` / `SetClipboardData` / `CF_DIB` /
  `CF_HDROP` 这套 Win32 API。非 Windows 平台在 `CMakeLists.txt` 里直接跳过整个
  插件（不留跑不起来的 stub），实现待补的位置在 CMakeLists 的 `// TODO:` 里注明：
  X11 用 `XSetSelectionOwner` + `image/png` target，macOS 用 `NSPasteboard`。
- 32 位/64 位进程都能写剪贴板，但 32 位进程写的 CF_DIB 与 64 位进程之间由系统
  做格式转换；本插件只按标准 CF_DIB 写，不依赖位深。

## 10. 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 自己采集/保存/编码 | 那是 `screen_capture` / `image_saver` 的职责；本插件只消费 `capture.done` |
| 订阅 `capture.request` 触发采集 | 会变成第二个"截屏入口"，和托盘/命令行抢职责，也违反事件总线单向消费的约定 |
| 从事件负载拿像素 | 像素不进事件总线（`document/CAPTURE.md` §3），只传路径 |
| 把图片转成 HTML / RTF / PNG 等多种剪贴板格式 | v1 只承诺"能粘贴图片"；加格式需要额外的编码器和兼容性验证，等有明确需求再说 |
| 开线程异步写剪贴板 | 解码只有几十毫秒；异步会让"退订后还有线程在写剪贴板"的生命周期变复杂，收益很小 |
| 把配置项写进 `metadata.json` | 真实 schema 在 `shot.configurable` 里，写两遍必然漂移（`document/METADATA.md` §3） |
| 自己写配置文件 | 存盘键是 `<plugin_id>/<key>`，落盘与启动回灌归宿主（`document/SETTINGS.md`） |

## 11. 检查清单

- [ ] 改配置项只改 `clipboard_config.cpp` 的 `kConfig[]` 与 `ClipboardConfig`，没有改宿主代码？
- [ ] `metadata.json` 的 `plugins[]` 顺序仍与 `kEntries` 一致、`interfaces` 与代码一致？
- [ ] 剪贴板操作前 `OpenClipboard` 有重试、每条失败路径都 `CloseClipboard` / `GlobalFree` 了吗？
- [ ] CF_DIB 是 32bpp BGRA + bottom-up 吗？`biSizeImage` / `biSize` 填对了吗？
- [ ] 失败路径都收敛成返回码 / WARN 了吗？异常有没有跨出 `extern "C"`？
- [ ] `destroy` 前退订了 `capture.done`、销毁了语言表吗？插件有没有留下线程或窗口？
- [ ] 改配置项文案时两张语言表的 key 集仍一致、每 key 两种语言都有文本、fallback 是中文原文吗？
- [ ] 公共头里的类/函数有 Doxygen 注释吗？未完成项写成代码里的 `// TODO:` 了吗？
