# 插件自检（`--test-plugin`）

这份文档写给**插件作者与 CI/skill 维护者**：怎么用宿主自带的自检模式离线验收一个
插件 DLL，输出长什么样、退出码怎么判、哪些问题它查不出来。**改自检逻辑、改 JSON
字段或退出码之前先读这里**；数据结构与命令行的唯一事实来源是
`include/pluginTest.h` / `src/pluginTest.cpp`。

## 1. 它是什么：真实宿主，不是假宿主

`Plugin-shot --test-plugin <dll>` 会把被测 DLL 拷进一个**隔离数据目录**，然后走
`HostRuntime::init()` 的真实加载路径（`PluginManager` 扫描 → 读 metadata → 依赖
拓扑排序 → `LoadLibrary`/`dlsym` → ABI 校验 → `create` → 能力包装 → 服务表注入），
跑完检查后真实卸载（`UI deinit` → `destroy` → `module shutdown` → 释放 DLL）。

因此它验证的是"宿主真的会怎么加载这个插件"，而不是另写一套模拟器。隔离体现在：

- 数据目录 = `<系统临时目录>/plugin-shot-test/<pid>-<毫秒>`，通过环境变量
  `PLUGIN_SHOT_DATA_DIR` 在解析 `core::dataPath` **之前**生效；日志、`config.json`、
  插件输出全部落在这里，不碰用户真实目录；
- 隔离目录的配置里 `plugins.disabled` 被清空、界面语言按 `--language` 写入，避免
  "用户在真实配置里禁用过它 / 换过语言"影响验收结论；
- 结束时删除隔离目录（`--keep-data` 保留）。

## 2. 命令与选项

```bash
# 最小用法：人类可读摘要 + 退出码
Plugin-shot.exe --test-plugin build/Windows/bin/plugins/example_processor.dll

# CI / skill：机器可读 JSON 落文件，人类摘要仍打印到 stdout
Plugin-shot.exe --test-plugin <dll> --json-out build/verify/plugin_test.json

# stdout 只输出 JSON（人类摘要改写入隔离目录的 logs/host.log）
Plugin-shot.exe --test-plugin <dll> --json

# 排错：保留隔离目录（日志、config.json、事件假图都在里面）
Plugin-shot.exe --test-plugin <dll> --keep-data --verbose
```

| 选项 | 含义 |
| --- | --- |
| `--test-plugin <dll>` / `--test-plugin=<dll>` | 必需。相对/绝对路径均可，扩展名必须是本平台插件库（Windows `.dll`；Linux `.so`/`.dylib`） |
| `--json` | stdout **只输出** JSON；人类摘要与插件日志写入日志文件 |
| `--json-out <file>` | 把 JSON 写到文件（父目录不存在时自动创建），可与 `--json` 同时用 |
| `--keep-data` | 保留隔离数据目录并在摘要里打印路径；默认结束时删除 |
| `--timeout-ms <n>` | 整体超时（默认 30000，上限 600000）。插件挂起时看门狗线程硬退出（exit 2） |
| `--language <tag>` | i18n 快照使用的语言标签，默认 `en-US`；写进隔离配置的 `core/language` |
| `--verbose` | 人类摘要里同时打印 PASS/SKIP 明细（默认只打印 FAIL/WARN） |

`--test-plugin` 与 `--capture` / `--settings` **互斥**（同时给出 → 用法错误 exit 2），
且自检不进入常驻主循环。

## 3. 检查项

工具级检查（`checks[]`，不属于某个插件）：

| 检查 id | 说明 |
| --- | --- |
| `metadata` | DLL 尾部 metadata.json 可解析（schema / abi / 插件条目数）。**缺失直接 FAIL**：第三方插件包必须带 metadata |
| `dependencies` | 按 metadata 的 `requires` 在**被测 DLL 同目录**找提供者并一并拷入；找不到记 WARN（宿主会按缺少依赖跳过） |
| `host.init` | `HostRuntime::init` 成功（插件目录可用） |
| `i18n.snapshot` | `i18n_list` 可读、`@meta` 存在，报告生效语言与回退语言 |
| `i18n.language` | `--language` 与生效语言一致；语言不在目录里时按回退链处理，记 WARN |
| `records` | `getInstalledPlugins()` 中存在 `dllPath == 被测 DLL` 的记录 |
| `events.dispatch` | 发布 `capture.done` / `capture.failed` 后派发两轮，自检探针收到事件 |
| `events.stability` | 两轮 `dispatchEvents()` 未崩溃/未挂起 |
| `events.fixture` | （仅失败时出现）事件试运行需要的假图写不出来 |
| `host.exception` | （仅失败时出现）宿主/插件抛出未捕获的 C++ 异常（违反契约） |

