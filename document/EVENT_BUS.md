# 宿主事件总线（`shot.host.event_bus`）

插件之间、插件与宿主之间的**进程内**消息通道。宿主实现、插件使用，接口定义在
`include/plugin_api.h` 的 `ShotHostEventBusV1`，插件侧封装是 `shot_plugin_helpers.h`
里的 `shot::EventBus`。

## 1. 为什么需要它

"万物皆插件"下没有 Qt 的 signal/slot 可用，插件之间也不能直接互调（跨 DLL 只能走
宿主）。总线让它们通过**主题字符串**解耦：发送方不知道谁在听，接收方不知道谁发的。

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
任意插件线程 --publish--> [宿主队列] --宿主 main 循环 dispatchEvents()--> 订阅回调
```

- **publish 任意线程可调**：只加锁入队，不做分发、不阻塞。
- **回调只在宿主分发线程（main 线程）执行**：顺序确定、不需要再同步。
- **回调里规则**：不要阻塞、不要做耗时任务；`publish` 会排到下一轮（不会重入）；
  `subscribe` / `unsubscribe` 安全（回调在锁外执行）。
- 同一轮分发使用**订阅快照**：退订保证"没有新回调"，不保证"当前轮没有在途回调"。

## 4. 主题约定

全小写、点分隔、模块作前缀，动词放最后：

| 主题 | 方向 | 负载 |
| --- | --- | --- |
| `capture.request` | UI → 采集 | 空或参数（区域/模式） |
| `capture.done` | 采集 → 编辑器/剪贴板 | 帧 id 等元数据（**不传像素**） |
| `editor.save` / `editor.copy` | 编辑器 → 宿主/剪贴板 | 输出路径或空 |
| `hotkey.triggered` | 托盘/热键 → 采集 | 热键 id |
| `settings.open` | 托盘/热键 → 设置页 | 空 |
| `config.applied` | 宿主 → 所有 | 空（设置页一次批量提交完成，见 `document/SETTINGS.md`） |
| `plugins.changed` | 宿主 → 所有 | JSON：插件启停结果（字段见 `document/SETTINGS.md` §3.2） |
| `app.quit` | 任意 → 宿主 | 空 |

**匹配规则**：精确匹配；`"capture.*"` 匹配该前缀下所有主题；`"*"` 匹配全部。
同一个 `callback` 用多个模式订阅时，会按每个匹配的订阅各回调一次（不是去重）。
第三方扩展用自己的前缀（如 `acme.sharpen.*`），别占用 `shot.*`。

## 5. 生命周期与限制

- 插件必须在 **`deinit` 返回前退订**（`unsubscribe(user)`）；卸载后宿主不再回调。
- 单条负载上限 **64 KiB**；队列上限 **4096** 条，满时 publish 返回 -1（丢弃，不阻塞）。
- **大对象不进总线**：截图帧走单独的 frame/surface 服务，总线上只传 id 与元数据。
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
- [ ] `deinit` 返回前调用了 `unsubscribe`？
- [ ] publish 返回值检查了（队列满会丢事件）？
- [ ] 新增的宿主服务能力已在 `document/ABI_POLICY.md` 登记？
