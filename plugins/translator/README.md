# translator —— 外部语言包插件

读取**外部语言包**（官方 `files/lang` + 用户目录）并贡献到 `i18n_source_list`：
用户在设置页就能选到包提供的语言，包里的译文覆盖内置文案与其它插件文案。
插件 id 是 `translator.main`，只声明一个能力 `shot.configurable`（Generic 插件），
**零 ABI 改动**：不改契约头、不新增宿主服务/能力/事件主题。

擅长的事：

- 一个 JSON 文件就是一门语言的译文，改措辞不用重新编译；
- 用户包覆盖官方包、两者都覆盖内置表（优先级 200 < 300 > 内置 100）；
- 坏包只 WARN 跳过，插件照常加载，绝不把宿主拖垮。

## 目录

```
plugins/translator/
├── CMakeLists.txt              # shot_add_plugin + nlohmann_json（+ Windows shell32/ole32）
├── metadata.json               # id translator.main / abi 2.0 / interfaces ["shot.configurable"]
├── include/                    # 头文件与 .cpp 一一对应（STYLE §6.8）
│   ├── plugin_entry.h          #   内部头：PluginEntry / PluginInstance / state_of
│   ├── pack_types.h            #   ParsedPack：解析后的包（纯数据）
│   ├── pack_dirs.h             #   PackDirs / PackLocation / pathToUtf8：目录解析（唯一平台代码）
│   ├── pack_loader.h           #   PackLoader：单文件解析与校验
│   ├── pack_scanner.h          #   PackScanner：目录 → 包列表
│   ├── source_publisher.h      #   SourcePublisher：合并 + 分块发布 + 撤回
│   ├── translator_config.h     #   TranslatorConfig：两个配置项
│   └── translator_plugin.h     #   TranslatorPlugin：create/destroy 与重扫描编排
└── src/
    ├── plugin_entry.cpp        # 唯一导出符号 shot_module_entry + kEntries 索引目录
    ├── translator_plugin.cpp   # 配置回调 + 语言表 + 重扫描编排 + 插件身份证
    ├── translator_config.cpp   # 配置描述符（label/tooltip 写语言表 key）+ get/set
    ├── pack_dirs.cpp           # exe 目录 / %LOCALAPPDATA% / 配置目录 → 扫描位置
    ├── pack_scanner.cpp        # 枚举 *.json（按文件名排序）+ 逐包解析
    ├── pack_loader.cpp         # JSON 解析、@meta、文本清洗、坏包 WARN
    └── source_publisher.cpp    # (target,key,tag) 合并、56 KiB 分块、撤回
```

## 构建

宿主仓库内：由根 `CMakeLists.txt` 的 `file(GLOB plugins/*/CMakeLists.txt)` 自动
`add_subdirectory`，跟着整个工程一起构建。

```bash
bash tools/verify/build.sh --target translator
```

产物：`build/Windows/bin/plugins/translator.dll`（导入库在 `build/lib`）。

## 目录与优先级

| 目录 | 来源 | priority |
| --- | --- | --- |
| `<exe>/files/lang/*.json` | 官方包（随安装包分发） | 200 |
| `%LOCALAPPDATA%\screen-shot\langs\*.json` | 用户包（升级不覆盖） | 300 |
| 配置项 `pack_dirs` 指定的目录 | 用户显式指定 | 300 |

- `pack_dirs` 为空 = 用上面两个默认目录；非空 = **只用它**，不再读默认目录。
  多个目录用分号分隔（`D:\packs\a;D:\packs\b`），按书写顺序扫描。
- 同一 `(owner, key, tag)`：priority 大者胜；同 priority 时**后扫描到的包**胜
  （目录内按文件名排序，所以结果可复现）。用户包因此天然覆盖官方包。
- exe 目录由 `GetModuleFileNameW` 取（ABI 不暴露数据目录）；非 Windows 用
  `/proc/self/exe`（Linux）或 `_NSGetExecutablePath`（macOS）。用户目录在
  Windows 走 `SHGetKnownFolderPath(FOLDERID_LocalAppData)`，其它平台用 `$HOME/.screen-shot/langs`。

## 包格式