每个插件记录（`plugins[]`）依次检查：

| 检查 id | 说明 |
| --- | --- |
| `state` | 记录必须是 `LOADED`；`DISABLED`/`FAILED` 直接 FAIL 并附宿主给出的 reason |
| `metadata.entry` | metadata 里有该插件 id 的声明（对不上只 WARN：宿主以运行期 `get_info` 为准） |
| `metadata.interface.<iface>` | metadata 声明了契约里不存在的接口 id → WARN |
| `metadata.type` | metadata 声明推导出的包装类型 == 宿主实际包装类型；不符 FAIL |
| `metadata.configurable` | 声明了 `shot.configurable` 但实例查询不到 → FAIL；反之未声明却实现 → WARN |
| `capability.image_processor.process` | 64×48 BGRA8（stride=width×4+16）合成图 `process` 返回 true |
| `capability.image_processor.layout` | 处理后 `data/width/height/stride/channels/pixel_format` 未被插件改（契约只允许改像素） |
| `capability.image_processor.stride_padding` | 行尾 16 字节填充未被写入（最常见越界点） |
| `capability.image_processor.tiny` | 1×1 图像不崩、返回 true |
| `capability.image_saver.save` / `.file` / `.size` / `.magic` | 真落盘一张 16×12 图：返回 0、文件存在、大小 > 0、文件头是 PNG/JPEG/BMP |
| `capability.screen_capture.describe` | `describe` 返回 0；返回 1（无显示器）→ **SKIP** |
| `capability.screen_capture.geometry` | 尺寸合法（≤16384）且 `pixel_format = BGRA8` |
| `capability.screen_capture.capture` / `.layout` | 真抓一张：返回 0、布局字段未被改 |
| `capability.screen_capture.pixels` | 缓冲里有非零像素；全 0 可能是锁屏/黑屏 → **WARN**（不判失败） |
| `capability.lib_provider.name` / `.version` | 库名非空；版本非空（不可解析 → WARN） |
| `capability.ui_provider` / `capability.generic` | 只报告类型 → **SKIP**（GUI/通用插件不做能力调用） |
| `config.query` | `query_interface(shot.configurable)` 不抛异常 |
| `config` / `config.list` | 没有该能力 / `config_list` 中没有条目 → SKIP；有条目则 PASS |
| `config.fields` | 每项都有非空 `label`、合法 `kind`(1..7)、`default` 字段 |
| `config.default` | 默认值为空（契约允许）→ WARN，回写测试改用当前值 |
| `config.writeback.<key>` | **默认值回写回读**：发布 `lists.update` → 宿主转 `set_config` → 重建列表 → 回读一致 |
| `config.writeback.publish` | （仅失败时出现）`lists.update` 事件发布失败 |
| `config.unknown_key` | 对未知 key 的 `set_config` 必须返回负值（宿主回执 `ok=false`）；被接受 → WARN |
| `config.commit` | 发布 `lists.commit` 后 `config.json` 真的落盘、回写项都在、收到 `config.applied` |
| `list.tray_command_list` / `list.hotkey_list` / `list.i18n_source_list` | 读宿主真实列表快照，筛 `owner == 插件 id` 的条目做字段校验（缺 `label`/`combo`/`texts` 等只 WARN；没有贡献 → SKIP） |

**全部检查只读隔离环境**：配置回写改的是隔离 `config.json`，事件试运行用的假图写在
隔离目录，屏幕采集若真抓屏也只读桌面。

## 4. 输出

### 4.1 人类可读摘要（stdout）

```text
== Plugin-shot 插件自检（PASS）==
被测 DLL: D:\...\example_processor.dll
隔离数据目录: C:\Users\...\Temp\plugin-shot-test\1234-1700000000000（结束后删除）
语言: en-US | 宿主 ABI 2.0 | Windows x64 Debug
一并拷贝的依赖: opencv_provider.dll
工具/环境检查:
[PASS] example.invert  state=loaded type=image_processor  pass=13 fail=0 skip=1 warn=0
检查汇总: pass=34 fail=0 skip=3 warn=0 | 耗时 52 ms
结果: PASS（exit 0）
```

