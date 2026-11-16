# 能力决策：需求 → 能力 / 事件 / 列表

> 本文件随 Plugin-shot 离线 SDK 分发（SDK `skill/plugin-shot-dev/references/`）。
> 接口以 SDK `docs/` 内快照为准；宿主源码版 `document/` 是上游源。

## 1. 三步判断

1. **要改像素吗？** 要 → `shot.image_processor`（`image_processor` 模板）。
2. **不改像素、只在截图后做事？** → Generic 订阅者（`generic` 模板），订阅 `capture.done`。
3. **要换"保存"本身（格式/目录/命名/质量）？** → `shot.image_saver`（`image_saver` 模板）。
   注意：内置保存器先加载，换保存器通常要禁用内置 `image_saver.main`，影响所有截图。

| 需求原话（示例）                                         | 实现位置                                                                            | 模板                        |
| -------------------------------------------------------- | ----------------------------------------------------------------------------------- | --------------------------- |
| "右下角加时间水印" / "打码" / "模糊" / "调色" / "加边框" | `ShotImageProcessorV1::process` 原地改像素                                        | `image_processor`         |
| "按日期归档到文件夹"                                     | 订阅 `capture.done`，拿 `path` 移动/复制                                        | `generic`                 |
| "另存一份宽度 1280 的图"                                 | 订阅 `capture.done`，自带 stb 解码→缩放→写文件                                  | `generic`（SDK 已含 stb） |
| "截图后复制到剪贴板 / 弹通知"                            | 订阅 `capture.done`（剪贴板/通知 API 自己调）                                     | `generic`                 |
| "托盘多一个菜单项"                                       | `create` 里 `lists.add` 到 `tray_command_list`                                | `generic` 片段            |
| "功能热键（如长截图/自定义动作）"                        | `create` 里 `lists.add` 到 `hotkey_list`（**不要自己 RegisterHotKey**） | `generic` 片段            |
| "加几个设置项"                                           | 实现 `shot.configurable`                                                          | 任意模板                    |
| "把 OpenCV 带进进程供别的插件用"                         | `shot.lib_provider` + metadata `provides`                                       | `lib_provider`            |
| "换保存格式/目录/命名"                                   | `shot.image_saver` + `shot.configurable`                                        | `image_saver`             |
| "自定义窗口/覆盖层/托盘"                                 | `shot.ui_provider`                                                                | 无模板                      |
| "替换抓屏（GDI → 别的 API）"                            | `shot.screen_capture`                                                             | 无模板                      |

## 2. 能力契约速查（ABI 2.0）

| interface id             | 结构体                   | 关键语义                                                                                                                                             |
| ------------------------ | ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| `shot.image_processor` | `ShotImageProcessorV1` | `process(self, ShotImage*)`：就地改像素，返回 0 = 成功；必须按 `stride` 逐行寻址，`stride` 可为负                                              |
| `shot.image_saver`     | `ShotImageSaverV1`     | `save(self, image, directory, basename, path, path_size)`：`0`=落盘、`1`=参数非法、`2`=path 缓冲不足、`3`=保存失败；成功时写回最终绝对路径 |
| `shot.screen_capture`  | `ShotScreenCaptureV1`  | `describe` 报虚拟桌面几何（物理像素），`capture` 写宿主分配的 BGRA8 缓冲；插件从不分配图像内存                                                   |
| `shot.configurable`    | `ShotConfigurableV1`   | 声明配置项 + 字符串读写；未知 key 返回负值；宿主自动渲染进设置页                                                                                     |
| `shot.lib_provider`    | `ShotLibProviderV1`    | 只报告库名/版本；`metadata.provides` 提供依赖名                                                                                                    |
| `shot.ui_provider`     | `ShotUIProviderV1`     | 宿主只调 `init`/`deinit`/`post_to_plugin`；窗口/线程全归插件                                                                                   |