```json
{
  "@meta": { "tag": "ja-JP", "name": "日本語", "fallback": "en-US" },
  "texts": {
    "core":        { "settings.open": "設定", "capture.request": "撮影" },
    "hotkey.main": { "config.capture_hotkey.label": "撮影ホットキー" }
  }
}
```

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `@meta` | 是 | 字符串（= tag），或 `{tag, name, fallback}` 对象；`tag` 必填 |
| `@meta.tag` | 是 | BCP-47 语言标签；`ja_JP` 会被规范化成 `ja-JP` |
| `@meta.name` | 否 | 设置页「语言」下拉里的显示名；缺省用 tag |
| `@meta.fallback` | 否 | 该语言的回退语言；缺省用宿主默认（en-US） |
| `texts` | 是 | owner（`core` 或插件 id）→ 文本 key → 译文 |

校验规则（**坏包只 WARN，绝不让插件加载失败**）：

- 文件不是合法 JSON 对象 / 缺 `@meta` / `@meta` 没有有效 tag / 缺 `texts` /
  一条可用文本都没有 → WARN 并跳过整个包；
- 文件名与 `@meta.tag` 不一致 → WARN，**以 `@meta.tag` 为准**（重命名文件很常见）；
- 文本值是空串 = 这条还没翻（跳过，走宿主的回退链）；不是字符串/键为空/用
  `@meta` 作键 → 计数后 WARN 一次并跳过这些条目；
- 单个文件上限 4 MiB，超过按坏包跳过（语言包不可能这么大，通常是放错文件）；
- `texts` 里的 owner 不是 `core`、也不是当前已加载插件 → WARN 并跳过该 owner：
  宿主只接受这两种 target，提前过滤可以少一条宿主 WARN。

## 发布条目的形状

对每个包贡献一条语言声明，对每个 `texts[owner][key]` 贡献一条文本条目
（`lists.add` 到 `i18n_source_list`）：

```json
{"id":"@language/ja-JP","texts":{"ja-JP":"日本語"},"fallback":"en-US"}
{"id":"settings.open","target":"core","priority":300,"texts":{"ja-JP":"設定"}}
```

- `target`：为哪个 owner 供文本（`core` = 宿主文案）；
- `priority`：200 = 官方包，300 = 用户包/配置目录；宿主按 priority 与内置表（100）合并；
- **一个必须先合并的原因**：宿主按 `(list, owner, id)` 存条目，而本插件只有一个
  贡献者身份（`translator.main`），同一个 `(target, key)` 发两次只会互相覆盖。
  所以插件内先按 `(target, key, tag)` 做一次与宿主 `mergeText` 完全相同的合并
  （priority 大者胜、同 priority 后到者胜），再把结果作为一条条目发布；条目的
  `priority` 取该条胜出候选的最高值。效果与"让宿主合并两个包"一致：
  - 同一 `owner/key/tag`：用户包（300）覆盖官方包（200）；
  - 同一 key 的**不同语言**可以来自不同包，不会被整条覆盖吃掉；
  - 与其它贡献者（插件内置表 100）比较仍由宿主完成，包始终赢。

**分块**：`lists.add` 单条事件负载上限 64 KiB，按 **56 KiB 预算**分块（留 8 KiB
给 `list`/`owner`/`items` 外壳与 JSON 转义），单条条目本身就超限时跳过 + WARN。
`lists.remove` 撤回时省略 `ids`（= 按 owner 整段撤回），不存在 id 数组撑爆的问题。

## 配置项

存在 `config.json` 的 `translator.main/<key>`（宿主按"插件 id/配置 key"落盘）：

| key | kind | 默认值 | 说明 |
| --- | --- | --- | --- |
| `enabled` | BOOL | `"0"` | 是否加载外部语言包。**默认关**：一开就会改写 core 文案，现有 e2e 断言按内置中英文写的 |
| `pack_dirs` | PATH（`options="directory"`） | 空 | 空 = 官方 + 用户两个默认目录；非空 = 只用它（分号分隔多个） |

配置变化（设置页写值、`lists.commit` 落盘）后：`set_config` 只置脏标记，真正的
重新扫描发生在**下一轮 `lists.changed` 回调**（宿主 main 线程）。`set_config` 自己
**不发任何事件**——它可能在任意线程被调用，而 `lists.add` 只允许 main 线程处理。

## 语言包能改什么、不能改什么

- **能改**：所有走 `i18n_list` 解析的文案，即 `display(owner, key)` 的数据——
  托盘菜单项、配置项 label/tooltip、ENUM 选项、`config_list` 的插件分区名
  （`plugin.name`）等；
