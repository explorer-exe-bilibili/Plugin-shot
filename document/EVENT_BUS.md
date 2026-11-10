# 宿主事件总线（`shot.host.event_bus`）

**线程之间**（插件线程 ↔ 宿主 main 线程、插件 ↔ 插件、采集 worker ↔ main）的**进程内**
消息通道。宿主实现、插件使用，接口定义在 `include/plugin_api.h` 的 `ShotHostEventBusV1`，
插件侧封装是 `shot_plugin_helpers.h` 里的 `shot::EventBus`。

## 1. 为什么需要它

"万物皆插件"下没有 Qt 的 signal/slot 可用，插件之间也不能直接互调（跨 DLL 只能走
宿主）。注意区分两条通道：**宿主调用插件能力是直接函数调用**（同一个线程上一次调用，
例如 worker 线程上的 `image_processor` 链共享同一张 `ShotImage`，不经过总线）；
**需要跨线程 / 跨插件通知时才走总线**——总线通过**主题字符串**解耦：发送方不知道谁
在听，接收方不知道谁发的，订阅回调统一在宿主 main 线程执行。

它同时补上了旧的 `broadcast_event`：那条路径只把事件塞进宿主队列、没有订阅者；
现在 `post_to_host` 只投给宿主，`broadcast_event` 等价于 `publish`，总线投给所有订阅者。

## 2. 接口（版本 1）

| 字段 | 签名 | 语义 |
| --- | --- | --- |
| `publish` | `int32_t (*)(void* ctx, const char* topic, const void* data, uint32_t len)` | 发布；负载深拷贝后入队，**非阻塞**；0 = 已入队，-1 = 参数非法 / 超限 / 队列满（丢弃） |
| `subscribe` | `int32_t (*)(void* ctx, const char* topic, Cb cb, void* user)` | 订阅主题模式；`Cb = void (*)(void* user, const char* topic, const void* data, uint32_t len)` |
| `unsubscribe` | `int32_t (*)(void* ctx, void* user)` | 退订该 `user` 的全部订阅；0 = 至少退订一条 |

获取方式：`host->query_interface(host->host_context, SHOT_HOST_IFACE_EVENT_BUS, 1)`，
返回 `const ShotHostEventBusV1*`；老宿主返回 `NULL`，插件按"没有总线"降级。

## 3. 线程模型

```
任意插件线程 --publish--> [宿主队列 + 唤醒标志] --宿主 main 循环阻塞等待--> dispatchEvents() --> 订阅回调
```

- **publish 任意线程可调**：只加锁入队 + 唤醒 main 线程，不做分发、不阻塞。采集流水线的
  `capture.done` / `capture.failed` 就是由**工作线程**发布的，但订阅回调仍然只在 main 线程执行。
- **宿主 main 循环是事件驱动阻塞等待**：没有事件、也没有待处理的主线程工作时，main 线程
  阻塞在 `HostServices::waitForActivity()`（condition_variable）上，不再 16ms 轮询；
  `publish` / `post_to_host` / 插件启停请求都会立即唤醒它（1s 超时只是防漏唤醒的兜底）。
- **回调只在宿主分发线程（main 线程）执行**：顺序确定、不需要再同步。
- **回调里规则**：不要阻塞、不要做耗时任务；`publish` 会排到下一轮（不会重入）；
  `subscribe` / `unsubscribe` 安全（回调在锁外执行）。
- 同一轮分发使用**订阅快照**：退订保证"没有新回调"，不保证"当前轮没有在途回调"。
- 列表协议也建立在这个线程模型上：`lists.*` 事件只在 main 线程分发处理，所以列表写路径
  天然单线程（见 `document/LISTS.md` §3、§7）。

## 4. 主题约定

全小写、点分隔、模块作前缀，动词放最后：

