# dsh-plugin-shot-dev

把 **Plugin-shot 插件开发技能**（`plugin-shot-dev`）装进 DeepSeek Harness 的一个最小 DSH
插件 bundle。装好之后，你可以直接对 agent 说：

> 给 Plugin-shot 写一个插件：每次截图后在图片右下角加时间水印。

agent 会自己下载 Plugin-shot 离线 SDK、生成插件工程、写代码、编译、用宿主自检模式
`Plugin-shot.exe --test-plugin` 验收，最后告诉你怎么装进宿主。**全程不需要宿主源码，也不需要
装 vcpkg / OpenCV。**

## 这个包是什么

- 只注册一条技能，不注册工具、不影响宿主其它功能；
- 技能正文在 `skills/plugin-shot-dev/`，由仓库里的同名目录同步而来（事实源在 Plugin-shot
  仓库的 `skills/plugin-shot-dev/`，改那边之后重新同步）：
  `rm -rf dsh-plugin-shot-dev/skills/plugin-shot-dev && cp -r skills/plugin-shot-dev dsh-plugin-shot-dev/skills/`；
- 技能与宿主内置技能同一注册路径（`bundled` 来源、rank 600），你自己放的同名技能优先；
- 禁用本插件行或卸载插件，技能即从技能列表消失。

## 安装

任选一种（DSH 的插件管理入口版本不同，文案可能是「插件」「Bundles」「添加插件」）：

1. **本地路径**：设置 → 插件 → 添加插件 → 选择本目录（`dsh-plugin-shot-dev/`）；
2. **开发模式**：在 DSH profile 的 workspace 里 `pnpm add file:<本目录绝对路径>`，重启 DSH；
3. 安装后在「插件」列表里应看到 `dsh-plugin-shot-dev`（技能行），刷新页面/重启后
   技能列表里出现 `plugin-shot-dev`。

要求 DSH 版本支持技能注册表（`ctx.skills.registerProvider`，DSH 0.2.x 起）。

## 验证安装

- 插件页能看到本 bundle 的技能行；
- 对 agent 说「Plugin-shot 插件开发怎么开始」，它应当能说出：下载 SDK → 模板 →
  `cmake` 构建 → `--test-plugin` 自检 的流程。若没有，检查 DSH 日志里
  `dsh-plugin-shot-dev: ...` 开头的告警（技能文件缺失或 frontmatter 不合法）。

## 配套发布物

| 产物 | 用途 |
| --- | --- |
| `plugin-shot-sdk-<版本>-win64.zip` | 离线 SDK 核心包（契约头 + 构建规则 + 第三方头 + 文档 + 模板 + 本技能副本） |
| `plugin-shot-sdk-<版本>-win64-opencv.zip` | 可选 OpenCV 叠加包（解压进核心 SDK 根） |
| `Plugin-shot-<版本>-win64.zip` | 宿主便携版（自检/冒烟/实际使用） |

下载地址见 Plugin-shot 的 GitHub Releases；技能正文 `sdk-acquire.md` 里有完整回退策略。

## 许可

Apache-2.0（与 Plugin-shot 一致）。技能正文与 SDK 文档同源。