- 每个插件一行：`state` / 包装类型 / 各状态检查条数；
- FAIL/WARN 逐条打印明细（`--verbose` 时连 PASS/SKIP 一起）；
- 失败时（退出码非 0）会把隔离目录里 `logs/host.log` 的末尾回显到 **stderr**，
  skill 不需要自己去找日志。

### 4.2 JSON（`--json-out` / `--json`）

```json
{
  "tool": "plugin-shot-test",
  "version": "0.2.0",
  "versionFull": "0.2.0.183",
  "abiComment": "宿主 ABI 2.0（x64/Debug）：插件必须用同一 ABI 主版本、同一架构……",
  "hostAbi": { "major": 2, "minor": 0 },
  "build": { "platform": "Windows", "architecture": "x64", "compiler": "MSVC", "type": "Debug" },
  "dll": "D:\\...\\example_processor.dll",
  "copiedDll": "C:\\...\\Temp\\plugin-shot-test\\1234-1700000000000\\plugins\\example_processor.dll",
  "dataDir": "C:\\...\\Temp\\plugin-shot-test\\1234-1700000000000",
  "dataKept": false,
  "language": "en-US",
  "dependencies": ["opencv_provider.dll"],
  "exitCode": 0,
  "result": "pass",
  "checks": [
    { "id": "metadata", "status": "pass", "detail": "metadata.json 可解析：schema=1，abi=2.0，插件条目 2 个" }
  ],
  "plugins": [
    {
      "id": "example.invert",
      "name": "Invert (keep alpha)",
      "version": "1.0.0",
      "state": "loaded",
      "type": "image_processor",
      "declaredInterfaces": ["shot.image_processor", "shot.configurable"],
      "checks": [
        { "id": "capability.image_processor.process", "status": "pass", "detail": "64x48 BGRA8（stride=width*4+16）处理返回 true" }
      ]
    }
  ],
  "summary": { "pass": 34, "fail": 0, "skip": 3, "warn": 0 },
  "durationMs": 52
}
```

| 字段 | 说明 |
| --- | --- |
| `result` | `"pass"` / `"fail"`（等价于 `summary.fail == 0`） |
| `plugins[]` | 被测 DLL 里 **metadata 声明的每个插件**一条（一个 DLL 可以有多条） |
| `plugins[].state` | `"loaded"` / `"disabled"` / `"failed"`；非 loaded 时另有 `stateReason` |
| `plugins[].type` | `image_processor` / `image_saver` / `screen_capture` / `lib_provider` / `ui_provider` / `generic` / `none` |
| `checks[]` | 工具级检查；`plugins[].checks[]` 是逐插件检查 |
| `checks[].status` | `"pass"` / `"fail"` / `"skip"` / `"warn"`（**稳定枚举**） |
| `checks[].id` | 稳定检查 id（见 §3），CI 可按 id 精确断言 |
| `summary` | 全部检查（工具级 + 逐插件）的计数；`fail > 0` ⇒ 退出码 1 |
| `dependencies` | 自检自动一并拷进隔离目录的依赖提供者文件名 |
| `dataKept` | 是否保留了隔离目录（`--keep-data`） |

**约定**：`--json` 时 stdout 只有这一个 JSON 对象（可被 `ConvertFrom-Json` /
`json.loads` 直接解析）；FAIL 明细在 `checks[].detail` 里，不再额外打印到 stdout。

## 5. 退出码

| 码 | 含义 | 典型场景 |
| --- | --- | --- |
| `0` | 没有 FAIL 检查（可以有 SKIP/WARN） | 插件可用；GUI/无显示器场景跳过属正常 |
| `1` | 至少一条 FAIL | 加载失败、能力调用失败、配置回写不一致、metadata 缺失…… |
| `2` | 用法/IO 错误或整体超时 | 参数错误、DLL 不存在、临时目录建不出来、`--json-out` 写不了、看门狗超时 |

WARN **不改变**退出码；CI 需要"WARN 也算问题"时自己读 JSON 的 `summary.warn`。

## 6. 状态含义