- **不能改**：插件用 `shot::I18n::tr()` 查**自己本地表**的文案（例如设置页窗口标题
  `settings_ui.window_title`），以及日志、事件主题、配置 key、ENUM 技术值——
  日志固定中文是仓库约定（e2e 按中文日志断言）。
- 本插件自己的配置文案也可以被语言包覆盖：在包里写
  `"translator.main": {"config.enabled.label": "…"}` 即可（owner 写插件 id）。

## 线程模型

- `lists.changed` 回调在宿主 main 线程执行：回调里只做一次原子判位，脏了才扫描；
- 扫描是同步文件 IO（每包上限 4 MiB、只读小文本），不另起线程——为几个 KB 的
  JSON 起线程反而要处理生命周期与队列；
- `set_config` / `get_config` 可能在任意线程调用：`TranslatorConfig` 用
  `std::atomic` + 互斥量保护；扫描只读配置快照，不持锁做 IO；
- `publish` / `withdraw` 只发事件（非阻塞入队），不做实际合并工作。

## 关闭与卸载

- `enabled` 改成 0：下一轮 main 线程重扫描时发现关闭，`lists.remove` 撤回本插件
  贡献的全部条目（"关"必须等于"从没加载过"，否则开关看起来关不掉）；
- 包被删掉/目录换了：重新扫描时**先撤回再发布**，不留"幽灵译文"
  （宿主按 `(target,id)` 存条目，新一批里没有的 key 不会自动清除）；
- `destroy`（禁用插件/宿主退出）：`stop()` 退订 `lists.changed` 并显式
  `lists.remove`。宿主在实例消失后还会自动清一遍；因为它的卸载顺序是"先摘实例
  再 destroy"，此时这条 remove 可能被判成"owner 不是已加载插件"而丢弃——那是
  **预期行为**，自动清理会兜住，不影响正确性。

## 冒烟脚本

`tools/verify/smoke_i18n_translator.ps1`（需交互桌面、单实例宿主）：

```bat
tools\verify\run.bat smoke_i18n_translator.ps1
```

它会备份/恢复 `config.json`、`bin\files\lang\`、`%LOCALAPPDATA%\screen-shot\langs\`，
造两个临时包（官方 + 用户，同一 key 不同文本）和一个坏包，验证：语言包已加载、
`@language` 声明进了语言目录、`i18n_list` 重新发布、托盘 label 显示**用户包**文本、
坏包只 WARN 无 error、关闭插件后回退 en-US。

## 平台说明

- Windows 已随构建与冒烟验证；
- Linux / macOS 只用到 `std::filesystem` + 各自的"取 exe 路径"API，代码可编译，
  但**未实机运行验证**（与其它插件的平台现状一致）；
- 配置目录分隔符固定用分号（Windows 习惯）；POSIX 上目录名本身可以含冒号，
  所以不改成冒号。

## 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 自己写 `config.json` | 存盘键是 `translator.main/<key>`，落盘归宿主（`document/SETTINGS.md`） |
| 在包里支持嵌套/复数/占位符替换 | 宿主协议只有"key → 文本"，多一层语法要么进 ABI、要么只有本插件认，都会漂移 |
| 监听文件系统变化自动重载 | 需要平台 API + 线程；配置改动/重启宿主已经覆盖真实使用场景 |
| 用 ERROR 级日志报坏包 | 语言包是用户可手改的输入，坏包不该被当成宿主的故障（e2e 也断言无 error） |
| 起后台线程扫描 | 包只有几 KB，主线程同步读更简单，也避免 DLL 卸载时的线程收尾问题 |

## 检查清单

- [ ] 改配置项只改 `translator_config.cpp` 的 `kConfig[]` 与 `TranslatorConfig`，没有改宿主代码？
- [ ] 新增/修改配置项时，在 `kLangTables` 的 `zh-CN` / `en-US` 两张表里同时补 key？
- [ ] 改条目形状前先读 `src/hostListCatalog.cpp` 的 `parseI18nSourceItem` / `handleAdd`？
- [ ] 新增日志仍是中文、坏包路径仍只 WARN？
- [ ] `metadata.json` 的 `plugins[]` 顺序仍与 `kEntries` 一致、`interfaces` 与代码一致？
- [ ] 公共头里的类/函数有 Doxygen 注释吗？未完成项写成代码里的 `// TODO:` 了吗？
