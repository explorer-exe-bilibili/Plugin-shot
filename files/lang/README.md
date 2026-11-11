# 官方语言包目录

本目录放**随安装包分发**的语言包（官方译文）。构建时整个 `files/` 会被同步到
`bin/files/`（`CMakeLists.txt` §19 的 `sync_resources`），所以运行时路径是
`<exe 目录>/files/lang/`。

## 文件命名

一个语言一个文件，文件名就是 **BCP-47 语言标签**：

```text
files/lang/zh-CN.json
files/lang/en-US.json
files/lang/ja-JP.json
```

## 优先级

优先级从高到低（同一 `(owner, key, 语言)` 后写覆盖先写）：

1. **用户语言包**：`%LOCALAPPDATA%\screen-shot\langs\<tag>.json`（升级不覆盖）
2. **官方语言包**：本目录 `<exe 目录>/files/lang/`
3. **内置表**：编译进宿主与各插件 DLL 的 `kLangTables[]` / 宿主文案表

所以用户可以直接改本地语言包来调整措辞，不需要重新编译。

## 文件格式

```json
{
  "@meta": { "tag": "ja-JP", "name": "日本語" },
  "texts": {
    "core":        { "settings.open": "設定", "capture.request": "撮影" },
    "hotkey.main": { "config.capture_hotkey.label": "撮影ホットキー" }
  }
}
```

- `@meta.tag`：必填，与文件名一致（不一致时 WARN，以 `@meta.tag` 为准）。
- `@meta.name`：可选，语言下拉里的显示名；缺省用语言注册表里的原生名。
- `texts`：`owner`（插件 id，宿主是 `core`）→ `key` → 文本。
- 文本按 key 精确匹配；未命中按 `core/language` 的回退链处理。

完整协议（回退链、校验规则、错误处理）见
[`document/I18N.md`](../../document/I18N.md)。
