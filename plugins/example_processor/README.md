# example_processor —— 示例插件工程

一个最小的**完整**插件工程：一个 DLL 里装两个插件（invert / grayscale），
两个都同时实现了 `shot.image_processor` 和 `shot.configurable`，
用来演示契约、目录结构、能力查询、配置、元数据贴尾和构建规则。

## 目录

```
plugins/example_processor/
├── CMakeLists.txt                      # 只做三件事：找到 shot_add_plugin、声明源文件、声明元数据
├── metadata.json                       # 插件清单：构建期被贴到 DLL 尾部
├── include/
│   └── processors.hpp                  # 行寻址工具 + PluginEntry 结构 + 两个插件的声明
├── src/
│   ├── plugin_entry.cpp                # 唯一的导出符号 shot_module_entry + 索引目录 + 共用语言表
│   ├── invert.cpp                      # 插件 0：能力目录 + 配置 + 实现
│   └── grayscale.cpp                   # 插件 1：同上
└── README.md
```

一个插件一个 `.cpp`：能力声明、配置项、实现都在这一个文件里，想加插件就复制一份。

## 构建

宿主仓库内：由根 `CMakeLists.txt` 自动 `add_subdirectory`，跟着整个工程一起构建。
本插件依赖 OpenCV：configure 时加 `-DSHOT_ENABLE_OPENCV=OFF` 会把本工程整体跳过
（CI / 没有 OpenCV 的环境用这个开关，默认 `ON`）。

离线 SDK 独立构建：