| 状态 | 含义 | 处理建议 |
| --- | --- | --- |
| `pass` | 检查通过 | — |
| `fail` | 插件违反契约或加载/调用失败 | 必须修；detail 里有可行动原因 |
| `skip` | **不适用**，不是失败 | 例：无显示器时屏幕采集返回 1；GUI 类插件不做界面检查；插件没有贡献某个列表 |
| `warn` | 可疑但不判失败 | 例：metadata 没声明某个能力、字段缺失、版本号不可解析、屏幕全黑 |

## 7. 局限（**刻意不做**的事）

- **同进程、同地址空间**：插件崩了就是自检进程崩了（拿不到 JSON）。自检不做沙箱，
  也不捕获访问违例（只兜底 C++ 异常）；想要"崩溃隔离"需要独立进程 + 崩溃转储，
  不在本期范围。
- **同架构 / 同 CRT / 同构建配置**：插件必须和宿主用同一 ABI 主版本、同一架构、
  同一构建配置（Debug 宿主只能加载 Debug 插件）。不满足时 Windows 上通常直接
  `LoadLibrary` 失败，自检给的是语言无关的加载失败原因，不会替你做 PE 架构分析。
- **GUI 类插件只做加载检查**：`ui_provider` 在宿主加载时已经 `init` 过（托盘图标等），
  自检不再重复 `init/deinit`、不点击菜单、不验证窗口内容；测 UI 交互请走
  `tools/verify/` 的 e2e 脚本。
- **事件试运行有真实副作用**：`capture.done` 会真的发给所有订阅者——`clipboard` 会写
  系统剪贴板、`notify` 会弹一条系统通知。这是"验证订阅链路"的代价；需要完全安静时
  不要用带订阅者的 DLL 跑自检。
- **依赖提供者只在同目录找**：自检按 metadata 的 `requires` 在被测 DLL 所在目录找
  提供者（插件部署常态），不递归全盘、不查 PATH。找不到时宿主会按缺少依赖跳过，
  JSON 里 `dependencies` 检查给 WARN；这不是插件代码缺陷，是部署环境不完整。
- **配置回写会改隔离配置**：自检把每项的默认值写回并回读，`config.json` 因此是
  "回写后"的状态；它只影响隔离目录，不影响用户真实配置。
- **看门狗超时是硬退出**：`--timeout-ms` 到期时进程直接 `_Exit(2)`，不会清理临时
  目录、不会写 JSON；排错时用 `--keep-data` 并检查 `<临时目录>/plugin-shot-test/`。
- **不替代 `--capture` e2e**：自检能证明"能力回调按契约工作"，但不覆盖"截屏 → 处理 →
  保存 → 通知/剪贴板"的完整链路；那部分由 `tools/verify/smoke_capture.ps1` /
  `e2e_clipboard.ps1` 等真机脚本覆盖。
- **Linux/macOS**：自检代码跨平台（路径、环境变量、动态库扩展名都分了平台分支），
  但本轮只在 Windows 真机验证过；Linux 构建矩阵见 `document/PLATFORMS.md`。

## 8. 推荐用法

**插件作者**：提交前跑一次 `--test-plugin <你的.dll> --verbose`，先修 FAIL；WARN 里
通常是"metadata 写漏了能力声明"这类打包问题。

**CI / skill**：

```powershell
Plugin-shot.exe --test-plugin <dll> --json-out build\verify\plugin_test.json
if ($LASTEXITCODE -ne 0) { 失败；读 JSON 的 checks[].status == "fail" 拿明细 }
```

**排错**：加 `--keep-data`，然后看摘要里打印的隔离目录（`config.json`、`logs/host.log`、
`events/plugin_test_capture.png` 都在里面）。

## 9. 检查清单（改自检代码前）

- [ ] 新的检查 id 是否稳定、能否被 skill 按 id 断言？
- [ ] 失败时 `detail` 是否指向可行动原因（不是"失败了"）？
- [ ] 是否区分了 FAIL 与 SKIP/WARN（不要为了"全绿"把契约问题降级）？
- [ ] 是否仍然走真实宿主路径（`HostRuntime` / `PluginManager` / 列表事件），没有偷偷直连插件？
- [ ] `--json` 时 stdout 是否仍然**只有** JSON（人类摘要进日志）？
- [ ] 退出码是否保持 0/1/2 语义不变？
- [ ] `document/PLUGIN_TEST.md` 与 `tools/verify/smoke_plugin_test.ps1` 是否需要同步？
