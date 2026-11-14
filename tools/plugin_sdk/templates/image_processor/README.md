# image_processor —— 图像处理器模板（`shot.image_processor` + `shot.configurable`）

> 滤镜 / 水印 / 裁剪这类"就地改像素"插件的起点。示例实现是最直观的**反色**
> （B/G/R 取反、A 保留，按强度混合），改 `process()` 里的逐像素循环即可换成任何算法。

## 1. 定位

| 项 | 值 |
| --- | --- |
| 插件 id（占位符） | `{{PLUGIN_ID}}` |
| 能力 | `shot.image_processor`（就地处理）+ `shot.configurable`（开关 / 强度） |
| 输入 / 输出 | 同一张 `ShotImage`：BGRA8、按 stride 逐行、**只改像素不改结构体字段** |
| 线程 | `process` 在采集流水线 worker 线程；`set_config` 可能在宿主 main 线程 |
| 依赖 | 核心 SDK；不需要 OpenCV / vcpkg |

## 2. 用到的能力 / 数据

| 名 | 用途 |
| --- | --- |
| `SHOT_IFACE_IMAGE_PROCESSOR` | 宿主按加载顺序调用每个处理器的 `process`；非 0 只记 warn、不中断流水线 |
| `SHOT_IFACE_CONFIGURABLE` | 两个配置项在设置页自动出现 |
| `ShotImage` | `pixel_format == SHOT_PIXEL_FORMAT_BGRA8`、`channels == 4`、`stride` 可负/可带行尾填充 |

`process` 的完整校验（源码 `invert_processor.cpp`）：

1. `image` / `data` 非空、`width/height >= 1`；
2. `SHOT_HAS_FIELD(image, ShotImage, pixel_format)` 且等于 BGRA8（老宿主可能没这个字段）；
3. `channels == 4`；
4. `abs(stride) >= width * 4`；
5. 按 `image->data + y * stride + x * 4` 取像素——**不要**把 `data` 当连续块遍历。

## 3. 配置项

| key | kind | 默认 | 范围 | 说明 |
| --- | --- | --- | --- | --- |
| `enabled` | BOOL | `1` | —— | 关闭后不处理任何图像（`process` 直接返回 0） |
| `strength` | INT | `100` | 0 ~ 100 | 0 = 原图，100 = 完全反色，中间值线性混合 |

## 4. 多语言 key

`plugin.name` / `plugin.description` / `config.enabled.label|tooltip` /
`config.strength.label|tooltip`，在 `src/invert_processor.cpp` 的 `kLangTables` 里。

## 5. 目录

```text
{{TARGET}}/
├── CMakeLists.txt
├── metadata.json
├── include/
│   ├── plugin_entry.h       # ABI 胶水数据结构
│   └── invert_processor.h   # InvertProcessor：process + 配置
└── src/
    ├── plugin_entry.cpp     # 唯一导出符号 + 模块表 + ABI 回调
    └── invert_processor.cpp # 语言表、配置读写、逐像素反色
```

## 6. 构建

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=<SDK 根>
cmake --build build
:: 产物：build/bin/plugins/{{TARGET}}.dll
```

Debug 用独立目录：`-B build-debug -DCMAKE_BUILD_TYPE=Debug`。

## 7. 改成你自己的插件

1. `metadata.json` 的 `id` / `name` / `version` / `description`；
2. `src/plugin_entry.cpp` 的 `kInvert` 同步 `id` / `name` / `version`；
3. `src/invert_processor.cpp`：
   - 改 `kConfig`（配置项 key / kind / 默认值 / 范围）与 `getConfig/setConfig` 分支；
   - 改 `kLangTables`（key 必须与 `kConfig.label/tooltip` 一致，中英 key 集一致）；
   - 改 `process()` 的逐像素算法（保持第 2 节的校验与按行寻址）。
4. 想做"每张图只处理一次/按尺寸跳过"之类逻辑，代价小的判断放 `process` 里即可；
   需要缓存的资源放插件状态（`InvertProcessor` 成员），并在 `stop()` 里释放。

## 8. 自检

```bat
build\Windows\bin\Plugin-shot.exe --test-plugin build\bin\plugins\{{TARGET}}.dll --json-out selfcheck.json
```

期望 `结果: PASS（exit 0）`，插件检查里 `type=image_processor`；宿主会构造一张测试图
调用 `process` 并检查返回码。

## 9. 安装到宿主

复制 `build/bin/plugins/{{TARGET}}.dll` 到宿主 `bin/plugins/`，重启宿主。设置页
「插件」列表里会出现本插件与两项配置；处理器按插件**加载顺序**执行，加载顺序由
宿主扫描目录决定，需要严格顺序时把插件命名成能区分先后的名字。

## 10. 已知限制 / 刻意不做

- 只支持 BGRA8；契约目前也只定义了这一种像素格式（新格式需要扩契约）。
- 反色是演示算法，不追求观感；强度混合用整数除法，极端值可能有 1 级误差。
- 不改 alpha、不改尺寸/方向：处理器只该改像素，结构体字段归宿主。
- 不使用 OpenCV：核心 SDK 不保证任何第三方库；要链接 OpenCV 请参考 `lib_provider`
  模板与 SDK 的 OpenCV 叠加包说明。
