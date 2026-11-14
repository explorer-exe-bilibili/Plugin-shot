# watermark —— 时间水印 demo

> **用户原话：**"每次截图后，在图片右下角加上拍摄时间水印。"
>
> 一句话需求 → 一个可直接编译安装的插件：`shot.image_processor` +
> `shot.configurable`，纯标准库、不依赖 OpenCV。

## 1. 它做什么

每张截图在流水线处理阶段被原地画上一行本地时间（默认 `2026-11-14 23:59:07`）：

- **内置 5x7 点阵字体**：数字、字母 A-Z（小写自动转大写）、`:` `-` `.` `/` `_` 空格；
  按整数倍放大（1~4），放大后边缘依然锐利，不依赖系统字体/FreeType；
- **白字 + 1px 黑描边**：深色截图、浅色截图都看得清；
- **右下角排版**，距右/下边缘各留可配置的 margin；
- 时间取 `localtime_s`（Windows）/ `localtime_r`（POSIX），线程安全。

## 2. 能力 / 插件形态

| 名 | 说明 |
| --- | --- |
| `shot.image_processor` | `process()` 在采集流水线 worker 线程上被调用，就地改像素 |
| `shot.configurable` | 4 个配置项，设置页自动渲染；`set_config` 可能在宿主 main 线程 |
| metadata id | `demo.watermark` |
| 依赖 | 核心 SDK（`nlohmann-json` 来自 SDK `third_party`，用于 `shot::I18n`） |

实现上遵守 `ShotImage` 契约：**只改像素、不改结构体字段**；按 stride 逐行寻址
（含负 stride = 自底向上存储），每行只写 `width * 4` 字节。

## 3. 配置项

| key | kind | 默认 | 范围 | 说明 |
| --- | --- | --- | --- | --- |
| `enabled` | BOOL | `1` | —— | 关闭后不处理任何截图 |
| `format` | STRING | `%Y-%m-%d %H:%M:%S` | ≤ 64 字节 | strftime 格式串；非法/过长时回退默认格式 |
| `scale` | INT | `2` | 1 ~ 4 | 字号倍率：1 = 5x7 像素，4 = 20x28 像素 |
| `margin` | INT | `16` | 0 ~ 64 | 距右边缘 / 下边缘的像素数 |

## 4. 多语言 key

`plugin.name`（时间水印）/ `plugin.description` / `config.enabled.*` /
`config.format.*` / `config.scale.*` / `config.margin.*`，在
`src/watermark_plugin.cpp` 的 `kLangTables` 里，中英各一张。

## 5. 目录

```text
watermark/
├── CMakeLists.txt
├── metadata.json
├── include/
│   ├── plugin_entry.h        # ABI 胶水数据结构
│   ├── watermark_plugin.h    # 插件状态：配置快照 + 时间格式化
│   ├── watermark_renderer.h  # 右下角绘制（白字 + 描边）
│   └── bitmap_font.h         # 5x7 点阵字体接口
└── src/
    ├── plugin_entry.cpp      # 唯一导出符号 + 模块表 + ABI 回调
    ├── watermark_plugin.cpp  # 配置项、语言表、localtime、调用渲染器
    ├── watermark_renderer.cpp# 逐像素绘制，严格按 stride 寻址
    └── bitmap_font.cpp       # 字形表 + 文本宽度计算
```

## 6. 构建

```bat
:: 在 vcvars64.bat 环境里
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=<SDK 根>
cmake --build build
:: 产物：build/bin/plugins/watermark.dll

:: Debug（独立构建目录）
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSHOT_SDK_ROOT=<SDK 根>
cmake --build build-debug
```

## 7. 自检

```bat
build\Windows\bin\Plugin-shot.exe --test-plugin build\bin\plugins\watermark.dll --json-out watermark.json
```

本机实测（2026-11-14，SDK `dist/plugin-shot-sdk-0.1.0-win64.zip` + MSVC 19.51）：

```text
[PASS] demo.watermark  state=loaded type=image_processor  pass=15 fail=0 skip=2 warn=0
检查汇总: pass=22 fail=0 skip=3 warn=0
结果: PASS（exit 0）
```

自检覆盖：metadata、ABI 加载、能力与声明一致、4 个配置项读写回、i18n 快照、
事件试运行。**像素画得对不对不在自检范围内**，需要真实截图看。

## 8. 安装与手工验收

1. 关闭宿主，把 `build/bin/plugins/watermark.dll` 复制到宿主 `bin/plugins/`；
2. 启动宿主（或 `--capture` 截一张），打开生成的 PNG，右下角应有时间水印；
3. 在设置页改 `scale` / `margin` / `format`，再截一张对比；
4. 把 `enabled` 关掉再截一张，确认水印消失。

## 9. 已知限制 / 刻意不做

- **只支持 ASCII**：内置点阵表没有中文字形；需要中文水印要换字体表或接 FreeType。
- 时间取"处理这张图的时刻"：`ShotImage` 契约里没有采集时间戳，process 紧随采集
  执行，误差在秒级。
- 图像太小放不下整行时**整行不画**（返回处理成功、只记 debug），不会画出半行字。
- 不写入 alpha 混合：水印像素 A 恒为 255（避免在带透明通道的截图里看不见）。
- 不做阴影/底带/自定义颜色/位置（左上/居中）：需要时改
  `watermark_renderer.cpp` 的起点计算即可。
