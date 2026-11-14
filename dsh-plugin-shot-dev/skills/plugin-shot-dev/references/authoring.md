# Authoring 速查（完整版见 SDK `docs/AUTHORING.md`）

> 这是工作流里的"边写边查"卡片；教程与完整示例在 SDK `docs/AUTHORING.md`。

## 1. 最小骨架

```cpp
#include "plugin_api.h"
#include "shot_plugin_helpers.h"

namespace {
const ShotImageProcessorV1 kProcessorOps =
    SHOT_OPS(ShotImageProcessorV1, .process = &my_process);

const ShotIfaceEntry kIfaces[] = {
    {SHOT_IFACE_IMAGE_PROCESSOR, 1, &kProcessorOps},
    {nullptr, 0, nullptr},
};

struct Instance {
    const ShotHostServices* host = nullptr;
    std::unique_ptr<shot::I18n> i18n;
};

void* create_instance(std::uint32_t, const ShotHostServices* host) {
    auto* self = new (std::nothrow) Instance;
    if (self == nullptr) {
        return nullptr;
    }
    self->host = host;
    // 订阅与语言表在这里发布（唯一拿到宿主服务表的时机之一）
    return self;
}

void destroy_instance(void* handle) {
    auto* self = static_cast<Instance*>(handle);
    // 退订、join 线程、释放资源
    delete self;
}

void* query_instance(void* handle, const char* id, std::uint32_t version) {
    return handle == nullptr ? nullptr : shot_query_interface(kIfaces, id, version);
}

std::int32_t get_info(std::uint32_t index, ShotPluginInfo* out) {
    if (out == nullptr || index != 0) {
        return 1;
    }
    out->abi_minor = SHOT_ABI_MINOR;
    out->id = "acme.thing";
    out->name = "ACME Thing";
    out->version = "1.0.0";
    return 0;
}

const ShotModule kModule = {
    .struct_size = sizeof(ShotModule),
    .abi_major = SHOT_ABI_MAJOR,
    .abi_minor = SHOT_ABI_MINOR,
    .plugin_count = 1,
    .init = nullptr,
    .shutdown = nullptr,
    .get_info = &get_info,
    .create = &create_instance,
    .destroy = &destroy_instance,
    .query_interface = &query_instance,
};
}  // namespace

extern "C" {
SCREENSHOT_API const ShotModule* shot_module_entry(void) { return &kModule; }
}
```

要点：唯一导出符号 `shot_module_entry`；`SHOT_OPS` 负责填 `struct_size`/`version`；
能力目录以 `{nullptr,0,nullptr}` 结尾；异常绝不跨 C 边界。

## 2. metadata.json

```json
{
  "schema": 1,
  "abi": { "major": 2, "minor": 0 },
  "plugins": [
    {
      "id": "acme.thing",
      "name": "ACME Thing",
      "version": "1.0.0",
      "description": "一句话说明",
      "interfaces": ["shot.image_processor", "shot.configurable"],
      "requires": ["core>=0.1.0"],
      "provides": []
    }
  ]
}
```

- `plugins[]` 顺序 = `create(index)` 索引顺序；`id` 全局唯一（格式 `作者.名字`）。
- `interfaces` 必须与 `query_interface` 实际回答一致；abi major 相等、minor ≤ SDK 头。
- 构建期 `ValidatePluginMeta.cmake` 会校验 id 唯一 / interface 存在 / abi / requires 语法。

## 3. 配置项（自动进设置页）

```cpp
const ShotConfigDesc kConfigs[] = {
    {sizeof(ShotConfigDesc), SHOT_CONFIG_KIND_INT, "strength", "强度", "50",
     nullptr, 0, 100, 1, "0 = 原图，100 = 最强"},
    {sizeof(ShotConfigDesc), SHOT_CONFIG_KIND_ENUM, "format", "格式", "format.png",
     "format.png|format.jpg", 0, 0, 0, nullptr},
};
std::uint32_t get_config_count(void*) { return std::size(kConfigs); }
std::int32_t get_config_desc(void*, std::uint32_t i, ShotConfigDesc* out) {
    if (out == nullptr || i >= std::size(kConfigs)) return 1;
    *out = kConfigs[i];   // struct_size 由调用方填好；这里整体覆盖时保持它
    return 0;
}
std::int32_t get_config(void*, const char* key, char* buf, std::uint32_t size);
std::int32_t set_config(void*, const char* key, const char* value);  // 未知 key 返回负值
```

