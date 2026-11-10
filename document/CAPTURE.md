# 采集流水线（`capture.request` / `capture.done` / `capture.failed`）

这份文档说明"截一张图"从事件到落盘的完整链路：宿主如何编排屏幕采集插件、图像
处理插件与图像保存插件，`capture.request` 的整屏 / 区域两种语义，事件主题与 JSON
负载，以及线程模型与失败语义。**改 `Host::CapturePipeline`、改三个能力插件、改
`--capture` 之前先读这里。**

## 1. 总体结构

宿主自己**不做**任何采集、处理、保存算法：它只按顺序调用插件、分配一次像素缓冲、
按请求裁剪并最终把结果变成事件。三个能力都是普通插件（一个能力一个 `interface`），
可替换、可启停。

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| 屏幕采集 | `shot.screen_capture`（`ShotScreenCaptureV1`） | `describe` 报告虚拟桌面几何（物理像素），`capture` 往宿主缓冲里写整屏像素 |
| 图像处理 | `shot.image_processor`（`ShotImageProcessorV1`） | 原地处理像素（可能 0 个，也可能多个） |
| 图像保存 | `shot.image_saver`（`ShotImageSaverV1`） | 把处理完的图像写成文件，格式/目录由插件自己决定 |
| 流水线 | `Host::CapturePipeline`（`include/capturePipeline.h`） | 解析请求、编排上面三步、整屏抓取后按区域裁剪、发事件 |
| 区域选择 | `plugins/region_selector`（`shot.ui_provider`） | 用户框选区域后，再发一条**带 `region`** 的 `capture.request`；不参与流水线执行 |
| 事件总线 | `shot.host.event_bus`（`document/EVENT_BUS.md`） | 主题发布/订阅；订阅回调在宿主 main 线程执行 |

一次采集的数据流（请求在 main 线程入队，执行在流水线工作线程）：

```
capture.request（托盘菜单 / 热键 / region_selector / --capture）
        │
        ▼ main 线程分发回调：只拷贝负载 + 入队（有界队列，满了丢弃并 warn）
CapturePipeline 工作线程（一次只处理一张，整段持宿主生命周期锁）
  ├─ 解析负载（三态，见 §3.1）
  │    ├─ 整屏请求（空负载 / 非 region 负载）→ 继续抓整屏
  │    ├─ 带合法 region → 抓整屏后按物理像素裁剪（越界夹取）
  │    └─ mode=region 但无 region / region 非法 → 直接忽略：不抓屏、不落盘、不发事件
  ├─ 取加载顺序里第一个 screen_capture
  │    describe() → 宿主按 width/height 分配 BGRA8 缓冲 → capture()
  ├─ 若是带合法 region 的请求：按物理像素从整屏图里裁剪
  ├─ 按加载顺序对每一个 image_processor 调 process()（单个失败只 warn）
  └─ 取加载顺序里第一个 image_saver
       save() → 插件落盘并写回最终路径
        ├─ 成功：发布 capture.done {path,width,height,processors}
        └─ 失败：发布 capture.failed {stage,reason}
```

`capture.done` / `capture.failed` 由工作线程发布，但事件总线只负责入队，**订阅回调
仍然在宿主 main 线程执行**；所以消费方（通知、剪贴板等）没有线程安全问题。

## 2. 两个能力的职责边界

### 2.1 屏幕采集：`shot.screen_capture`

两段式设计：**插件从不分配图像内存**。

| 函数 | 宿主做什么 | 插件做什么 |
| --- | --- | --- |
| `describe(self, out)` | 提供填好 `struct_size` 的输出结构 | 填 `width` / `height` / `origin_x` / `origin_y` / `pixel_format` |
| `capture(self, dst)` | 按 `pixel_format` 分配缓冲并填好 `ShotImage` | 只写像素；不得改结构体字段、不得保存 `dst`/`data` |

- v1 只定义 `SHOT_PIXEL_FORMAT_BGRA8`（4 字节/像素）。宿主把 `stride` 填成
  `width*4`、`channels` 填成 4，插件必须**按行寻址**（`stride` 可能是负值，表示自底向上）。