宿主服务：`shot.host.config`、`shot.host.event_bus`、`shot.host.plugin_lists`、
`shot.host.plugin_registry`、`shot.host.board_msg`（旧接口）。
插件侧封装（`#include "shot_plugin_helpers.h"`）：`shot::Logger` / `shot::Config` /
`shot::EventBus` / `shot::PluginLists` / `shot::PluginRegistry` / `shot::I18n`。

## 3. 事件与负载（订阅/发布）

| 主题                | 方向             | 负载                                                                                                                                       |
| ------------------- | ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `capture.request` | 任意 → 流水线   | 空 = 整屏；`{"mode":"region","region":{x,y,width,height}}` = 裁剪；`{"mode":"region"}` = 请求框选（流水线忽略）                        |
| `capture.done`    | 流水线 → 订阅者 | `{"path":"...","width":1920,"height":1080,"processors":2}`；`path` 是**最终绝对路径**，`path` 可能为空（保存器没写回时只警告） |
| `capture.failed`  | 流水线 → 订阅者 | `{"stage":"capture"\|"save","reason":"..."}`                                                                                              |
| `config.applied`  | 宿主 → 插件     | 空；一批设置提交完成，插件可据此重算派生状态                                                                                               |
| `lists.changed`   | 宿主 → 订阅者   | `{"list":"<list id>"}`                                                                                                                   |
| `plugins.changed` | 宿主 → 插件     | JSON：插件启停结果                                                                                                                         |
| `settings.open`   | 托盘 → 设置页   | 空                                                                                                                                         |
| `app.quit`        | 任意 → 宿主     | 空；插件应响应并清理                                                                                                                       |

限制：单条负载 ≤ **64 KiB**，队列 4096；订阅回调只在宿主 **main 线程**执行；回调里不要阻塞。

## 4. 列表贡献配方（托盘 / 热键）

```cpp
// 托盘命令：create() 里发一次；owner 必须是自己的插件 id
shot::EventBus bus(host);
bus.publish("lists.add", R"json({
  "list": "tray_command_list",
  "owner": "acme.tools",
  "items": [{"id":"acme.capture","label":"acme.capture","event":"capture.request",
             "data":"","order":20,"separator":false}]
})json");

// 功能热键：只贡献数据，注册由 hotkey.main 负责
bus.publish("lists.add", R"json({
  "list": "hotkey_list",
  "owner": "acme.tools",
  "items": [{"id":"acme.snap","combo":"Ctrl+Alt+T","event":"acme.snap.start",
             "data":"","label":"acme.snap"}]
})json");
```

- `label` 允许是字面量或文本 key（托盘/hotkey 按 `display(owner,label)` 解析）。
- 插件卸载/禁用时宿主按 owner 自动清理贡献；重新启用会重新 `create`，**要重新发**。
- 同一 DLL 多插件时，用静态标记保证每条贡献只发一次，并在实例销毁时重置。

## 5. 能力边界话术（不要写代码，照着说）

| 需求                | 对用户说                                                                                |
| ------------------- | --------------------------------------------------------------------------------------- |
| 录屏 / GIF / 多帧   | "当前插件契约只支持单帧截图，录屏需要宿主新增采集能力，得改宿主源码，离线 SDK 做不到。" |
| 新增一种能力/改 ABI | "契约是只追加的，新增能力要改宿主并重编，不是插件能做的。"                              |

## 6. 选错能力的典型后果

- 用 processor 做"归档"：拿不到文件路径，只能改内存里的图 → 白做。
- 用订阅者做"水印"：拿不到像素（`capture.done` 只有路径和尺寸），还得自己解码 → 多余且慢。
- 自己 `RegisterHotKey`：与 `hotkey.main` 抢注册、卸载不干净 → 用 `hotkey_list`。
- 新增 ABI 字段/服务表达菜单项：违反"数据形状不进 ABI" → 用 `lists.add`。

相关文档（SDK `docs/`）：`AUTHORING.md`、`CAPTURE.md`、`EVENT_BUS.md`、`LISTS.md`、
`I18N.md`、`SETTINGS.md`、`METADATA.md`、`ABI_POLICY.md`。