- 值一律字符串；`BOOL` 用 `"1"`/`"0"`；`INT`/`FLOAT` 用十进制；`ENUM` 候选值放 `options`，
  用 `|` 分隔；`PATH` 的 `options` 写 `directory`/`file`；`STRING` 写 `hotkey` 会变成热键录入控件。
- **ENUM 候选值就是 i18n key**（插件自己命名空间，如 `format.png`），查不到时原样显示。
- 持久化键是 `<插件 id>/<key>`，落盘与回灌由宿主做；插件只 get/set。
- 不用改宿主：宿主枚举配置项，设置页自动出现一节。

## 4. i18n（多语言）

```cpp
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN","plugin.name":"我的插件","config.strength.label":"强度"})json",
    R"json({"@meta":"en-US","plugin.name":"My plugin","config.strength.label":"Strength"})json",
};
// create() 里：
m_i18n = std::make_unique<shot::I18n>(host, "acme.thing");
m_i18n->loadTables(kLangTables, std::size(kLangTables));
// 取文案：
const char* text = m_i18n->tr("config.strength.label", "强度");
```

key 规范：`plugin.name` / `plugin.description` / `config.<key>.label` /
`config.<key>.tooltip` / ENUM 候选值 / 托盘命令 id。语言表**每语言一张单行 JSON**，
`@meta` 必填；必须在 `create`、宿主读 `config_list` 之前发布。缺失语言按回退链
（当前语言 → 主语言族 → en-US → 第一张表）。

## 5. 列表贡献（托盘 / 热键）

见 [能力决策表 §4](capability-decision.md#4-列表贡献配方托盘--热键)。owner 必须等于自己
插件 id；宿主按 owner 自动清理；`label` 可以是 key 或字面量。

## 6. 事件与线程

| 上下文 | 线程 | 规则 |
| --- | --- | --- |
| `process` / `save` / `describe` 能力回调 | 采集 worker（串行、一次一张） | 不做 UI、不阻塞；按 stride 寻址 |
| `set_config` | main 线程（启动回灌/设置页应用） | 与 worker 共享状态要加锁 |
| 事件订阅回调 | main 线程 | 只入队/拷贝；耗时任务转自己的线程 |
| 插件自建线程 | 自己的 | `destroy` 里停 + `join`；UI 类窗口类 `UnregisterClassW` |

- 订阅在 `create`/`init`，退订在 `destroy`/`deinit`；卸载后宿主不再回调。
- 负载上限 64 KiB；大对象（像素）不进总线，`capture.done` 只给路径与尺寸。
- 生命周期：`module->init` → 逐实例 `create` → … → `destroy` → `module->shutdown` → 卸载 DLL。
- 不要同步等待 `lists.update`/`lists.commit` 结果（会卡住卸载路径）。

## 7. 卸载清单

- [ ] 所有订阅已退订
- [ ] 自建线程已 `join`
- [ ] 定时器/回调已取消
- [ ] UI：窗口销毁 + `UnregisterClassW` + GLFW/消息循环停止
- [ ] 静态标记重置（多插件 DLL 重新启用时才能重新贡献）
- [ ] 不 delete 宿主内存；插件分配的对象由 `destroy` 释放

## 8. 常见错误

| 错误 | 原因 |
| --- | --- |
| 设置页没有我的配置项 | 没声明 `shot.configurable`；`get_config_count` 返回 0；插件未加载 |
| 界面显示 key 而不是文案 | 语言表没发布/在 create 之前用；key 拼错；`@meta` 缺失 |
| 托盘没有我的菜单 | `lists.add` owner 不对；发布晚于宿主建列表（下一次重建才出现）；label/event 为空 |
| 水印位置不对/竖条 | 没有按 stride 逐行写，把填充写进了图 |
| 宿主退出卡住 | `destroy` 里等宿主/线程没 join/同步等 `lists.commit` |
| 卸载后崩溃 | 窗口类未注销、回调仍在、静态对象调宿主服务 |

完整教程：SDK `docs/AUTHORING.md`；规范：`docs/METADATA.md`、`docs/LISTS.md`、
`docs/I18N.md`、`docs/SETTINGS.md`、`docs/ABI_POLICY.md`、`docs/EVENT_BUS.md`、
`docs/CAPTURE.md`。
