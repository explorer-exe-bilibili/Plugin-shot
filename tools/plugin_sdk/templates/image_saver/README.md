# image_saver —— 图像保存模板（`shot.image_saver` + `shot.configurable`）

> "截图该存成什么文件"由这个插件决定：格式、质量、目录都是它自己的配置项，宿主
> 不硬编码任何保存策略。示例用 SDK 内置的 **stb_image_write**（header-only，不需要
> vcpkg / OpenCV）。

## 1. 定位

| 项 | 值 |
| --- | --- |
| 插件 id（占位符） | `{{PLUGIN_ID}}` |
| 能力 | `shot.image_saver`（落盘）+ `shot.configurable`（format / quality / directory） |
| 支持格式 | PNG（无损、保留 alpha）/ JPEG（有损、丢 alpha）/ BMP（不压缩） |
| 输入 | `ShotImage`（BGRA8），`save` 在采集流水线 worker 线程调用 |
| 依赖 | 核心 SDK 的 `third_party/include/stb_image_write.h`（SDK 模式自动加入 include 路径） |

## 2. 用到的能力 / 契约细节

| 名 | 说明 |
| --- | --- |
| `ShotImageSaverV1::save` | 返回值固定：`0` 成功、`1` 参数非法、`2` path 缓冲不足、`3` 保存失败 |
| `directory` / `basename` 覆盖参数 | 非 NULL 时优先；NULL 用插件配置 / 时间戳文件名（`basename` 不含扩展名） |
| `path` 出参 | 成功时写回**最终**路径（UTF-8，含扩展名）；装不下返回 2，不截断 |
| stride | 可负、可带行尾填充：先按行拷成连续 RGBA 缓冲再交给 stb |
| BGRA → RGBA | stb 的输入是 RGBA 顺序；JPEG 只送 3 通道（不带 alpha） |

文件名规则：默认 `shot_YYYYMMDD_HHMMSS`；同名文件自动加 `_1` / `_2` 后缀，**不覆盖**
已有截图。目标目录不存在时自动创建。

## 3. 配置项

| key | kind | 默认 | 说明 |
| --- | --- | --- | --- |
| `format` | ENUM | `format.png` | 候选值 `format.png｜format.jpeg｜format.bmp`（候选值本身是 i18n key） |
| `quality` | INT | `90` | 1 ~ 100，只对 JPEG 生效 |
| `directory` | PATH | 空 | 空 = 当前工作目录；`options = "directory"` 让设置页弹"选目录" |

## 4. 多语言 key

`plugin.name` / `plugin.description`；`config.format.label|tooltip` +
`format.png|format.jpeg|format.bmp`（ENUM 候选文案）；`config.quality.*`；
`config.directory.*`。全部在 `src/image_saver_plugin.cpp` 的 `kLangTables` 里，
中英 key 集必须一致。

## 5. 目录

```text
{{TARGET}}/
├── CMakeLists.txt
├── metadata.json
├── include/
│   ├── plugin_entry.h        # ABI 胶水数据结构
│   └── image_saver_plugin.h  # ImageSaverPlugin：save + 配置
└── src/
    ├── plugin_entry.cpp      # 唯一导出符号 + 模块表 + ABI 回调
    └── image_saver_plugin.cpp# stb 编码、路径/重名处理、配置读写
```

## 6. 构建

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=<SDK 根>
cmake --build build
:: 产物：build/bin/plugins/{{TARGET}}.dll
```

`ShotPlugin.cmake` 在 SDK 模式下自动把 `<SDK>/third_party/include` 加进编译路径，
所以**不需要** `find_package(Stb)`；SDK 包里没有 vcpkg，也不应该去找。

## 7. 改成你自己的插件

1. `metadata.json` 的 `id` / `name` / `version` / `description`；
2. `src/plugin_entry.cpp` 的 `kSaver` 同步 `id` / `name` / `version`；
3. `src/image_saver_plugin.cpp`：`kConfig`（配置项）、`kLangTables`（文案）、
   `encode()`（换编码器/格式）、`defaultBasename()`（文件名模板）；
4. 需要覆盖扩展名时改 `formatExtension()`；新增格式要同时加 ENUM 候选值与 i18n 文案。

## 8. 自检

```bat
build\Windows\bin\Plugin-shot.exe --test-plugin build\bin\plugins\{{TARGET}}.dll --json-out selfcheck.json
```

期望 `结果: PASS（exit 0）`，`type=image_saver`。宿主会构造测试图调用 `save`，
并检查落盘与 `path` 回写。

## 9. 安装到宿主

复制 `build/bin/plugins/{{TARGET}}.dll` 到宿主 `bin/plugins/`，重启宿主。保存格式/
目录/质量在设置页改，落盘位置会出现在 `capture.done` 的 `path` 与通知/剪贴板插件里。

## 10. 已知限制 / 刻意不做

- 目录为空时用**当前工作目录**：模板为了不依赖平台 API（Windows "图片"库需要
  shell32）。正式插件建议默认填用户图片目录。
- 只写 PNG/JPEG/BMP；没有元数据（EXIF/sRGB 标记）、没有 DPI、没有渐进式 JPEG。
- 时间戳格式写死 `shot_%Y%m%d_%H%M%S`，没有做成配置项。
- 文件名重名去重上限 1000 次；超过就报"保存失败"（返回 3）而不是覆盖。
- JPEG 质量只在 1~100 内夹取；非法字符串保留原值（不报错，设置页显示旧值）。
