# 官方语言包目录

本目录放**随安装包分发**的语言包（官方译文）。构建时整个 `files/` 会被同步到
`bin/files/`（`CMakeLists.txt` 的 `sync_resources`），所以运行时路径是
`<exe 目录>/files/lang/`。

## 优先级

同一个 `(target, key, 语言)` 多来源时的优先级（数值越大越优先）：

| 来源 | 路径 | priority |
| --- | --- | --- |
| 用户语言包 | `%LOCALAPPDATA%\plugin-shot\langs\<tag>.json`（升级不覆盖） | 300 |
| `pack_dirs` 指定目录 | 用户显式指定；非空时**只用它**（分号分隔多个目录，按书写顺序扫描） | 300 |
| 官方语言包 | 本目录（运行时 `<exe 目录>/files/lang/`） | 200 |
| 内置表 | 编译进宿主与各插件 DLL 的 `kLangTables[]` / 宿主文案表 | 100 |

同 priority 后到者胜；用户包可以覆盖官方包与内置的 zh-CN / en-US 文本。
语言包由翻译插件（`plugins/translator`，插件 id `translator.main`）扫描；宿主不直接
读本目录。翻译插件默认**关闭**（`translator.main/enabled=0`），开启后才加载外部包；
`pack_dirs` 非空时只读它指定的目录。

## 文件命名

一个语言一个文件，文件名就是 **BCP-47 语言标签**；只扫描本目录顶层的 `.json` 文件
（扩展名大小写不敏感，不递归；其它文件忽略）：

```text
files/lang/zh-CN.json
files/lang/en-US.json
files/lang/ja-JP.json
```

`@meta.tag` 与文件名不一致时 WARN，以 `@meta.tag` 为准。

## 当前官方包

本目录当前随安装包提供 `zh-TW.json`（繁體中文，台湾用语；`@meta.fallback` 为
`zh-CN`），覆盖宿主 `core` 与全部内置插件的 zh-CN 文案。启用翻译插件
（`translator.main/enabled=1`）后，语言下拉里的「繁體中文」即显示繁体文案；
包内缺文本时按回退链落到 zh-CN。

## 文件格式

```json
{
  "@meta": { "tag": "ja-JP", "name": "日本語", "fallback": "en-US" },
  "texts": {
    "core":        { "settings.open": "設定", "capture.request": "撮影" },
    "hotkey.main": { "config.capture_hotkey.label": "撮影ホットキー" }
  }
}
```

- UTF-8；文件开头允许 UTF-8 BOM（读取方剥掉，不能把 BOM 带进 tag/key）。
- `@meta` 可以是字符串 `"ja-JP"`（只给 tag），也可以是对象
  `{"tag","name","fallback"}`：`tag` 必填，`name` 是语言下拉显示名（缺省用 tag；
  对内置标签会覆盖宿主注册表里的原生名），`fallback` 是该语言缺文本时的下一站
  （缺省用注册表 / `en-US`）。
- `texts`：`owner`（`core` 或插件 id）→ `key` → 文本。
- **空串 = 缺失**：按回退链取别的语言，不要用空串表示"故意显示空白"。
- 文件里不写 `target` / `priority`：翻译插件按 owner 生成 `target`，按目录来源生成
  `priority`（官方 200 / 用户包与 `pack_dirs` 300）。
- 只有 `*.json` 会被扫描；一个文件里没解析出任何可用文本时只 WARN，不影响其它包。

## 翻译插件会贡献什么

`files/lang/ja-JP.json` 会被翻译插件转成下面这些 `i18n_source_list` 条目
（`document/I18N.md` §3.3）：

```json
[
  {"id":"@language/ja-JP","texts":{"ja-JP":"日本語"},"fallback":"en-US"},
  {"id":"settings.open","texts":{"ja-JP":"設定"},"target":"core","priority":200},
  {"id":"config.capture_hotkey.label","texts":{"ja-JP":"撮影ホットキー"},"target":"hotkey.main","priority":200}
]
```

- `@language/<tag>` 只声明语言（显示名 / fallback），不是文本条目。
- `target` 是文本服务的 owner：`core` 或**当前已加载**的插件 id；给未安装插件准备的
  条目会被宿主丢弃并 WARN，不会出现在 `i18n_list` 里。
- `priority`：官方目录 200、用户目录与 `pack_dirs` 300、内置表 100；同
  `(target, id, 语言)` 高者胜，同 priority 后到者胜。
- `@meta` 是 `i18n_source_list` 的保留 id，不能当文本 key。

## 校验与错误处理

- 坏 JSON / 缺 `@meta` / 缺 `texts` / tag 为空 / 没有可用文本：WARN 并跳过该文件
  （或该条），翻译插件继续加载其它包，插件本身不失败。
- 单个文件超过 4 MiB：按坏包跳过并 WARN（语言包通常只有几 KB，多半是放错文件）。
- `target` 不是 `core` 或已加载插件：宿主丢弃该条并 WARN。
- `priority` 不是整数：WARN，按缺省 100 处理。
- 单条事件负载上限 64 KiB（翻译插件 / helper 按 56 KiB 预算分块）；单条 item 自身
  超限时跳过并 WARN。
- 完整协议（回退链、合并规则、`i18n_list` 结构）见
  [`document/I18N.md`](../../document/I18N.md) §2、§3.2、§3.3。
