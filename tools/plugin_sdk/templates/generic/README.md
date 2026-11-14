# generic —— Generic 插件模板（`shot.configurable` + 事件订阅 + 托盘命令）

> 这是离线 SDK 的**默认起点**：一个不提供图像/采集类具名能力、但"能配置、能看事件、
> 能给托盘加命令"的插件。用它改造成任何"纯逻辑/纯订阅"插件。

## 1. 定位

| 项 | 值 |
| --- | --- |
| 插件 id（占位符） | `{{PLUGIN_ID}}` |
| 能力 | `shot.configurable`（只声明这一项，宿主按 `PluginType::Generic` 兜底包装） |
| 订阅 | `capture.done`（截图落盘后收到 `{"path","width","height","processors"}`） |
| 贡献 | `tray_command_list` 一条命令：点击发 `capture.request`（走宿主截屏流水线） |
| 依赖 | 核心 SDK；不需要 OpenCV / vcpkg |

演示"订阅者 + 配置 + 托盘"三件套，也是理解插件生命周期的最短路径：

```text
create(index, host) → 发布语言表 → 订阅 capture.done → 发布 lists.add（托盘命令）
destroy             → 退订 → 销毁语言表 → delete
```

## 2. 用到的宿主能力 / 事件 / 列表

| 名 | 方向 | 用途 |
| --- | --- | --- |
| `shot.configurable` | 插件 → 宿主 | 声明两个配置项，设置页自动渲染 |
| `capture.done` | 宿主 → 插件 | 截图成功后通知，回调里按配置记日志 |
| `lists.add` | 插件 → 宿主 | 注册托盘命令（`list = "tray_command_list"`），点击时宿主发 `capture.request` |
| `lists.changed` | 宿主 → 插件 | `shot::I18n` 内部订阅，语言切换后重读文案快照 |

## 3. 配置项

| key | kind | 默认 | 说明 |
| --- | --- | --- | --- |
| `log_prefix` | STRING | `[{{TARGET}}]` | 收到截图事件时日志的前缀 |
| `verbose` | BOOL | `1` | 开 = 每次截图记 INFO；关 = 只记 DEBUG |

## 4. 多语言 key

`plugin.name` / `plugin.description` / `config.log_prefix.label|tooltip` /
`config.verbose.label|tooltip` / `{{TARGET}}.capture`（托盘命令文案，命令 id 当 key）。
语言表在 `src/generic_plugin.cpp` 的 `kLangTables` 里，中英各一张；**两张表的 key 集
必须一致**，否则宿主会记一条 i18n 警告。

## 5. 目录

```text
generic/
├── CMakeLists.txt
├── metadata.json
├── include/
│   ├── plugin_entry.h      # ABI 胶水的数据结构（PluginEntry / PluginInstance）
│   └── generic_plugin.h    # GenericPlugin 类：配置 + 订阅 + 托盘
└── src/
    ├── plugin_entry.cpp    # 唯一导出符号 + 模块表 + ABI 回调
    └── generic_plugin.cpp  # 语言表、配置读写、capture.done 解析
```

## 6. 构建

```bat
:: 在 vcvars64.bat 环境里；SDK 根用 -DSHOT_SDK_ROOT 指定
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=<SDK 根>
cmake --build build
:: 产物：build/bin/plugins/{{TARGET}}.dll
```

技能/脚手架生成（推荐）：`cmake -DTEMPLATE=generic -DDEST=<目标目录> -DPLUGIN_ID=... -P <SDK>/tools/new_plugin.cmake`，
详见 `tools/plugin_sdk/README.md` §3。

## 7. 改成你自己的插件

1. `metadata.json`：`id` / `name` / `version` / `description`（`interfaces` 保持
   `["shot.configurable"]`，除非你还实现了别的能力）；
2. `src/plugin_entry.cpp` 的 `kGeneric`：`id` / `name` / `version` 必须与 metadata 一致；
3. `src/generic_plugin.cpp`：配置项 key / 默认值 / 语言表文案，以及 `handleCaptureDone`
   里真正想做的事（**重活不要留在回调里**，拷走数据转自己的线程）；
4. 托盘命令 payload：`id` / `label`（i18n key）/ `event` / `order`，owner 保持插件 id。

## 8. 自检

```bat
build\Windows\bin\Plugin-shot.exe --test-plugin build\bin\plugins\{{TARGET}}.dll --json-out selfcheck.json
```

期望：`结果: PASS（exit 0）`，插件检查里 `type=generic`、`pass>0 fail=0`。
自检会用隔离数据目录真实加载、读回配置、试运行一次 `capture.done` / `capture.failed`。

## 9. 安装到宿主

把 `build/bin/plugins/{{TARGET}}.dll` 复制到宿主 `bin/plugins/`，重启宿主；设置页会
出现一节配置，托盘会多一条命令（`order = 50`）。配置值存在
`%LOCALAPPDATA%\plugin-shot\config.json` 的 `{{PLUGIN_ID}}/log_prefix` 等键下。

## 10. 已知限制 / 刻意不做

- 回调里只解析 JSON + 写日志：这是"最小订阅者"示例，不用它演示线程（见 `demos/archive/`）。
- 托盘命令固定发空负载的 `capture.request`（整屏截图）；区域截图需要带 `region` 负载，
  不属于本模板范围。
- 没有窗口/UI：`shot.ui_provider` 需要自己管理窗口类和线程，见 SDK 文档。
- 不检查 `capture.done` 的 `width/height/processors`：模板只关心 `path`。