| 主题 | 方向 | 负载 |
| --- | --- | --- |
| `capture.request` | 任意插件/UI（托盘菜单、热键、region_selector、`--capture`）→ 流水线 | 三态：空 / 非 region = 整屏；`{"mode":"region","region":{x,y,width,height}}` = 抓整屏后按物理虚拟桌面像素裁剪；`{"mode":"region"}` 无 region 或 region 非法 = **框选请求**（流水线忽略：不采集、不落盘、不发 `capture.*`），事件仍分发给其它订阅者（region_selector 用它开始框选）。见 `document/CAPTURE.md` §3.1 |
| `capture.done` | 流水线（工作线程）→ 所有订阅者 | JSON：`{"path":"...","width":1920,"height":1080,"processors":2}`；`width/height` 是最终图像尺寸（区域截图时是裁剪后的） |
| `capture.failed` | 流水线 → 所有订阅者 | JSON：`{"stage":"capture","reason":"..."}`；`stage` = `"capture"` / `"save"` |
| `settings.open` | 托盘（`plugins/tray`）→ 设置页 | 空 |
| `config.applied` | 宿主 → 所有插件 | 空（设置页一次批量提交完成，见 `document/SETTINGS.md`） |
| `hotkey.suspend` | 设置页（`plugins/settings_ui`）→ 热键插件（`plugins/hotkey`） | 空；开始"按组合键录入"前发布，热键插件据此撤销全部全局注册（否则用户按到当前生效的组合键会先触发真实动作，例如立刻截一张图） |
| `hotkey.resume` | 设置页 → 热键插件 | 空；录入结束（确认 / Esc / 清空）后发布，热键插件按**当前配置**重新注册（幂等；暂停期间收到 `config.applied` 只撤销不注册） |
| `plugins.changed` | 宿主 → 所有 | JSON：插件启停结果（字段见 `document/SETTINGS.md` §3.2）；一次启停可能发多条（级联启用） |
| `lists.add` | 插件 → 宿主 | JSON：`{"list":"...","owner":"<插件 id>","items":[...]}`；运行期贡献列表条目，见 `document/LISTS.md` §3.1 |
| `lists.remove` | 插件 → 宿主 | JSON：`{"list":"...","owner":"...","ids":["..."]}`；`ids` 省略 = 撤回该 owner 全部，见 `document/LISTS.md` §3.2 |
| `lists.update` | 消费方（设置页）→ 宿主 | JSON：`{"list":"config_list","tag":"...","changes":[{"owner","id","value"}]}`；仅 `config_list`，见 `document/LISTS.md` §3.3 |
| `lists.commit` | 消费方（设置页）→ 宿主 | JSON：`{"list":"config_list","tag":"..."}`；仅 `config_list`，落盘，见 `document/LISTS.md` §3.4 |
| `lists.updated` | 宿主 → 所有 | JSON：`{"list":"config_list","tag":"...","results":[{"owner","id","ok","reason"}],"error"?}`；写值/提交结果，失败也回，见 `document/LISTS.md` §3.5 |
| `lists.changed` | 宿主 → 所有 | JSON：`{"list":"<list id>"}`；列表内容变化的唯一通知，同轮合并、每 list 一条，见 `document/LISTS.md` §3.6 |
| `app.quit` | 任意 → 宿主 | 空；宿主收到后退出主循环 |

**匹配规则**：精确匹配；`"capture.*"` 匹配该前缀下所有主题；`"*"` 匹配全部。
同一个 `callback` 用多个模式订阅时，会按每个匹配的订阅各回调一次（不是去重）。
第三方扩展用自己的前缀（如 `acme.sharpen.*`），别占用 `shot.*`。

`capture.*` 的完整数据流、阶段失败语义与 `--capture` 退出码见 `document/CAPTURE.md`；
本文只记主题约定，不重复长篇。

`lists.*` 是"列表协议"的主题组：**读**走宿主服务 `shot.host.plugin_lists` 拿 JSON 快照，
**写**走上面这些事件。完整的负载 schema、owner 校验、排序规则与刷新时机见
`document/LISTS.md`；改列表协议要同步改那份文档的登记表，不要只改这里。

## 5. 生命周期与限制

- 插件必须在**实例销毁前退订**：`shot.ui_provider` 插件在 `deinit` 返回前
  `unsubscribe(user)`；`PluginType::Generic` 插件没有 `init/deinit`，订阅在
  `create(index, host)` 里建立、在 `destroy` 里退订（见 `document/SETTINGS.md` §3.3）。
  卸载后宿主不再回调。
- 单条负载上限 **64 KiB**；队列上限 **4096** 条，满时 publish 返回 -1（丢弃，不阻塞）。
- **大对象不进总线**：`capture.done` 只带路径与尺寸，画面数据走单独的 frame/surface 服务
  （见 `document/CAPTURE.md` §3）；列表内容也不塞进事件——事件里只放 list id 与条目 JSON，
  完整快照用 `list_json` 读（见 `document/LISTS.md` §8）。
- 请求-应答：负载里带 `request_id`，应答发到 `<topic>.reply`（约定，不是硬机制）。
- 事件名/主题在接收方收到前一直有效（宿主已拷贝），接收方不需要自己再拷。

## 6. 用法

C 侧（插件 ABI 内）：

```c
static void on_capture_done(void* user, const char* topic, const void* data, uint32_t len) {
    /* 只做入队/拷贝，耗时任务转到自己的线程 */
}
const ShotHostEventBusV1* bus =
    (const ShotHostEventBusV1*)host->query_interface(host->host_context, SHOT_HOST_IFACE_EVENT_BUS, 1);
if (bus != NULL && SHOT_HAS_FIELD(bus, ShotHostEventBusV1, subscribe)) {
    bus->subscribe(host->host_context, "capture.*", &on_capture_done, self);
}
```

C++ 侧（`shot_plugin_helpers.h`）：

```cpp
shot::EventBus bus(host);
bus.subscribe("capture.done", &on_capture_done, this);
bus.publish("capture.request", payload);
// deinit: bus.unsubscribe(this);
```

## 7. 检查清单

- [ ] 主题全小写、点分隔，第三方用自己前缀？
- [ ] 负载 < 64 KiB，像素/大对象走 frame 服务而不是总线？
- [ ] 回调里没有阻塞、没有直接操作其他插件的 UI？
- [ ] 退订时机正确：UI 插件在 `deinit` 返回前，Generic 插件在 `destroy` 里？
- [ ] publish 返回值检查了（队列满会丢事件）？
- [ ] 新增的宿主服务能力已在 `document/ABI_POLICY.md` 登记？
- [ ] 新增/修改的是列表协议（`lists.*` / 新 list id）？→ 同步 `document/LISTS.md` 的登记表与 schema。
