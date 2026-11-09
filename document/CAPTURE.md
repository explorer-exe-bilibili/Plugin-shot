# 采集流水线（`capture.request` / `capture.done` / `capture.failed`）

这份文档说明"截一张图"从事件到落盘的完整链路：宿主如何编排屏幕采集插件、图像
处理插件与图像保存插件，事件主题与 JSON 负载长什么样，哪些失败必须优雅降级。
**改 `Host::CapturePipeline`、改三个能力插件、改 `--capture` 之前先读这里。**

## 1. 总体结构

宿主自己**不做**任何采集、处理、保存算法：它只按顺序调用插件、分配一次像素缓冲、
把结果变成事件。三个能力都是普通插件（一个能力一个 `interface`），可替换、可启停。

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| 屏幕采集 | `shot.screen_capture`（`ShotScreenCaptureV1`） | `describe` 报告尺寸，`capture` 往宿主缓冲里写像素 |
| 图像处理 | `shot.image_processor`（`ShotImageProcessorV1`） | 原地处理像素（可能 0 个，也可能多个） |
| 图像保存 | `shot.image_saver`（`ShotImageSaverV1`） | 把处理完的图像写成文件，格式/目录由插件自己决定 |
| 流水线 | `Host::CapturePipeline`（`include/capturePipeline.h`） | 编排上面三步、分配缓冲、发事件 |
| 事件总线 | `shot.host.event_bus`（`document/EVENT_BUS.md`） | 主题发布/订阅；回调在宿主 main 线程执行 |

一次采集的数据流（回调全部在宿主 main 线程上同步执行）：

```
capture.request（托盘菜单/设置页插件发布；--capture 直接调流水线）
        │
        ▼ 宿主分发线程(main)
CapturePipeline::captureOnce()
  ├─ 取加载顺序里第一个 screen_capture
  │    describe() → 宿主按 width/height 分配 BGRA8 缓冲 → capture()
  ├─ 按加载顺序对每一个 image_processor 调 process()（单个失败只 warn）
  └─ 取加载顺序里第一个 image_saver
       save() → 插件落盘并写回最终路径
        ├─ 成功：发布 capture.done {path,width,height,processors}
        └─ 失败：发布 capture.failed {stage,reason}
```

## 2. 两个能力的职责边界

### 2.1 屏幕采集：`shot.screen_capture`

两段式设计：**插件从不分配图像内存**。

| 函数 | 宿主做什么 | 插件做什么 |
| --- | --- | --- |
| `describe(self, out)` | 提供填好 `struct_size` 的输出结构 | 填 `width` / `height` / `origin_x` / `origin_y` / `pixel_format` |
| `capture(self, dst)` | 按 `pixel_format` 分配缓冲并填好 `ShotImage` | 只写像素；不得改结构体字段、不得保存 `dst`/`data` |

- v1 只定义 `SHOT_PIXEL_FORMAT_BGRA8`（4 字节/像素）。宿主把 `stride` 填成
  `width*4`、`channels` 填成 4，插件必须**按行寻址**（`stride` 可能是负值，表示自底向上）。
- `origin_x/origin_y` 是内容左上角在系统坐标里的位置；多显示器时可能是负数。
  v1 的宿主只把它当作信息，不据此裁剪。
- 返回码：`0` 成功；`1` 当前没有可采集的屏幕；`2` 参数非法；`3` 系统调用失败。
  宿主对任何非 0 都发布 `capture.failed`，不会使用 `dst` 的内容。
- 缺 `describe` 或 `capture` 的实例**不会被包装成采集插件**：加载时按"没有这个
  能力"跳过，与现有能力缺失的处理一致。

### 2.2 图像保存：`shot.image_saver`

保存策略全部在插件侧，宿主不硬编码格式、扩展名、目录布局：

- `directory` / `basename` 是**本次调用的覆盖参数**：传 `NULL` 表示"用插件自己的
  配置"（主机流水线与 `--capture` 都传 `NULL`）；设置页"另存为"这类功能才传非空值。
- `basename` 不含扩展名；扩展名由保存插件按自己的格式追加。
- `path` 由宿主提供（v1 固定 1024 字节，NUL 结尾）；插件成功时把最终路径写进去，
  长度不够返回 `2`。路径只用于日志与事件负载，宿主不解析它。
- 返回码：`0` 成功；`1` 参数非法；`2` path 缓冲不足；`3` 保存失败。

## 3. 事件主题与负载

| 主题 | 方向 | 负载 |
| --- | --- | --- |
| `capture.request` | 任意插件/UI → 流水线 | 空；v1 忽略负载（区域参数属于 TODO，见 §8） |
| `capture.done` | 流水线 → 所有订阅者 | JSON：`{"path":"...","width":1920,"height":1080,"processors":2}` |
| `capture.failed` | 流水线 → 所有订阅者 | JSON：`{"stage":"capture","reason":"没有可用的屏幕采集插件"}` |
| `app.quit` | 任意插件 → 宿主 | 空；宿主收到后退出主循环 |

`capture.failed` 的 `stage` 只有两个值：

- `"capture"`：没有采集插件、`describe`/`capture` 失败、尺寸/像素格式非法；
- `"save"`：没有保存插件、`save` 返回非 0。

`capture.done` 的 `processors` 是**成功处理过这张图**的处理器个数（失败的只记
warn，不计入）。两个负载都是纯通知：宿主没有"确认/重试"机制，队列满时事件会丢
（见 `document/EVENT_BUS.md` §5），但文件已经落盘。

