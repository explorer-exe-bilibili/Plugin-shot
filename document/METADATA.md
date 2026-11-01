# 插件元数据（metadata.json）

每个插件工程根目录有一个 `metadata.json`，构建期由 `cmake/AppendPluginMeta.cmake`
追加到 DLL/SO 尾部（纯文本标记，见 `plugin_api.h` 的 `PLUGIN_META_START_MARKER`）。
它跟着文件走：复制、改名都不会出现「清单与 DLL 分家」。

## 1. 它是什么级别的事实

**metadata 只是声明，不是权威。** 宿主里唯一的权威是插件自己：

| 用途 | 用 metadata | 用插件真实回答 |
| --- | --- | --- |
| 加载前预筛（这个包值不值得 `LoadLibrary`） | ✅ | —— |
| 插件列表 / 搜索 / 依赖提示界面 | ✅ | —— |
| abi 是否兼容的**最终判定** | ❌ | `ShotModule::abi_major` |
| 插件 id / 名称 / 版本的**最终值** | ❌ | `ShotModule::get_info()` |
| 有哪些配置项 | ❌ | `shot.configurable` |
| 到底会哪些能力 | ❌ | `query_interface()` |

理由很实在：文件尾那段 JSON 谁都能用记事本改，不能当安全边界。
**加载后以真实回答为准；metadata 与它不一致时只警告，不拒绝加载。**

## 2. 字段

```json
{
  "schema": 1,
  "abi": { "major": 1, "minor": 1 },
  "plugins": [
    {
      "id": "example.invert",
      "name": "Invert (keep alpha)",
      "version": "1.0.0",
      "description": "把 B/G/R 逐字节取反，alpha 可选保留",
      "interfaces": ["shot.image_processor", "shot.configurable"],
      "requires": ["core>=0.1.0"],
      "provides": []
    }
  ]
}
```

| 字段 | 必填 | 含义 |
| --- | --- | --- |
| `schema` | ✅ | **元数据格式**的版本（与 ABI 无关）。字段增删时递增，宿主遇到不认识的 schema 只提示、不解析 |
| `abi.major` / `abi.minor` | ✅ | 本包实现的契约版本，与 `plugin_api.h` 的 `SHOT_ABI_*` 一致；用于**加载前**把不兼容的包灰掉 |

| `plugins[].id` | ✅ | 与 `get_info().id` 一致；**全局唯一**，格式 `作者.名字`。宿主的配置键就用它（`<id>/<key>`） |
| `plugins[].name` | ✅ | 界面显示名 |
| `plugins[].version` | ✅ | 插件自己的语义化版本 |
| `plugins[].description` | 可选 | 一行说明，显示在插件列表里 |
| `plugins[].interfaces` | ✅（可为空数组） | 本插件打算实现的能力 id 列表，用于预筛；必须与 `query_interface` 的实际回答一致 |
| `plugins[].requires` | ✅（可为空数组） | 依赖什么：`name` 或 `name>=x.y.z`，见下一节 |
| `plugins[].provides` | ✅（可为空数组） | 向进程提供什么名字（**只有名字，不带版本**），见下一节 |

**`plugins[]` 的顺序必须与 `create(index)` 的索引一一对应** —— 这是唯一的硬要求，
来自契约本身，不是元数据的附加约定。

## 3. 刻意不写的东西

| 不写 | 原因 |
| --- | --- |
| 配置项 schema（`config`） | 真实 schema 在 `shot.configurable` 里，写两遍必然漂移；要浏览配置项就加载插件 |
| 插件在哪个 DLL / 导出符号名 | 一台机器上就是一个文件一个包，元数据本身就贴在这个文件上 |
| 依赖的 `^1.2` / `~1.2.3` 这类区间简写 | 要下上界就写两行 `>=` / `<`，不必再定义一套运算符语义 |
| 提供者的版本 | 版本只由运行时 `shot.lib_provider` 报告，避免和 vcpkg 升级不同步 |
| `package`（包名） | 与目标 DLL 名重复；配置键用全局唯一的 `plugins[].id` 更稳（改名、换目录都不受影响） |
| 哈希 / 签名 | 想做完整性校验时再说，那时属于另一个层（安装器/签名），不属于契约 |

## 4. 依赖与加载顺序

依赖写在**插件级**（不是包级）：因为"这个插件用不用得上 opencv"本来就是每个插件自己的事，
一个包里两个插件需求可以完全不同。

```json
{ "id": "example.sharpen", "requires": ["core>=0.1.0", "opencv>=4.12"], "provides": [] }
{ "id": "opencv.provider",                "requires": ["core>=0.1.0"], "provides": ["opencv"] }
```