- **几何与像素都是物理像素**：`width/height` 是多显示器合成的虚拟桌面总尺寸，
  `origin_x/origin_y` 是内容左上角在系统坐标里的位置（多显示器时可能为负）。
  宿主不按 `origin` 二次平移——区域坐标本身就定义在"虚拟桌面左上角"这个原点上（见 §3.1）。
- 混合 DPI 实测（本机 150% 主屏 2560x1600 @0,0 + 100% 副屏 1024x768 @2560,0）：
  虚拟桌面是 **3584x1600 物理像素**；DPI-unaware 线程会看到 3584x1067 这类逻辑尺寸。
  采集插件在平台层入口临时把**调用线程**的 DPI 上下文提升到
  `PER_MONITOR_AWARE_V2`（运行期解析，失败只 warn、按原上下文继续，返回前恢复），
  所以不依赖宿主进程的 DPI 声明；宿主自己也已在最早阶段声明 per-monitor v2（见
  `document/SETTINGS.md` §6.1）。
- **负 origin 未实机验证**：公式全程有符号（`GetSystemMetrics` → 契约 `int32_t` →
  `BitBlt` 的 `int` 源坐标），本机两块屏都在主屏右侧，origin 为 `(0,0)`。
- **光标合成（可选）**：`plugins/screen_capture` 同时实现 `shot.configurable`，配置项
  `include_cursor`（BOOL，默认 `"1"`）。开启时插件在 `BitBlt` 之后、读回像素之前用
  `GetCursorInfo` + `GetIconInfo` + `DrawIconEx` 把**当前**鼠标光标画进整屏位图：屏幕坐标
  先减 `origin_x/origin_y` 换算成图像坐标、再减图标热点；光标此刻不可见
  （`CURSOR_SHOWING` 未置位）就不画，部分落在虚拟桌面外时由 `DrawIconEx` 自然裁剪。
  取光标信息 / 绘制失败只 WARN，**抓屏仍算成功**（`capture` 返回 0，只是没有光标）。
  配置值在每次抓屏前读一次快照，抓屏期间改设置只影响下一张图。
- 返回码：`0` 成功；`1` 当前没有可采集的屏幕；`2` 参数非法；`3` 系统调用失败。
  宿主对任何非 0 都发布 `capture.failed`，不会使用 `dst` 的内容。
- 缺 `describe` 或 `capture` 的实例**不会被包装成采集插件**：加载时按"没有这个
  能力"跳过，与现有能力缺失的处理一致。
- **能力回调运行在流水线工作线程上**（不是 main 线程）：插件实现不要再假设"永远
  在宿主主线程"，不要直接碰 UI；与 `set_config`（main 线程）共享状态时要自己做同步。

### 2.2 图像保存：`shot.image_saver`

保存策略全部在插件侧，宿主不硬编码格式、扩展名、目录布局：

- `directory` / `basename` 是**本次调用的覆盖参数**：传 `NULL` 表示"用插件自己的
  配置"（主机流水线与 `--capture` 都传 `NULL`）；设置页"另存为"这类功能才传非空值。
- `basename` 不含扩展名；扩展名由保存插件按自己的格式追加。
- `path` 由宿主提供（v1 固定 1024 字节，NUL 结尾）；插件成功时把最终路径写进去，
  长度不够返回 `2`。路径只用于日志与事件负载，宿主不解析它。
- 返回码：`0` 成功；`1` 参数非法；`2` path 缓冲不足；`3` 保存失败。
- 与采集一样，`save` 可能在流水线工作线程上被调用。

## 3. 事件主题与负载

| 主题 | 方向 | 负载 |
| --- | --- | --- |
| `capture.request` | 任意插件/UI（托盘菜单、热键、region_selector、`--capture`）→ 流水线 | 空 = 整屏；或 JSON：`{"mode":"region","region":{"x":0,"y":0,"width":800,"height":600}}`，语义见 §3.1 |
| `capture.done` | 流水线（工作线程）→ 所有订阅者 | JSON：`{"path":"...","width":1920,"height":1080,"processors":2}`；`width/height` 是**最终图像**尺寸（区域截图时是裁剪后的） |
| `capture.failed` | 流水线（工作线程）→ 所有订阅者 | JSON：`{"stage":"capture","reason":"没有可用的屏幕采集插件"}` |
| `app.quit` | 任意插件 → 宿主 | 空；宿主收到后退出主循环 |