**像素不进总线**：`capture.done` 只带路径与尺寸。需要画面数据的功能（编辑器、
剪贴板）以后走单独的 frame/surface 服务，不靠事件总线传大对象。

## 4. 执行顺序与失败语义

| 阶段 | 取哪些实例 | 失败语义 |
| --- | --- | --- |
| 采集 | 加载顺序里的**第一个** `screen_capture` | 直接发布 `capture.failed(stage=capture)`，不进入后续阶段 |
| 处理 | 加载顺序里的**所有** `image_processor` | 单个处理器失败：warn 后**继续**用当前图像往下走；0 个处理器也照常保存 |
| 保存 | 加载顺序里的**第一个** `image_saver` | 直接发布 `capture.failed(stage=save)` |

- "加载顺序"= 插件目录扫描 + 依赖拓扑排序后的创建顺序；同一类型内就是注册顺序。
- 处理器原地修改同一张 `ShotImage`，所以顺序会影响结果；v1 不做并行处理。
- 任何阶段失败都只影响**这一次采集**：宿主继续运行，等下一次 `capture.request`。
- 保存成功但插件没写回路径（空串）只警告，不把这次采集判失败；`capture.done`
  的 `path` 可能为空。

## 5. 插件启停与降级

- 流水线每次执行都从 `PluginManager` 取**实例快照**（`shared_ptr`），不缓存实例，
  所以"这次采集用哪个插件"永远是最新状态。
- 启停（`shot.host.plugin_registry` → `PluginManager::setPluginEnabled`）只在宿主
  main 线程执行，`captureOnce` 也只在 main 线程执行：两者天然串行，不会卸到一半
  还在调插件。插件视图里的 `deinit` 仍必须停线程、停一切后台任务。
- 没有采集插件 / 没有保存插件：发布 `capture.failed` 并写日志，**宿主不崩、不退出**。
  `--capture` 在这种情况下返回非 0。
- 热禁用当前"第一个"采集/保存插件后，加载顺序里的下一个同类插件会顶上；没有
  替补就按"没有插件"失败。这与 `document/SETTINGS.md` 的启停语义一致，不需要
  通知流水线做特殊处理。
- 事件订阅在 `HostRuntime::init` 建立、在析构时先 `CapturePipeline::stop()` 再卸载
  插件：保证回调不会再落到已经卸载的插件上。

## 6. `--capture` 用法

```
Plugin-shot --capture
```

| 步骤 | 行为 |
| --- | --- |
| 1 | 初始化日志、配置、插件（与常驻模式完全相同） |
| 2 | 直接调用 `CapturePipeline::captureOnce()`（不经过事件总线，等价于一次 `capture.request`） |
| 3 | 成功：日志打印 `Capture saved: <path>`（同时写入 host.log），退出码 `0` |
| 4 | 失败：日志里有 `capture.failed` 的原因，退出码 `1` |
| 5 | 未知参数：打印用法，退出码 `2` |

前提是屏幕采集插件与图像保存插件都已加载；任一缺失就走降级路径（退出码 `1`）。

## 7. 线程模型

- `capture.request` 可由任意线程 `publish`（只入队），但**回调只在宿主分发线程
  （main 线程）执行**，所以流水线里调用插件永远在 main 线程。
- `captureOnce()` 是同步阻塞的：抓屏 + 处理 + 编码期间不处理其他事件。v1 接受这个
  限制；等采集/保存变重（4K 编码、网络上传）再挪到工作线程，见 §8。
- `capture.done` / `capture.failed` 的订阅回调同样跑在 main 线程：只做入队/刷新 UI，
  不要在里面再开一次采集。

## 8. 刻意不做的事（v1 边界）

| 不做 | 原因 |
| --- | --- |
| 区域采集 / 单显示器采集 | v1 只描述整个虚拟桌面；以后用新字段或新结构体扩展，老字段不动 |
| 光标、多帧动画 | 合成规则因平台/场景而异，先不定义，避免把错误规则固化进 ABI |
| 剪贴板 / 直接复制 | 属于独立的 copy 服务，和"落盘"是两件事，不塞进 image_saver |
| 内置图像编辑器 | 编辑器应当是独立插件，通过 `capture.done` + frame 服务取图 |
| 工作线程 / 异步流水线 | 先保证正确性与线程安全；目前同步执行，慢的问题在 §7 记录 |
| 采集参数（区域/mode） | `capture.request` 的负载 v1 忽略，保留给以后扩展；没有参数就不会有"参数不认识"的歧义 |

未完成项在代码里以 `// TODO:` 标注（`src/main.cpp`、`src/capturePipeline.cpp`），
不进本文档的"计划"。

## 9. 检查清单

- [ ] 新增/修改插件能力？→ 改 `include/plugin_api.h` 并同步 `document/ABI_POLICY.md`
      登记表与 `SHOT_ABI_MINOR`，再改本文档 §2。
- [ ] 改了流水线的阶段顺序或失败语义？→ 同步本文档 §4。
- [ ] 新增事件主题或改负载字段？→ 同步本文档 §3 **和** `document/EVENT_BUS.md` §4。
- [ ] 采集插件返回的图像按 `stride` 逐行访问了吗？`stride` 可以为负/大于
      `width*channels` 吗？
- [ ] 保存插件写回 `path` 了吗？路径缓冲不够时返回 `2` 了吗？
- [ ] 插件 `deinit` 里退订了自己订阅的主题、停掉后台线程了吗？
- [ ] `--capture` 的退出码与日志符合本文档 §6 吗？