### 4.1 语法

`requires` 的每一项是 `name` 或 `name<op>version`：

| 写法 | 含义 |
| --- | --- |
| `"opencv"` | 有提供者就行，版本不限 |
| `"opencv>=4.12"` / `">4.12"` / `"<=5.0"` / `"<5.0"` / `"==4.12.0"` | 版本比较，按数字段，缺的段按 0 |
| 同一名字写多条 | 按 **AND** 组合成区间：`["opencv>=4.0", "opencv<5.0"]` |

**`core` 是宿主自己**，它的版本来自 CMake 生成的 `version.h`（`VERSION_MAJOR.MINOR.PATCH`）。
插件写 `"core>=0.2.0"` 就是"宿主低于 0.2.0 我不用"，这样宿主太老时插件不会被加载。

### 4.2 名字从哪来（谁能满足一条依赖）

| 提供者 | 名字 | 版本来源 |
| --- | --- | --- |
| 宿主 | `core` | `version.h` |
| 另一个插件 | 它的 `id` | 该插件注册后 `get_info().version` |
| 另一个插件的库 | 它 `provides` 里的名字 | 该插件注册后 `shot.lib_provider` 报告的库版本 |

注意 `provides` **不写版本**：库版本由 `shot.lib_provider` 在运行时报告（`get_library_version`）。
如果也写进 metadata，vcpkg 一升级 OpenCV 就得同步改两份，而运行时那份永远不会撒谎。
同一包里"插件 A 依赖插件 B"也走这里 —— 写 B 的 `id` 即可，宿主会保证 B 先创建。

### 4.3 宿主怎么用这些声明

1. 扫目录，**先读每个 DLL 尾部的 metadata**（不加载），得到"插件 → 依赖/提供"的图；
2. 建 `提供者名字 → 插件` 索引（插件 id 与它 provides 的名字都算提供者）；
3. 拓扑排序，**提供者先加载**；
4. 逐个插件检查依赖：`core` 比 `version.h`，其它比提供者**运行期**报告的版本；
5. 不满足就**只跳过这个插件**（同包里其它插件和整个宿主继续跑），日志写明原因。

兜底策略都是"往回退一步，不炸整个宿主"：缺提供者 → 跳过；提供者被跳过 → 连带跳过；
成环 → 打印警告后按发现顺序继续；某个 DLL **没有元数据** → 失败开放，最后加载、不做依赖检查
（一个包不能因为少了一段 JSON 就让宿主打不开）。

## 5. 构建期校验（cmake/ValidatePluginMeta.cmake）

`shot_add_plugin()` 在**链接之后、贴尾之前**跑一次校验，写错就让构建失败 ——
这些都是"不改就要到宿主运行期才发现"的问题：

| 检查 | 抓的是 |
| --- | --- |
| `plugins[]` 非空 | 忘了写插件清单 |
| `id` 唯一 | 复制粘贴后忘了改 id |
| `interfaces` 里的 id 都在契约头里有定义 | 拼错一个字母，宿主预筛就永远认不出这个插件 |
| `abi.major` / `abi.minor` 与 `plugin_api.h` 一致 | 契约升级了但忘了同步 metadata |
| `requires` 每条都是 `name[op version]` | 少个 `>`、版本写成 `x.y.z-beta` 之类，依赖会静默永不满足 |
| `provides` 只是名字、且不能是 `core` | 把版本写进 provides（会和运行期打架），或占用宿主的保留名字 |

**刻意不做 JSON 语法校验**：CMake 没有 JSON 解析器，硬写正则解析器既脆又长；
语法错误留给宿主读取时报告，这里只抓语义漂移。

## 6. 与 ABI 版本的关系

- `abi.major` 不同 = 这个包根本不可能工作，宿主可以据此**不加载**它。
- `abi.minor` 大于宿主 = 可能用到宿主不认识的新字段，宿主照常加载，但只请求自己认识的接口版本。
- `schema` 变了 **不代表** ABI 变了：前者只影响"宿主怎么读这段 JSON"，后者影响"宿主怎么调用这些函数"。
  两者独立递增，不要混用。

## 7. 改这个格式时的检查清单

- [ ] 新增字段要么可选、要么同时递增 `schema`（老宿主遇到新字段必须能忽略）
- [ ] 没有把"权威信息"搬进 metadata（配置项、能力实现情况都不行）
- [ ] `plugins[]` 的顺序仍然等于 `create(index)` 的顺序
- [ ] `document/ABI_POLICY.md` 的登记表不需要跟着改（metadata 里没有能力定义）