`capture.failed` 的 `stage` 只有两个值：

- `"capture"`：没有采集插件、`describe`/`capture` 失败、尺寸/像素格式非法；
- `"save"`：没有保存插件、`save` 返回非 0。

`capture.done` 的 `processors` 是**成功处理过这张图**的处理器个数（失败的只记
warn，不计入）。`path` 可能为空（保存插件返回成功但没写回路径，只记 warn，不判失败）。
两个负载都是纯通知：宿主没有"确认/重试"机制，队列满时事件会丢
（见 `document/EVENT_BUS.md` §5），但文件已经落盘。

**像素不进总线**：`capture.done` 只带路径与尺寸。需要画面数据的功能（剪贴板、
通知缩略图）从文件读，不靠事件总线传大对象。

### 3.1 `capture.request` 的三种负载语义

```json
{"mode":"region","region":{"x":100,"y":200,"width":800,"height":600}}
```

`capture.request` 按负载分成**三态**（最终约定）：

1. **整屏请求**：空负载，或任何"非 region"的负载（没有 `mode`、`mode` 是其它值、
   JSON 非法等）→ 整屏采集。托盘「截屏」、热键的 `capture_hotkey`、`--capture` 都是空负载。
2. **区域采集请求**：`mode = "region"` 且 `region` 是**合法对象** → 先抓整屏，再按区域裁剪。
   坐标语义（**与 `screen_capture` 的 `describe` 同一坐标系**）：
   - 单位是**物理像素**，原点 = **虚拟桌面左上角**（不是某块显示器、也不受 DPI 缩放影响）；
   - `x/y/width/height` 必须是**整数**，`width/height` 必须 ≥ 1；
   - **越界夹取**：`x/y` 夹进画面，`width/height` 不超过剩余范围；
   - `capture.done` 的 `width/height` 是**裁剪后**的尺寸，不是虚拟桌面尺寸；
   - 带合法 `region` 的请求不会再被 region_selector 当作框选触发（防自触发循环），
     它只负责采集。
3. **区域框选请求**：`mode = "region"` 但**没有 `region` 对象**，或 `region` 字段非法
   （不是对象、`x/y/width/height` 缺失或非整数、`width/height` ≤ 0）→ **流水线直接忽略**：
   **不抓屏、不落盘、不发 `capture.done` / `capture.failed`**，只记一条 debug 日志
   （"交给 region_selector"）。**事件本身照常分发给其它订阅者**——`plugins/region_selector`
   就是靠它弹出框选覆盖层的；用户确认后它再发一条**带合法 `region`** 的请求，进入第 2 态。
   - 这是"两段式区域截屏"：第 3 态是"请开始框选"，第 2 态才是"按这个区域采集"。
   - 以前缺 `region` 时流水线会把它当成整屏采集（用户可感知：每次区域截屏多存一张
     全屏图）；现在按第 3 态直接忽略，这个 bug 已经修掉。
   - `region_selector` 没加载 / 被禁用时，第 3 态请求只是没人弹框选（不会误采集）。

`mode` 是其它值：info 日志 + 按第 1 态（整屏）处理，向前兼容（以后加新 mode 时老流水线
不报错）。负载不是合法 JSON 对象：warn + 按第 1 态（整屏）处理。

`region_selector` 发布的区域也是物理虚拟桌面像素：`region.x = 覆盖窗口屏幕位置 +
窗口内光标位置 − 虚拟桌面原点`（详见 `plugins/region_selector/README.md` §4）。

## 4. 执行顺序与失败语义

