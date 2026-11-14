# Plugin-shot 插件开发手册（离线 SDK）

> **适用**：你拿到了 Plugin-shot 宿主程序和 `plugin-shot-sdk-<ver>-win64.zip`，**没有宿主源码**、
> 没有 vcpkg、没有预装 OpenCV。目标是写出一个能装进宿主 `plugins/` 的 DLL。
>
> **事实源**：本文与 SDK `include/` 契约头同版本。SDK 版本、ABI、`host_min_version`、
> `contract_sha256` 见 SDK 根 `manifest.json`；与本文冲突时以 `include/plugin_api/*.h`
> 和 `manifest.json` 为准。
>
> 本文是 SDK 文档的单一事实源，发布时原样拷进 SDK 的 `docs/AUTHORING.md`；
> 同目录还有 `METADATA.md`（元数据）、`ABI_POLICY.md`（契约规则）、`LISTS.md`（列表协议）、
> `EVENT_BUS.md`（事件总线）、`CAPTURE.md`（采集流水线）、`I18N.md`（多语言）、
> `SETTINGS.md`（设置页）、`PLATFORMS.md`（平台矩阵）。

## 目录

1. [目标与边界](#1-目标与边界)
2. [选择能力](#2-选择能力)
3. [第一个插件](#3-第一个插件)
4. [plugin_entry 与 ABI 约定](#4-plugin_entry-与-abi-约定)
5. [配置项](#5-配置项)
6. [多语言（i18n）](#6-多语言i18n)
7. [列表贡献（托盘 / 热键）](#7-列表贡献托盘--热键)
8. [事件与线程](#8-事件与线程)
9. [构建](#9-构建)
10. [自检](#10-自检)
11. [安装与卸载](#11-安装与卸载)
12. [日志与排错](#12-日志与排错)
13. [能力边界（做不到什么）](#13-能力边界做不到什么)
14. [版本与兼容](#14-版本与兼容)

---

## 1. 目标与边界

**能做什么**（都通过 `include/plugin_api.h` 的契约，不改宿主）：

| 能力 | interface id | 你能得到什么 |
| --- | --- | --- |
| 图像处理 | `shot.image_processor` | 截图落盘前拿到 BGRA8 像素，可原地修改 |
| 屏幕采集 | `shot.screen_capture` | 提供整屏/虚拟桌面截图（替换或补充内置采集） |
| 图像保存 | `shot.image_saver` | 决定格式/目录/命名/质量（替换或补充内置保存） |
| 可配置 | `shot.configurable` | 声明配置项，宿主自动渲染进设置页并持久化 |
| 运行库提供者 | `shot.lib_provider` | 只管把 OpenCV 之类运行库带进进程 |
| UI 提供者 | `shot.ui_provider` | 自己建窗口/托盘，宿主只转发生命周期事件 |

**触发与扩展面**：订阅事件（`capture.request/done/failed`、`settings.open`、`config.applied`、
`lists.changed`、`plugins.changed`、`app.quit`）；贡献数据列表（`tray_command_list` 托盘菜单、
`hotkey_list` 热键、`i18n_source_list` 文案）；调用宿主服务（config / event_bus /
plugin_lists / plugin_registry）。**数据形状不进 ABI**：要新增一类 UI 数据就定义新 list id +
JSON schema，不新增结构体/服务。

**不能做什么**：录屏/多帧（契约单帧）；OCR/识别（无引擎、无模型）；标注编辑器（重型 UI）；
上传（需要凭据与网络策略，能做但不是"一句话"）；改采集/区域/多屏逻辑或新增能力 = 改宿主源码。
详见 [§13](#13-能力边界做不到什么)。

**插件是什么样的文件**：一个 `.dll`，导出唯一符号 `shot_module_entry`；一个 DLL 可含多个插件；
`metadata.json` 在构建期被贴到 DLL 文件尾（复制/改名都不会丢）。

---

## 2. 选择能力

按需求从上往下判断：

| 用户想要 | 用这个 | 模板 |
| --- | --- | --- |
| 截图后改像素（水印/时间戳/打码/模糊/调色/边框） | `shot.image_processor` | `image_processor` |
| 截图后做别的事（归档/复制/上传/通知/跑命令/缩图另存） | Generic 订阅 `capture.done` | `generic` |
| 换保存格式/目录/命名/质量 | `shot.image_saver` | `image_saver` |
| 加托盘菜单项 / 功能热键 | `lists.add`（`tray_command_list` / `hotkey_list`） | `generic` 片段 |
| 加设置项 | `shot.configurable` | 任意 |
| 把 OpenCV 带进进程供别的插件用 | `shot.lib_provider` | `lib_provider` |
| 自定义窗口/覆盖层/托盘、替换抓屏 | `shot.ui_provider` / `shot.screen_capture` | 无模板 |

判断流程：

```text
需要改截图像素？ ── 是 → image_processor
        │否
        ▼
需要换"保存"行为？ ── 是 → image_saver（注意：内置保存器先加载，要生效通常得禁用内置）
        │否
        ▼
截图后要做事？ ── 是 → Generic 订阅者（capture.done 只给路径+尺寸，像素不在事件里）
        │否
        ▼
只想加菜单/热键/设置项？ → generic 片段 + shot.configurable
        │
        ▼
要窗口/托盘/替换抓屏？ → ui_provider / screen_capture（工作量最大，先确认）
```

模板与占位符细节见 SDK `templates/` 与 `tools/new_plugin.cmake`；两个完整示例
`demos/watermark`（右下角时间水印）与 `demos/archive`（按日期归档）位于 SDK `demos/`。

---

## 3. 第一个插件

### 3.1 准备 SDK

解压 `plugin-shot-sdk-<ver>-win64.zip`（校验 `.sha256`），SDK 根应有：

```text
manifest.json  README.md  LICENSE  NOTICE  THIRD_PARTY_LICENSES/
include/         契约头 + shot_plugin_helpers（不含 opencv4）
third_party/     include/nlohmann/ 整树 + 扁平 stb_*.h
cmake/           ShotPlugin.cmake 等构建模块
docs/            本手册与其它规范
templates/       generic / image_processor / image_saver / lib_provider
demos/           watermark / archive（可直接编译的完整示例工程）
skill/           DSH 技能副本（plugin-shot-dev）
tools/new_plugin.cmake  脚手架
opencv/          ← 仅解压了 OpenCV 叠加包才有：include/lib{/debug}/bin{/debug}
```

### 3.2 用脚手架生成工程

```bash
cmake -DTEMPLATE=generic \
      -DDEST="D:/work/acme_archive" \
      -DPLUGIN_ID=acme.archive \
      -DPLUGIN_NAME="ACME 归档" \
      -DPLUGIN_DESCRIPTION="截图保存后按日期归档到子目录" \
      -DVERSION=1.0.0 \
      -DOWNER="ACME" \
      -P "<SDK>/tools/new_plugin.cmake"
```

占位符：`{{TARGET}}`（目标/DLL 名，id 的 `.`→`_`）、`{{PLUGIN_ID}}`、`{{PLUGIN_NAME}}`、
`{{PLUGIN_DESCRIPTION}}`、`{{VERSION}}`、`{{OWNER}}`。先生成再改代码；生成后 `grep -R "{{"` 应为空。

### 3.3 目录结构

```text
acme_archive/
├── CMakeLists.txt        # list(APPEND CMAKE_MODULE_PATH <SDK>/cmake) + shot_add_plugin(...)
├── metadata.json         # 身份 + ABI + 能力 + 依赖
├── README.md             # 功能/构建/配置/验证/限制
├── include/              # 自己的头文件
└── src/
    ├── plugin_entry.cpp  # 唯一导出 shot_module_entry：模块表、能力目录、create/destroy
    ├── <业务>.cpp        # 业务实现
    └── lang_tables.cpp   # 每语言一张 JSON 表
```

`CMakeLists.txt` 核心（模板已写好，不要写 `find_package` / vcpkg）：

```cmake
list(APPEND CMAKE_MODULE_PATH "${SHOT_SDK_ROOT}/cmake")
include(ShotPlugin)
shot_add_plugin(acme_archive
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp src/archive_plugin.cpp src/lang_tables.cpp
    INCLUDE_DIRS include
)
```

`shot_add_plugin()` 统一负责：隐藏符号、`/MD`、产物落 `build/bin/plugins/`、构建期校验
metadata、把 metadata 贴到 DLL 尾、剪枝产物目录。SDK 模式会自动补 `third_party/include`
与可选 `opencv/` 的 include/link 路径。

### 3.4 metadata.json 字段

```json
{
  "schema": 1,
  "abi": { "major": 2, "minor": 0 },
  "plugins": [
    {
      "id": "acme.archive",
      "name": "ACME 归档",
      "version": "1.0.0",
      "description": "截图保存后按日期归档到子目录",
      "interfaces": ["shot.configurable"],
      "requires": ["core>=0.2.0"],
      "provides": []
    }
  ]
}
```

| 字段 | 说明 |
| --- | --- |
| `schema` | metadata 格式版本（当前 1），与 ABI 无关 |
| `abi.major/minor` | 必须与 SDK `manifest.abi` 一致（major 相等、minor ≤） |
| `plugins[].id` | 全局唯一，`作者.名字`；配置键用它（`<id>/<key>`），发布后不要改 |
| `plugins[].name` | 界面显示名（设置页分区标题会按 i18n 的 `plugin.name` 覆盖） |
| `plugins[].version` | 插件自己的语义化版本 |
| `plugins[].description` | 一行说明（插件列表显示） |
| `plugins[].interfaces` | 你打算实现的能力 id；必须与 `query_interface` 实际回答一致 |
| `plugins[].requires` | 依赖：`core>=<host_min_version>`、`opencv>=4.12` 等；见 `METADATA.md` |
| `plugins[].provides` | 你向进程提供的库名（只写名字，不写版本），如 `["opencv"]` |

**`plugins[]` 顺序必须等于 `create(index)` 的索引顺序。** 运行期贡献（托盘/热键/文案）不写进
metadata，它们在 `create` 里用 `lists.add` 发布。字段语义详见 SDK `docs/METADATA.md`。

---

## 4. plugin_entry 与 ABI 约定

### 4.1 唯一导出符号

```cpp
#include "plugin_api.h"
#include "shot_plugin_helpers.h"

extern "C" {
SCREENSHOT_API const ShotModule* shot_module_entry(void);
}
```

- 一个 DLL **只导出** `shot_module_entry`（`CXX_VISIBILITY_PRESET hidden` 已由
  `shot_add_plugin` 设好；该符号靠 `SCREENSHOT_API` 导出）。
- 不要定义 `SCREENSHOT_HOST_EXPORTS`（那是宿主 exe 用的），否则符号导出会丢。
- 返回静态存储的 `ShotModule`，**不要返回栈/临时对象**，其内容在 DLL 卸载前不可变。

### 4.2 模块表与实例

```cpp
const ShotModule kModule = {
    .struct_size = sizeof(ShotModule),
    .abi_major = SHOT_ABI_MAJOR,     // 必须等于宿主；当前 2
    .abi_minor = SHOT_ABI_MINOR,     // 大于宿主只提示，插件需降级
    .plugin_count = 1,               // 本 DLL 的插件个数；可为 0
    .init = &module_init,            // 可 NULL；返回非 0 = 本宿主上不工作
    .shutdown = nullptr,             // 可 NULL；所有实例 destroy 之后调用
    .get_info = &get_info,           // 可 NULL；填 ShotPluginInfo
    .create = &create_instance,      // 必填；返回不透明实例指针（插件分配）
    .destroy = &destroy_instance,    // 必填；释放实例全部资源
    .query_interface = &query_instance,  // 可 NULL；按 id/version 回能力函数表
};
```

- 生命周期顺序：`module->init(host, 0)` → 每个索引 `create(index, host)` →（运行）→
  `destroy` → `module->shutdown()` → 卸载 DLL。
- `create/destroy` 必须成对；宿主绝不 `delete` 插件的实例。
- `init` 里不要创建依赖屏幕/UI 的东西（`--capture`、无桌面场景也会走加载）。

### 4.3 能力目录与函数表

```cpp
namespace {
int32_t process_image(void* /*self*/, ShotImage* image);   // 业务实现

const ShotImageProcessorV1 kProcessorOps =
    SHOT_OPS(ShotImageProcessorV1, .process = &process_image);  // struct_size/version 自动填

const ShotIfaceEntry kIfaces[] = {
    {SHOT_IFACE_IMAGE_PROCESSOR, 1, &kProcessorOps},
    {nullptr, 0, nullptr},   // 必须以此结尾
};

void* query_instance(void* handle, const char* id, uint32_t version) {
    return handle == nullptr ? nullptr : shot_query_interface(kIfaces, id, version);
}
}  // namespace
```

- `shot_query_interface()` 只回答"不高于请求版本"的能力；老宿主请求 1 时返回版本 1 的表。
- 没有的能力返回 `NULL`，宿主安静跳过；**不要**为了多拿服务假装有能力。
- `struct_size` 漏填 = 整张表不存在（宿主用 `SHOT_HAS_FIELD` 判否）。

### 4.4 跨边界纪律

- 结构体只能在**末尾追加**字段；读任何字段前用 `SHOT_HAS_FIELD`；跨边界只用定宽类型。
- 异常绝不跨 `extern "C"` 边界：每个边界函数内部 `try/catch(...)` 并返回错误码。
- 谁分配谁释放：插件分配的对象由插件的 `destroy` 释放。
- `/MD`、Debug/Release 必须与宿主一致（`shot_add_plugin` 已强制 `/MD`；配置要自己对齐）。
- 一个 DLL 多个插件：`plugins[]` 顺序 = 索引顺序；`metadata` 里每个插件一条；部分禁用时
  DLL 不卸载，"只发一次"的贡献要用静态标记 + 销毁时重置。

---

## 5. 配置项

实现 `shot.configurable` 后，宿主把配置项自动收进 `config_list`、设置页自动出现一节、
值自动落盘并在启动时回灌——**不需要改宿主**。

```cpp
const ShotConfigDesc kConfigs[] = {
    // struct_size, kind, key, label, default, options, min, max, step, tooltip
    {sizeof(ShotConfigDesc), SHOT_CONFIG_KIND_BOOL, "enabled", "启用", "1",
     nullptr, 0, 0, 0, nullptr},
    {sizeof(ShotConfigDesc), SHOT_CONFIG_KIND_INT, "strength", "强度", "50",
     nullptr, 0, 100, 1, "0 = 原图，100 = 最强"},
    {sizeof(ShotConfigDesc), SHOT_CONFIG_KIND_ENUM, "format", "格式", "format.png",
     "format.png|format.jpg", 0, 0, 0, "输出格式"},
    {sizeof(ShotConfigDesc), SHOT_CONFIG_KIND_PATH, "directory", "目录", "",
     "directory", 0, 0, 0, "留空 = 宿主默认保存目录"},
};

uint32_t get_config_count(void*) { return std::size(kConfigs); }

int32_t get_config_desc(void*, uint32_t index, ShotConfigDesc* out) {
    if (out == nullptr || index >= std::size(kConfigs)) {
        return 1;
    }
    const uint32_t callerSize = out->struct_size;  // 调用方填的长度
    *out = kConfigs[index];                        // 先写本 SDK 认识的字段
    out->struct_size = callerSize;                 // 保持调用方视角
    return 0;
}

int32_t get_config(void* self, const char* key, char* buf, uint32_t buf_size);
int32_t set_config(void* self, const char* key, const char* value);
```

规则：

| 项 | 规则 |
| --- | --- |
| `kind` | `SHOT_CONFIG_KIND_BOOL/INT/FLOAT/STRING/ENUM/COLOR/PATH` |
| 值 | 一律字符串：BOOL `"1"/"0"`，INT/FLOAT 十进制，ENUM 存候选值本身 |
| `options` | ENUM 用 `a|b|c`；PATH 写 `directory`/`file`（浏览对话框类型）；STRING 写 `hotkey` 变热键录入控件；以 `#` 开头变颜色控件 |
| ENUM 候选 | **候选值就是 i18n key**（插件命名空间，如 `format.png`），查不到译文原样显示 |
| 未知 key | `get/set` 必须返回负值且不影响其它项（宿主升级会多传项） |
| 范围 | `min/max/step` 只对 INT/FLOAT 有意义 |
| 线程 | `set_config` 可能在 main 线程被调用，能力回调在 worker；共享值加锁或原子量 |
| 副作用 | 单值副作用放 `set_config`；需要"整批生效后重算"就等 `config.applied` |
| 持久化 | 宿主用 `<插件 id>/<key>` 存盘；插件**不要**自己写配置文件 |

设置页控件映射与 `options` 语义见 SDK `docs/SETTINGS.md`。

---

## 6. 多语言（i18n）

宿主用"数据列表 + 事件"做多语言，插件侧用 `shot::I18n` 封装，零 ABI 改动。

```cpp
// 每语言一张单行 JSON：@meta 必填
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"ACME 归档",)json"
    R"json("plugin.description":"截图保存后按日期归档",)json"
    R"json("config.directory.label":"归档目录",)json"
    R"json("config.directory.tooltip":"留空 = 源文件同级 archive/",)json"
    R"json("format.date":"按日期",)json"
    R"json("format.date.tooltip":"年-月-日 子目录"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"ACME Archive",)json"
    R"json("plugin.description":"Archive screenshots into date folders",)json"
    R"json("config.directory.label":"Archive folder",)json"
    R"json("config.directory.tooltip":"Empty = archive/ next to the source",)json"
    R"json("format.date":"By date",)json"
    R"json("format.date.tooltip":"Year-month-day subfolder"})json",
};

// create() 里（拿到 host 之后、宿主读 config_list 之前）：
m_i18n = std::make_unique<shot::I18n>(host, "acme.archive");
m_i18n->loadTables(kLangTables, std::size(kLangTables));

// 取自己的文案：
const char* label = m_i18n->tr("config.directory.label", "归档目录");
// 解析别人的文本 key（i18n_list 缓存；老宿主原样返回）：
const char* text = m_i18n->resolve(owner, item.label);
```

key 规范：

| key | 用途 |
| --- | --- |
| `plugin.name` / `plugin.description` | 设置页插件分区标题/插件表描述 |
| `config.<key>.label` / `config.<key>.tooltip` | 配置项显示名与悬停提示（`<key>` 就是 desc 的 key） |
| ENUM 候选值（如 `format.png`） | 下拉候选的显示文本（候选值本身当 key） |
| 托盘命令 id（如 `acme.capture`） | 菜单文字（托盘按 `display(owner,label)` 解析） |

- `@meta` 必填：字符串标签（`"zh-CN"`）或对象（`{"tag":"ja-JP","name":"日本語","fallback":"en-US"}`）。
- 发布时机：`create`/`init` 里一次；重复调用会先撤回旧贡献。
- 缺失语言走回退链：当前语言 → 主语言族（zh-TW→zh）→ en-US → 第一张表。
- 语言表也可以放在插件目录的 JSON 文件里由外部语言包提供（翻译插件协议），插件只需贡献
  `i18n_source_list`；完整规则见 SDK `docs/I18N.md`。

---

## 7. 列表贡献（托盘 / 热键）

托盘和全局热键都是**数据驱动**的：功能插件只贡献数据，消费方（托盘插件 / `hotkey.main`）
渲染并发布事件。插件间不互相调用。

### 7.1 托盘菜单 `tray_command_list`

```cpp
// create() 成功后发一次（同一 DLL 有多插件时用 static 标记保证只发一次）
shot::EventBus bus(host);
const bool ok = bus.publish("lists.add", R"json({
  "list": "tray_command_list",
  "owner": "acme.archive",
  "items": [
    {"id":"acme.archive.open","label":"acme.archive.open",
     "event":"acme.archive.open","data":"","order":60,"separator":false}
  ]
})json");
```

| 字段 | 必填 | 含义 |
| --- | --- | --- |
| `list` | ✅ | `"tray_command_list"` |
| `owner` | ✅ | **必须等于自己的插件 id**（宿主校验并据此清理） |
| `items[].id` | ✅ | 条目唯一标识（owner 内），发布后不要改 |
| `items[].label` | ✅ | 菜单文字：字面量或文本 key（按 `display(owner,label)` 解析） |
| `items[].event` | ✅ | 点击时发布的事件主题 |
| `items[].data` | 可选 | 事件负载；字符串原样发布，对象/数组序列化成 JSON 文本 |
| `items[].order` | 可选 | 升序排序键，缺省 1000 |
| `items[].separator` | 可选 | `true` = 分隔线（不可点击） |
| `items[].default` | 可选 | `true` = 左键单击托盘图标时执行（取排序后第一个） |

点击后你的插件（或任何订阅者）会收到 `event` 主题；例如让托盘项触发一次截图：
`event: "capture.request"`。要处理自定义动作就订阅自己的主题。

### 7.2 功能热键 `hotkey_list`

```cpp
bus.publish("lists.add", R"json({
  "list": "hotkey_list",
  "owner": "acme.archive",
  "items": [{"id":"acme.archive.snapshot","combo":"Ctrl+Alt+A",
             "event":"acme.snapshot","data":"","label":"acme.snapshot"}]
})json");
```

| 字段 | 必填 | 含义 |
| --- | --- | --- |
| `id` | ✅ | 稳定标识；同时是设置页配置键 `hotkey.main/<id>`，owner 间避免同名 |
| `combo` | ✅ | 默认组合键（`Ctrl+Alt+S` 语法；用户可在设置页改） |
| `event` | ✅ | 按下时发布的事件主题 |
| `data` | 可选 | 负载；空串 = 空负载 |
| `label` | 可选 | 设置页显示文本 key（解析 owner 是 `hotkey.main`，见 `LISTS.md` §5.5） |

**不要自己 `RegisterHotKey`**：全局热键只由 `hotkey.main` 注册；它读 `hotkey_list`、按
`lists.changed` 整批重注册。插件被禁用/卸载时宿主自动清掉条目。

规则：`owner` 必须是自己；`items` 里每条都要有非空字符串 `id`；宿主生成的列表
（`config_list` / `i18n_list` / `plugin_list`）不能写；单条事件负载 ≤ 64 KiB。
完整 schema 与 upsert/order/清理规则见 SDK `docs/LISTS.md`。

---

## 8. 事件与线程

### 8.1 用事件总线

```cpp
// 订阅（create()/init() 里）：
shot::EventBus bus(host);
bus.subscribe("capture.done", &on_capture_done, this);

// 回调签名（宿主 main 线程执行）：
void on_capture_done(void* user, const char* /*topic*/, const void* data, uint32_t len) {
    auto* self = static_cast<MyPlugin*>(user);
    // 只解析 + 入队；文件/网络操作转到自己的 worker 线程
    self->enqueue(std::string(static_cast<const char*>(data), len));
}

// 退订（destroy() 里）：bus.unsubscribe(this);
```

- 主题匹配：精确（`capture.done`）、前缀通配（`capture.*`）、全局（`*`）。
- 负载已由宿主深拷贝，回调返回后仍可安全读取；回调里不要阻塞、不要做耗时任务。
- `publish()` 非阻塞：队列满返回失败（丢弃），要检查返回值。
- 单条上限 64 KiB；像素不进事件——`capture.done` 只给 `path/width/height/processors`。
- 订阅必须在 `destroy`/`deinit` 返回前退订，否则卸载后回调会踩已卸载代码。

### 8.2 `capture.done` / `capture.failed` 负载

```json
{"path":"C:\\Users\\me\\Pictures\\shot_20261114_231500.png","width":1920,"height":1080,"processors":2}
{"stage":"capture","reason":"..."}   // stage = "capture" | "save"
```

- `path` 是**最终绝对路径**（保存器写回的）；可能为空（保存器没写回时宿主只警告）。
- `width/height` 是最终图像尺寸（区域截图是裁剪后的）。
- `processors` 是成功处理过这张图的处理器个数。
- `capture.failed.stage`：`"capture"`（抓屏失败）或 `"save"`（无保存插件/保存返回非 0）。

### 8.3 线程模型

| 代码位置 | 线程 | 注意 |
| --- | --- | --- |
| `image_processor::process`、`image_saver::save`、`screen_capture::describe/capture` | 采集 worker（串行、一次一张） | 不做 UI、不阻塞；按 stride 逐行写 |
| `shot.configurable::set_config` | 宿主 main 线程（启动回灌/设置页） | 与 worker 共享的状态要同步 |
| 事件订阅回调 | 宿主 main 线程 | 只入队/拷贝 |
| 插件自建线程 | 自己的 | `destroy` 里停 + `join` |
| `shot.ui_provider::post_to_plugin` | 宿主任意线程 | 返回前拷走 `ShotEvent::data` |

**禁止**在 `create`/`init`/`deinit` 里同步等待 `lists.update`/`lists.commit` 的结果（宿主持有
生命周期锁，互等会卡死）。只读的 `list_json` 任意线程可调。

---

## 9. 构建

### 9.1 前提

- CMake ≥ 3.20（建议 3.28+）、Ninja、MSVC v143+（VS2022/VS18）＋ Windows SDK、C++23。
- **不需要** vcpkg、OpenCV 安装、Python、git、网络。
- Windows 下 `cl` 不在 PATH 是正常的：构建前在 cmd 里 `call vcvars64.bat`，然后跑 cmake。

### 9.2 命令

```bat
:: Windows（vcvars64 环境里）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT="D:\sdk\plugin-shot-sdk-0.2.0-win64"
cmake --build build
```

```bash
# Linux/macOS：官方离线 SDK 目前只发布 win64；在宿主源码仓库或自建 Linux SDK 时命令同形
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT="$SDK"
cmake --build build -j
```

- 产物：`build/bin/plugins/<target>.dll`（Linux `.so`）；metadata 已在 POST_BUILD 贴尾。
- `SHOT_SDK_ROOT` 也可以设环境变量；不传时模板自动探测 `../../manifest.json`（在 SDK 内构建）。
- Debug：`-DCMAKE_BUILD_TYPE=Debug`。**Debug 插件只能进 Debug 宿主**；OpenCV 用 `*d` 变体，
  由 SDK 模块按 `$<CONFIG>` 自动选 `opencv/lib/debug`。
- 多配置生成器（VS）：`CMAKE_BUILD_TYPE` 无效，请给每个配置独立 build 目录。
- 升级 SDK：整体替换 SDK 目录 + 重新配置/构建；不要新旧头混用。

### 9.3 无 vcpkg 的依赖来源

| 依赖 | 来源 |
| --- | --- |
| nlohmann-json | SDK `third_party/include/nlohmann/`（整树，`shot_plugin_helpers` 必需） |
| stb（PNG/JPEG 编解码） | SDK `third_party/include/stb_*.h`（**扁平**文件名） |
| OpenCV | 可选叠加包 `opencv/`（`-DSHOT_SDK_ROOT` 不变，SDK 自动检测 `opencv/manifest.json`） |
| 其它第三方 | 优先 header-only 自带；不要引入需要系统安装的库，也不要把基础 DLL 装进插件目录 |

### 9.4 常见编译错误

| 现象 | 原因 | 修法 |
| --- | --- | --- |
| `C1083: "nlohmann/json.hpp"` | 没走 SDK 模式 / SDK 不完整 | 传 `-DSHOT_SDK_ROOT`；确认 `third_party/include` 存在；不要手工拷头 |
| `C1083: "stb_image_write.h"` | stb 未进 include | 同上；stb 是扁平文件名 |
| `C1083: "opencv2/core.hpp"` | 没解压 OpenCV 叠加包 | 解压 `opencv/` 到 SDK 根 |
| `LNK2019: ... cv::debug_build_guard::...` | Release/Debug 混用 | Debug 用 `*d` 库与 `lib/debug`，整工程重配 |
| `LNK2019: ... shot_module_entry` | 导出宏/`extern "C"` 丢失 | 照模板恢复；不要定义 `SCREENSHOT_HOST_EXPORTS` |
| `LNK4098` | `/MT` 与 `/MD` 混用 | 用 `shot_add_plugin`（已设 `/MD`） |
| metadata 校验失败 | id 重复 / interface 拼错 / abi 不符 / requires 语法 | 按报错改 metadata |
| `这个工程还是模板` | 占位符没替换 | 用 `new_plugin.cmake` 或手工改 `{{TARGET}}` |
| LNK1104 文件被占用 | 宿主在运行/杀毒扫描 | 关闭宿主后重试 |

---

## 10. 自检

宿主程序自带自检模式：**用真实宿主运行时加载被测 DLL**，在隔离数据目录里跑，不碰用户配置。

```bash
Plugin-shot.exe --test-plugin build/bin/plugins/acme_archive.dll --json-out report.json
```

```text
用法: Plugin-shot --test-plugin <dll> [选项]
  --json                把结构化 JSON 输出到 stdout（人类摘要改写进日志）
  --json-out <file>     把结构化 JSON 写到文件（可与 --json 同时用）
  --keep-data           保留隔离数据目录，便于排错（默认结束时删除）
  --timeout-ms <n>      整体超时毫秒数，默认 30000
  --language <tag>      i18n 快照使用的语言标签，默认 en-US
  --verbose             人类摘要里同时打印 PASS/SKIP 明细
退出码: 0=无失败 1=至少一条失败 2=用法/IO 错误或超时
```

自检做什么：把被测 DLL（及同目录能解析到的依赖提供者）拷进隔离插件目录 → 设
`PLUGIN_SHOT_DATA_DIR` 指向临时目录 → 走真实的 `HostRuntime`（扫描/校验/依赖/加载 + 真实
服务表）→ 逐插件检查 metadata、模块表、能力行为、配置项回写、列表贡献、i18n → 真实卸载并
重载一次（验证热卸载）→ 清理隔离目录（`--keep-data` 时保留）。

### 10.1 读 JSON

```json
{
  "result": "pass",
  "exitCode": 0,
  "summary": { "pass": 42, "fail": 0, "skip": 3, "warn": 1 },
  "checks": [ { "id": "host.load", "status": "pass", "detail": "..." } ],
  "plugins": [
    { "id": "acme.archive", "state": "loaded", "type": "generic",
      "declaredInterfaces": ["shot.configurable"],
      "checks": [ { "id": "config.readback", "status": "pass", "detail": "..." } ] }
  ]
}
```

判定：`result == "pass"`（或 exitCode 0）。失败项从 `plugins[].checks[]` / `checks[]` 里
`status == "fail"` 取 `id` + `detail` 定位。

### 10.2 PASS / SKIP / WARN 含义

| 状态 | 含义 | 怎么处理 |
| --- | --- | --- |
| `pass` | 检查通过 | 不用管 |
| `fail` | 不符合契约 | **必须修**；按 `detail` 改代码/元数据后重跑 |
| `skip` | 不适用（无显示器、UI 插件、GUI 副作用等） | 不是失败；在交付说明里解释 |
| `warn` | 可疑但可容忍（字段缺失、版本号不可解析、可选约定未满足） | 应修；不阻断，交付时列出 |

常见 FAIL（检查 id 是稳定机器标识，按前缀定位；以报告实际 id 为准）：`metadata.*`
（缺 metadata/JSON 非法/ABI 不匹配）、`host.*`（隔离宿主初始化或加载失败）、
`capability.*`（能力返回非 0、越界写、字段被改）、`config.*`（desc 非法、get/set 回读不一致、
未知 key 未返回负值）、`list.*`（`lists.add` 的 owner/条目非法、写了宿主生成的列表）、
`events.*`（`capture.done/failed` 试运行时回调异常）。`detail` 里是中文原因，`--keep-data`
可保留隔离目录里的日志。

连续 3 轮修不动：停下，把 `report.json` 的 FAIL 条目与已尝试修改告诉用户，不要靠改宿主/系统
设置绕过。

---

## 11. 安装与卸载

### 11.1 安装

1. 关闭宿主；备份 `plugins/`（记录将新增/覆盖的文件名）。
2. 把 `<target>.dll` 复制到**宿主 exe 所在目录的 `plugins/`**：
   - 不是数据目录、不是 `bin/`；
   - 不要带任何基础依赖 DLL（`opencv_*.dll` / `fmt` / `spdlog` / `glfw3` 等），
     需要 OpenCV 就声明依赖宿主已有的 provider。
3. 启动宿主。新插件**默认启用**；只有 `config.json` 里 `plugins.disabled` 数组列出的才禁用。
4. 冒烟：`Plugin-shot.exe --capture` → exit 0，日志有 `Capture saved:`；`--settings` 能看到
   插件与配置项；`host.log` 有插件 id、无 error。
5. 测试完恢复 `config.json` 备份（若测试改过配置）。

卸载：关闭宿主 → 删除该 DLL → 重启。可选：清掉 `config.json` 里该插件 id 的配置键（不清也
只是残留数据）。不要删内置/别人装的 DLL。

### 11.2 数据目录与文件位置

| 项 | 路径 |
| --- | --- |
| 数据目录 | Windows `%LOCALAPPDATA%\plugin-shot\`；Linux `~/.plugin-shot/` |
| 日志 | `<数据目录>/logs/host.log`（UTF-8） |
| 配置 | `<数据目录>/config.json`（含 `plugins.disabled`、`configItems`、`config.schema`） |
| 插件 | 宿主 exe 同级的 `plugins/` |
| 隔离模式 | 环境变量 `PLUGIN_SHOT_DATA_DIR` 可覆盖数据目录（自检模式内部使用） |

---

## 12. 日志与排错

### 12.1 写日志

```cpp
shot::Logger log(host);          // 绑定宿主服务，老宿主自动降级（安静丢弃）
log.info("acme.archive: ready");
log.debug("archived " + path);
```

- 日志落在 `host.log`，logger 名是 `plugin`（行格式 `[时间] [级别] [plugin] 正文`）。
- 日志正文建议带插件 id 前缀，便于 grep；**日志不参与翻译**，用固定语言（中文/英文）。
- 日志里不要打印凭据/完整用户路径（隐私）。

### 12.2 排错顺序

1. 插件有没有加载：`host.log` 搜插件 id；`plugin.state.*` / `plugin.error.*` 行。
2. ABI 是否匹配：`host.log` 里 `abi` 相关行；插件 `metadata.abi` 与 SDK `manifest.abi`。
3. 依赖是否满足：`requires` 里 `core` / `opencv` 的提供者是否存在、版本是否够。
4. 事件有没有到：临时订阅 `*`（开发期）看 `capture.done` 是否发布；检查主题拼写。
5. 效果有没有：用 `--capture` 单次复现，确认输出路径与文件内容；检查是否被内置插件覆盖。
6. 崩溃/卡死：先移除该 DLL 复现宿主正常，再缩小到具体回调；检查越界写、异常跨边界、
   未 `join` 的线程、未注销的窗口类。

### 12.3 常见现象

| 现象 | 先查 |
| --- | --- |
| 宿主列表里没有插件 | DLL 不在 `plugins/`、ABI major 不符、无 metadata、架构不对 |
| 插件在但被禁用 | `config.json` 的 `plugins.disabled`；设置页插件页 |
| 加载后没效果 | 主题拼错、订阅晚于发布（`create` 前已发布的事件收不到）、process/save 返回非 0 |
| 设置页没有配置项 | 没实现 `shot.configurable`；`get_config_count` 为 0；插件未加载 |
| 界面显示 key 而不是文案 | 语言表未发布/发布晚于宿主读列表；key 拼错；`@meta` 缺失 |
| 托盘菜单不出现 | `lists.add` owner 不是自己；`list`/`id`/`event` 为空；发布时宿主的列表已建好但只在下次重建生效 |
| 截图后进程崩 | 像素越界/variable stride 写错、异常跨边界、在 worker 线程操作 UI |
| 退出卡住 | `destroy` 里等宿主、线程未 join、同步等 commit |

---

## 13. 能力边界（做不到什么）

| 需求 | 结论 | 原因 / 替代 |
| --- | --- | --- |
| 录屏 / GIF / 多帧 | ❌ | 契约只有单帧采集；需要宿主新增能力 |
| OCR / 识别 / AI 推理 | ❌ | SDK 不含模型运行时；自带模型属于定制且体积大 |
| 标注 / 画框 / 图片编辑器 | ❌（可做但重） | 需要完整 UI 插件（窗口、交互、撤销），不适合一句话交付 |
| 上传图床/网盘 | ⚠️ 有条件 | 需要用户提供地址/凭据/重试策略，且以用户权限联网 |
| 改区域截图 / 多屏 / 热键注册逻辑 | ❌ | 在宿主核心或内置插件里，离线 SDK 改不了 |
| 新增一种能力 / 改 ABI | ❌ | 契约属宿主源码；插件只能使用已发布的 interface id |
| 替换保存器/采集器 | ⚠️ 影响全局 | 内置插件先加载；要生效通常需禁用内置插件，影响所有截图 |
| 定时截图/监控 | ⚠️ 定制 | 插件可自带定时器，但要定义间隔、停止条件与失败策略 |
| 读取截图像素做处理（订阅者） | ❌ | `capture.done` 只给路径与尺寸；要像素用 `image_processor` |

**边界话术**（对用户）：

- "录屏当前做不到：插件契约只支持单帧截图，录屏需要宿主新增采集能力。"
- "OCR 需要模型/引擎，离线 SDK 不包含；这属于定制开发。"
- "上传可以做，但需要你提供接口和凭据，并确认允许插件联网；建议先做本地归档。"
- "改区域截图/多屏逻辑必须改宿主源码，插件做不到。"

---

## 14. 版本与兼容

三条版本轴，不要混用：

| 轴 | 当前值 | 载体 | 变化规则 |
| --- | --- | --- | --- |
| 宿主版本 | 0.2.0 | exe / git tag | 产品发布节奏 |
| SDK 包版本 | 与宿主同版本 | zip 名 / `manifest.sdk_version` | 与宿主同步发布 |
| ABI 版本 | **2.0** | `include/plugin_api/basics.h`、`metadata.abi`、`ShotModule` | 追加能力 = minor+1；破坏 = major+1 |

加载规则：

- 宿主**只接受 `abi_major` 相等**的插件（major 不等 → 整个 DLL 跳过）。
- `abi_minor` 大于宿主：照常加载，但宿主只请求自己认识的版本；插件必须能在老宿主上降级
  （用 `SHOT_HAS_FIELD` / `query_interface` 判空）。
- `metadata.abi` 构建期校验：major 相等、minor ≤ SDK 头 minor；建议写"实际需要的最低 minor"，
  老宿主上更安全。
- 旧 SDK 插件 → 新宿主：major 相同即可用（append-only 契约保证）。
- 新 SDK 插件 → 旧宿主：可能缺少新字段/能力，插件必须容错。

`manifest.json` 关键字段（下载/安装时记录到交付信息）：

| 字段 | 含义 |
| --- | --- |
| `sdk_version` | SDK 包版本 |
| `host_min_version` | 首次携带该 ABI 的宿主版本（插件可写 `requires: ["core>=…"]`） |
| `abi.major/minor` | 契约版本 |
| `contract_sha256` | 契约头整份文本的 SHA-256；用于发现"新旧头混用/SDK 被改" |
| `opencv_overlay` | 该构建是否提供 OpenCV 叠加包 |
| `files[]` | 关键文件的相对路径 + sha256 |

升级 SDK：整体替换 SDK 目录 → 删旧 build 或重新 configure → 重编 → 重跑 `--test-plugin`；
不要手工修改 SDK 里的契约头（`contract_sha256` 会变，且行为不再可复现）。

---

## 附录 A：最小验收清单

- [ ] `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=<SDK>` exit 0
- [ ] `cmake --build build` exit 0，产物 `build/bin/plugins/<target>.dll`
- [ ] `Plugin-shot.exe --test-plugin <dll> --json-out report.json` exit 0，`summary.fail == 0`
- [ ] DLL 只导出 `shot_module_entry`；metadata 贴尾完整（自检会查）
- [ ] 宿主 `--capture` 冒烟 exit 0，能看到插件效果，`host.log` 无 error
- [ ] 设置页能看到插件行与配置项；改值保存后重启仍生效
- [ ] README 写清功能/配置/验证/限制；交付说明含安装、卸载、风险

## 附录 B：SDK 内相关文档

| 文档 | 内容 |
| --- | --- |
| `docs/METADATA.md` | metadata 字段、依赖语法、加载顺序、构建期校验 |
| `docs/ABI_POLICY.md` | 只追加规则、版本规则、interface id / 配置 key 登记 |
| `docs/LISTS.md` | 列表协议、各 list id schema、owner/order/清理 |
| `docs/EVENT_BUS.md` | 主题约定、线程模型、64 KiB 限制 |
| `docs/CAPTURE.md` | 采集/处理/保存流水线、`capture.*` 负载、失败语义 |
| `docs/I18N.md` | 语言表、`i18n_source_list` / `i18n_list`、回退链 |
| `docs/SETTINGS.md` | `config_list` schema、控件映射、应用/取消语义 |
| `docs/PLATFORMS.md` | Windows/Linux 平台支持矩阵与已知限制 |
| SDK 根 `README.md` | SDK 目录、构建约定、ABI 约定、debug 变体 |
