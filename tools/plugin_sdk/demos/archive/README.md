# archive —— 日期归档 demo

> **用户原话：**"截图保存后自动按日期归档到「年-月-日」文件夹。"
>
> 一句话需求 → 一个可直接编译安装的插件：纯订阅者（`shot.configurable` +
> 订阅 `capture.done`）+ 自己的 worker 线程，不依赖 OpenCV。

## 1. 它做什么

每次截图落盘后，把保存插件写回的文件移进（或复制到）`<归档根>/YYYY-MM-DD/`：

```text
截图.png ──capture.done──► 事件回调（只入队）
                              │
                              ▼
                       归档 worker 线程
                         ├─ 建 <根>/2026-11-14/
                         ├─ 重名 → 截图_1.png、截图_2.png
                         ├─ move：rename 优先；失败回退 copy + 删除源文件
                         └─ copy：只复制，原文件保留
```

设计要点：

- **事件回调不做文件 IO**：宿主事件分发线程被阻塞会拖慢所有插件；回调只解析
  `capture.done` 的 `path`、拷进入队缓冲并唤醒 worker（队列上限 64，满了丢新并 WARN）。
- **默认根为空 = 源文件所在目录**：不配置时行为是"就地按日期整理"，不会把用户文件
  移到意外位置。
- **不覆盖同名文件**：依次尝试 `name`、`name_1`、`name_2`……最多 1000 次。
- **move 优先 rename**（同卷原子且快）；跨卷/被占用时回退 `copy_file + remove`；
  任何一步失败都只 WARN 并**保留原文件**。
- **停止顺序**：先退订（不再有新任务）→ 置停止标记并唤醒 worker → 等已入队任务做完
  → join。这样"截图刚落盘就被禁用插件"时不会丢那张图的归档。

## 2. 能力 / 事件 / 插件形态

| 名 | 说明 |
| --- | --- |
| `shot.configurable` | 2 个配置项（根目录、move/copy） |
| `capture.done` | 订阅主题；负载 `{"path","width","height","processors"}`，本插件只读 `path` |
| metadata id | `demo.archive` |
| 宿主包装 | 只声明 `shot.configurable` 的纯订阅者，靠 metadata 才按 `PluginType::Generic` 加载 |

## 3. 配置项

| key | kind | 默认 | 说明 |
| --- | --- | --- | --- |
| `directory` | PATH | 空 | 归档根目录；空 = 源文件所在目录；`options = "directory"` 让设置页弹"选目录" |
| `mode` | ENUM | `mode.move` | 候选值 `mode.move｜mode.copy`（候选值本身是 i18n key） |

## 4. 多语言 key

`plugin.name`（日期归档）/ `plugin.description` / `config.directory.*` /
`config.mode.*` / `mode.move` / `mode.copy`，在 `src/archive_plugin.cpp` 的
`kLangTables` 里，中英各一张。

## 5. 目录

```text
archive/
├── CMakeLists.txt
├── metadata.json
├── include/
│   ├── plugin_entry.h     # ABI 胶水数据结构
│   └── archive_plugin.h   # 插件状态：配置 + 队列 + worker 线程
└── src/
    ├── plugin_entry.cpp   # 唯一导出符号 + 模块表 + ABI 回调
    └── archive_plugin.cpp # 语言表、capture.done 解析、worker 归档逻辑
```

## 6. 构建

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=<SDK 根>
cmake --build build
:: 产物：build/bin/plugins/archive.dll

cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSHOT_SDK_ROOT=<SDK 根>
cmake --build build-debug
```

## 7. 自检

```bat
build\Windows\bin\Plugin-shot.exe --test-plugin build\bin\plugins\archive.dll --json-out archive.json
```

本机实测（2026-11-14，SDK `dist/plugin-shot-sdk-0.1.0-win64.zip` + MSVC 19.51）：

```text
[PASS] demo.archive  state=loaded type=generic  pass=8 fail=0 skip=4 warn=0
检查汇总: pass=15 fail=0 skip=5 warn=0
结果: PASS（exit 0）
```

自检会真实发布一条 `capture.done`；日志里能看到归档确实发生（隔离数据目录内）：

```text
[plugin] [archive] 已订阅 capture.done：截图保存后按日期归档
[plugin] [archive] 已移动：...\events\2026-11-14\plugin_test_capture.png
```

## 8. 安装与手工验收

1. 关闭宿主，把 `build/bin/plugins/archive.dll` 复制到宿主 `bin/plugins/`；
2. 启动宿主，设置页把「归档根目录」选到一个空目录（或留空用源目录）；
3. 截一张图，确认 `<根>/<今天日期>/` 里出现该文件；
4. 把 `mode` 改成 `mode.copy` 再截一张：原位置与新目录各有一份；
5. 对同一秒内连续两张截图检查是否出现 `_1` 后缀（不覆盖）。

## 9. 已知限制 / 刻意不做

- 日期取"处理时刻"的本地时间（`localtime_s` / `localtime_r`），不是文件系统的
  创建时间；截图刚落盘就到，误差在秒级。
- 跨卷 move 是 `copy + 删除`，不是原子操作；复制成功但删源失败时保留两份并 WARN。
- 不做内容去重/哈希、不按星期/月份分多级目录、不做保留期清理：都是策略问题，
  需要时在 `archiveOne()` 里加。
- 只依赖 `capture.done`：没有保存插件时根本不会有这个事件，本插件安静待着。
- 队列上限 64 条，极端情况下会丢任务并 WARN（正常一次截图只有一条事件）。