| 阶段 | 取哪些实例 | 失败语义 |
| --- | --- | --- |
| 解析 region（三态） | —— | 空 / 非 region：整屏；合法 region：裁剪；**mode=region 但无 region / region 非法：整条请求被忽略**（不算失败，不抓屏、不落盘、不发事件） |
| 采集 | 加载顺序里的**第一个** `screen_capture` | 直接发布 `capture.failed(stage=capture)`，不进入后续阶段 |
| 裁剪 | 宿主自己（不调插件） | 越界夹取；裁剪不出有效像素（内部防御路径）warn 后按整屏继续，不算失败 |
| 处理 | 加载顺序里的**所有** `image_processor` | 单个处理器失败：warn 后**继续**用当前图像往下走；0 个处理器也照常保存 |
| 保存 | 加载顺序里的**第一个** `image_saver` | 直接发布 `capture.failed(stage=save)` |

- "加载顺序"= 插件目录扫描 + 依赖拓扑排序后的创建顺序；同一类型内就是注册顺序。
- 处理器原地修改同一张 `ShotImage`，所以顺序会影响结果；v1 不做并行处理。
- 任何阶段失败都只影响**这一次采集**：宿主继续运行，等下一次 `capture.request`。
- 保存成功但插件没写回路径（空串）只警告，不把这次采集判失败；`capture.done`
  的 `path` 可能为空。

## 5. 插件启停与降级

- 流水线工作线程每次执行都从 `PluginManager` 取**实例快照**（`shared_ptr`），不缓存
  实例；整段执行持**宿主生命周期锁**，所以一次采集从抓屏到保存结束，用到的插件不会被
  热卸载。
- 这也意味着**禁用/卸载/宿主退出要等当前这张图走完**（几百毫秒级）：宿主在 main
  线程拿同一把生命周期锁，天然与工作线程串行，不会出现"卸到一半还在调插件"。
- 工作线程**从不等待 main 线程**（发布事件只入队、完成通知只 notify 等待方），所以
  main 等 worker（`--capture` 的同步等待、退出时的 `join`）不会互相环等，见 §7。
- 没有采集插件 / 没有保存插件：发布 `capture.failed` 并写日志，**宿主不崩、不退出**。
  `--capture` 在这种情况下返回非 0。
- 热禁用当前"第一个"采集/保存插件后，加载顺序里的下一个同类插件会顶上；没有
  替补就按"没有插件"失败。这与 `document/SETTINGS.md` 的启停语义一致。
- 事件订阅在 `HostRuntime::init` 建立、在析构第一步 `CapturePipeline::stop()` 时退订，
  然后才卸载插件：保证回调不会再落到已经卸载的插件上。

## 6. `--capture` 用法

```
Plugin-shot --capture
```

| 步骤 | 行为 |
| --- | --- |
| 1 | 初始化日志、配置、插件（与常驻模式完全相同）；插件目录必须是 `./plugins` |
| 2 | 把一次空负载的采集请求排进与事件触发相同的队列，main 线程同步等 worker 完成 |
| 3 | 采集完成（无论成败）后，在退出前**主动派发一轮事件队列**，让 `capture.done` / `capture.failed` 的订阅者（剪贴板、通知等）收到本次结果 |
| 4 | 成功：日志打印 `Capture saved: <path>`（同时写入 host.log），退出码 `0` |
| 5 | 失败：日志里有 `capture.failed` 的原因，退出码 `1`（含队列满被丢弃、插件缺失、抓屏/保存失败） |
| 6 | 未知参数：打印用法，退出码 `2`；`--help` 退出码 `0` |

前提是屏幕采集插件与图像保存插件都已加载；任一缺失就走降级路径（退出码 `1`）。
`--capture` 与 `--settings` 同时给出时按 `--capture` 处理（`--settings` 被忽略并记 warn）。

## 7. 线程模型

```
main 线程                                 worker 线程
  capture.request 回调                     等待队列条件变量
    └─ 拷贝负载 + 入队（上限 4，满了丢弃）──▶ 取一条请求
  --capture：captureOnce()                  整段持宿主生命周期锁：
    └─ 放入请求后 wait completion cv          解析 region → 采集 → 裁剪 → 处理 → 保存
  阻塞等待事件 → dispatchEvents()（订阅回调仍在这里）        发布 capture.done/failed（只入队）
                                            notify completion cv（--capture 的等待方）
  退出：CapturePipeline::stop()
    退订 capture.request → 标记 stopping → 丢弃未开始的排队请求 → join worker
```