```bash
cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

产物：`build/bin/plugins/example_processor.dll`（导入库在 `build/lib`，pdb 在 `build/pdb/plugins`）。

## 关键点

1. **只导出两个字**：整个 DLL 只导出 `shot_module_entry`，它返回一张自描述的
   模块表 `ShotModule`。加能力是靠往结构体尾部追加字段，不是加导出符号。

2. **能力用字符串问**：宿主调
   `query_interface(instance, "shot.image_processor", 1)` 拿处理函数表，
   问 `"shot.configurable"` 拿配置函数表。没有的能力返回 `NULL`，宿主安静跳过。
   所以本插件完全不需要让宿主知道"我是第几类插件"。

3. **索引即契约**：`src/plugin_entry.cpp` 里 `kEntries` 的顺序就是
   `create(index)` 的索引，必须与 `metadata.json` 的 `plugins[]` 顺序一致。

4. **配置项由插件声明、宿主渲染**：每个配置项是一个 `ShotConfigDesc`
   （类型、key、显示名、默认值、取值范围），值是字符串。宿主没有硬编码任何
   配置项名字；插件遇到不认识的 key 返回负值，安全忽略。

5. **按行寻址**：`ShotImage.stride` 可以为负、也可以大于 `width * channels`，
   所以 `for_each_row()` 逐行算出有效区间，绝不假设 `data` 连续。

6. **元数据贴尾**：`shot_add_plugin()` 在链接后把 `metadata.json` 追加到 DLL 尾部，
   用 `plugin_api.h` 里的 `PLUGIN_META_START_MARKER` / `PLUGIN_META_END_MARKER` 分隔。
   标记字面量只有契约头那一份，构建脚本在构建期直接读头文件取宏值。

## 配置项（`shot.configurable`）

两个插件各有一组配置项，值一律是字符串（契约要求）；不认识的 key 返回负值，
插件保持可用。存盘键是 `<插件 id>/<配置 key>`，落盘与启动回灌归宿主
（`document/SETTINGS.md`），插件只负责 get/set。

**example.invert**（`src/invert.cpp`）：

| key | kind | 默认值 | 取值 | 说明 |
| --- | --- | --- | --- | --- |
| `keep_alpha` | BOOL | `1` | `1` / `0` | 取反时是否保留 A 通道；`0` 表示连 alpha 一起取反 |

**example.grayscale**（`src/grayscale.cpp`）：

| key | kind | 默认值 | 取值 | 说明 |
| --- | --- | --- | --- | --- |
| `mode` | ENUM | `mode.luma` | `mode.luma` / `mode.average` | `luma` = BT.601 亮度加权，`average` = (B+G+R)/3；候选值本身就是文案 key，ENUM 存储值不翻译；旧配置里的 `luma` / `average` 仍然接受 |
| `strength` | INT | `100` | 0..100 | 0 = 原图，100 = 完全灰度 |

`ShotConfigDesc` 的 `label` / `tooltip` 不写中文，写的是语言表 key（文案本体见
「多语言」；拼错时设置页会原样显示 key）：

| 插件 | 配置 key | label key | tooltip key |
| --- | --- | --- | --- |
| example.invert | `keep_alpha` | `config.keep_alpha.label` | `config.keep_alpha.tooltip` |
| example.grayscale | `mode` | `config.mode.label` | `config.mode.tooltip` |
| example.grayscale | `strength` | `config.strength.label` | `config.strength.tooltip` |

## 运行期贡献：给托盘加一条命令

示例插件还演示了「插件不碰宿主 UI 代码、也不新增 ABI 结构体，就能往宿主里加东西」：

- `example.invert` 实例化时（`src/plugin_entry.cpp` 的 `create_instance`）向事件总线
  发布一条 `lists.add` 事件，往 `tray_command_list` 里声明一个条目；点击时发布
  `capture.request`。
- `label` 写的是命令 id（`example.capture`）而不是中文：托盘按
  `display(owner, label)` 用 `i18n_list` 解析成当前语言文本（中文「示例：截屏
  （示例插件）」/ 英文 `Example: Capture (sample plugin)`）。老宿主不生成 `i18n_list`
  时显示 key 本身，这是协议允许的降级。
条目字段：

| 字段 | 值 | 说明 |
| --- | --- | --- |
| `list` | `tray_command_list` | 宿主聚合的托盘菜单列表 |
| `owner` | `example.invert` | 必须等于发布者插件 id |
| `id` | `example.capture` | 同一 owner 内唯一 |
| `label` | `example.capture` | 文案 key（见「多语言」） |
| `event` | `capture.request` | 点击后发布的事件 |
| `data` | 空串 | 整屏采集 |
| `order` | 20 | 排在 core 命令之后 |

- `owner` 必须等于发布者插件 id（`example.invert`）：宿主按它校验来源，并在插件被
  禁用/卸载时自动清理这条贡献，所以禁用 `example_processor` 后菜单项会消失、
  重新启用后回来。
- 托盘插件完全不认识这条命令，它只是把宿主聚合出来的 `tray_command_list` 渲染成菜单；
  点击后发布的 `capture.request` 和 core 自带的「截屏」走同一条采集流水线。
- 一个 DLL 里有两个插件，但这条贡献只属于 `example.invert`；DLL 内的 `static` 标记
  保证只发一次。标记在实例销毁时清掉，DLL 重载后也会自然重置，所以启停
  `example.invert`（哪怕 `example.grayscale` 还在、DLL 没卸载）都能重新声明。

数据形状（有哪些字段、谁排序）不进 ABI，协议见 `document/LISTS.md`。

## 多语言（i18n）

> 语言表同时包含插件自身的 `plugin.name` / `plugin.description`：设置页的插件分区标题按
> `display(插件 id, "plugin.name")` 解析（查不到时回退成 key 本身）。

用户可见文案走 `shot::I18n`（`include/shot_plugin_helpers/shot_i18n.h`），链路零 ABI
改动：插件用 `lists.add` 把语言表发布到 `i18n_source_list`，宿主汇总后生成只读的
`i18n_list`，所有消费方按同一条规则解析（完整协议见 `document/I18N.md`）：

```text
display(owner, key) = i18n_list[(owner, key)] ?? key
```

- **语言表位置**：`src/plugin_entry.cpp` 文件作用域的 `kLangTables[]`，每种语言一个
  JSON 对象，`@meta` 是 BCP-47 语言标签（当前 `zh-CN` / `en-US`）。一个 DLL 里的两个
  插件共用这一份表；两张表的 key 集必须完全一致，每个 key 两种语言都要有文本。
- **加载时机**：`create()` 只有 index、拿不到宿主服务表，所以 `create_instance()` 在
  拿到 `host` 后调用每个 entry 的 `start(state, host)`（`invert.cpp` /
  `grayscale.cpp`）：构造 `std::unique_ptr<shot::I18n>`、owner 用本实例 id
  （`example.invert` / `example.grayscale`），再调用
  `load_language_tables()` → `loadTables(kLangTables, ...)`。两个实例各发布一次同一份
  表，所以设置页按配置项 owner、托盘按 `example.invert` 都能解析到文本。
- **释放时机**：`invert_destroy` / `grayscale_destroy` 先 `i18n.reset()`（析构会退订
  `lists.changed`）再释放状态；不能在宿主卸载 DLL 后还留着订阅。
- **key 规范**：
  - 配置项：`config.<配置 key>.label` / `config.<配置 key>.tooltip`；
  - 插件身份：`plugin.name` / `plugin.description`（设置页分区标题用）；
  - ENUM 候选值本身就是 key（`mode.luma` / `mode.average`），设置页按候选值解析；
  - 托盘命令：直接用命令 id 当 key（`example.capture`）；
  - 本包没有自绘 UI，因此没有 `ui.*` key，也没有 `i18n.tr(...)` 调用点。
- **回退**：老宿主没有列表服务、语言表解析失败时，设置页/托盘按上面的规则回退显示
  key 本身；插件自己的 `tr(key, fallback)` 兜底必须写中文原文（本包当前没有调用点）。
- **不翻译的东西**：日志（`shot::Logger`）保持中文——e2e 按中文日志断言；事件主题
  `capture.request`、配置 key `keep_alpha` / `mode` / `strength`、ENUM 存储值
  `mode.luma` / `mode.average`（本身是文案 key）不翻译。
- **新增一条文案**：在两张语言表里同时加同一个 key，再用 key 引用；只加一种语言时
  `loadTables` 会记 WARN（各语言 key 数不一致），缺文本的语言按回退链取。

文案表：

| key | zh-CN | en-US |
| --- | --- | --- |
| `config.keep_alpha.label` | 保留 alpha | Keep alpha |
| `config.keep_alpha.tooltip` | 取反时跳过 A 通道 | Skip the alpha channel when inverting |
| `config.mode.label` | 模式 | Mode |
| `config.mode.tooltip` | 灰度算法 | Grayscale algorithm |
| `config.strength.label` | 强度 | Strength |
| `config.strength.tooltip` | 0 = 原图，100 = 完全灰度 | 0 = original image, 100 = fully grayscale |
| `example.capture` | 示例：截屏（示例插件） | Example: Capture (sample plugin) |

## 加一个自己的插件

1. 在 `src/` 下新建一个 `.cpp`，照着 `invert.cpp` 写：定义状态结构、
   `process`、可选的配置读写，最后组装 `ShotImageProcessorV1` /
   `ShotConfigurableV1` 两个实例和一张 `kIfaces[]` 目录。
2. 导出一个 `const example_processor::PluginEntry`（名字随意，`plugin_entry.cpp` 引用它）；
   状态里用 `std::unique_ptr<shot::I18n>` 持有语言表，并实现 `start(state, host)`，
   把用户可见文案按 key 加进 `plugin_entry.cpp` 的 `kLangTables`。
3. 在 `plugin_entry.cpp` 的 `kEntries` 末尾加一项，并在 `metadata.json` 的
   `plugins[]` 末尾加对应的 id/name/version —— 两处顺序必须一致。