- **入队（main 线程）**：只做"拷贝负载 + push 队列"，非阻塞；队列上限 **4** 条
  （串行执行，一张 4K 图几百毫秒，队列再长只会让用户觉得点了很久才响应）。满了
  **丢弃该请求并 warn**（日志写明上限）；`--capture` 的请求也走同一队列，满了同样失败。
- **执行（worker 线程）**：一次只处理一张，先进先出串行；**整段持宿主生命周期锁**，
  保证抓屏/处理/保存期间插件不被热卸载。代价是启停/退出要等当前图走完。
- **死锁边界**：worker 永不等待 main 线程——事件发布只入队、结果通知只 `notify`；
  main 等 worker 的场景（`--capture`、`stop` 的 join）都不持有生命周期锁。因此没有
  "main 持锁等 worker / worker 等锁"的环。
- **停止**：`stop()` 先退订、再标记停止并丢弃**未开始**的请求，然后 `join`；正在执行
  的那张图会走完再退出。正常退出路径不会有同步等待方；防御性地把被丢弃请求的等待方
  唤醒为失败，避免 `--capture` 卡死。
- **事件订阅回调**仍然只在 main 线程执行（`dispatchEvents()`），消费方规则不变：
  回调里只做入队/刷新 UI，不要在里面再触发一次采集或做耗时任务（要耗时请自己开线程）。
- **插件回调线程**：`screen_capture` / `image_processor` / `image_saver` 的能力回调在
  worker 线程上执行；`shot.configurable` 的 `set_config` 仍在 main 线程（启动回灌、
  设置页应用）。插件若有跨线程共享状态必须自己同步。

## 8. 刻意不做的事（v1 边界）

| 不做 | 原因 |
| --- | --- |
| 插件侧的"区域采集 / 单显示器采集" | 契约只描述整个虚拟桌面；宿主先抓整屏再按 region 裁剪，插件不感知取景范围 |
| 多帧动画 / 录屏 | 单帧截图之外的东西不在 v1 契约里定义（光标已经由 `plugins/screen_capture` 的可选合成实现，见 §2.1） |
| 剪贴板 / 直接复制 | 属于独立插件（`plugins/clipboard`）消费 `capture.done`，和"落盘"是两件事 |
| 内置图像编辑器 | 编辑器应当是独立插件，通过 `capture.done` + frame 服务取图 |
| 多 worker / 流水线内并行处理 | 一次只处理一张：内存峰值可控、处理器顺序稳定；并发收益小于复杂度 |
| 采集中途取消 | v1 没有取消事件；正在执行的那张图会走完（几百毫秒），排队请求可被 `stop` 丢弃 |
| `--capture` 之外的命令行采集参数 | 区域参数走 `capture.request` 负载；命令行只保留"截一张就走"的最简语义 |

未完成项在代码里以 `// TODO:` 标注（如 `src/capturePipeline.cpp`），
不进本文档的"计划"。

## 9. 检查清单

- [ ] 新增/修改插件能力？→ 改 `include/plugin_api.h` 并同步 `document/ABI_POLICY.md`
      登记表与 `SHOT_ABI_MINOR`，再改本文档 §2。
- [ ] 改了流水线的阶段顺序 / 线程模型 / 队列语义？→ 同步本文档 §4、§5、§7。
- [ ] 改了 `capture.request` 负载或 region 的坐标语义？→ 同步本文档 §3.1、
      `document/EVENT_BUS.md` §4、`plugins/region_selector/README.md`。
- [ ] 新增事件主题或改负载字段？→ 同步本文档 §3 **和** `document/EVENT_BUS.md` §4。
- [ ] 采集插件返回的图像按 `stride` 逐行访问了吗？`stride` 可以为负/大于
      `width*channels` 吗？
- [ ] 保存插件写回 `path` 了吗？路径缓冲不够时返回 `2` 了吗？
- [ ] 插件 `deinit`（UI 插件）/ `destroy`（Generic 插件）里退订了自己订阅的主题、
      停掉后台线程了吗？
- [ ] 能力回调在 worker 线程上被调用这件事，插件实现考虑线程安全了吗？
- [ ] `--capture` 的退出码、以及"退出前派发一轮事件"的行为符合本文档 §6 吗？
