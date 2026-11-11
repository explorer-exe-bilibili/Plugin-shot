# STYLE.md —— plugin-shot 的代码风格与工程约定

> 目标读者：接手这个仓库的**人**或 **AI**。这份文件不是"建议"，而是对**既有代码里已经稳定成立的做法**的整理。
>
> **权威顺序**：契约头与既有代码 > 本文 > 个人偏好。本文与代码冲突时以代码为准，并且欢迎顺手把本文改对。
>
> **现实前提**：本仓库**没有** `.clang-format`、`.editorconfig`、`.gitattributes`、CI、格式检查任务（已确认为零）。所以**风格只能靠约定和模仿**，这份文件就是那条约定。新写的代码请与周围文件保持一致；**宁可跟邻居一样丑，也不要局部好看**。

---

## 0. 30 秒速查

| 项 | 约定 | 例 |
| --- | --- | --- |
| 语言标准 | C++23（`CMakeLists.txt:51`） | `set(CPP_STANDARD 23)` |
| 编码 | UTF-8 **无 BOM**；新文件用 LF 且末尾有换行（13 个既有文件是 CRLF，编辑时保持原样，见 §2.3） | 源文件与文档 |
| 缩进 | 4 空格，禁止 Tab | |
| 花括号 | K&R：开括号跟在行尾 | `void f() {` / `if (x) {` |
| 行宽 | 目标 ≤100 列，上限 120 | 超了换行并对齐参数 |
| 注释语言 | **中文散文**，解释"为什么" | `// 插件分配的对象只能由插件自己释放，宿主绝不 delete。` |
| 公共 API 注释 | 头文件里的类 / 结构 / 函数必写 Doxygen `/** ... */`（§5.1） | `@brief` + `@param` + `@return` |
| 头文件粒度 | **一个类一个头文件**；唯一例外：一个基类 + 几个非常简单的子类（§6.8） | `settings_window.h` / `settings_panel.h` |
| .cpp 粒度 | 一个文件一个关注点；模型 / 渲染 / 提交这类可命名分区要拆开（§6.8） | `settings_model.cpp` / `settings_form.cpp` |
| 未完成项 | 写成 `// TODO: 中文说明` 放在代码处，**不写进文档**（§5.2） | `// TODO: 事件循环还是空转轮询` |
| 头文件 | 一律 `#pragma once`，不用 include guard | `include/plugin_api.h:1` |
| 类型 | PascalCase | `PluginEntry` `GrayscaleState` `ShotModule` |
| C 边界函数 / ABI 回调 | snake_case | `shot_module_entry` `create_instance` `grayscale_process` |
| C++ 类方法 | lowerCamelCase | `getName()` `queryInterface()` `postToPlugin()` |
| 类成员 | `m_` + camelCase | `m_ops` `m_instance` `m_impl` |
| 常量 | `k` + PascalCase | `kModule` `kEntries` `kConfig` `kBlueWeight` |
| 跨 TU 全局 | `g_` + camelCase | `g_hostServices` `g_eventQueue` |
| 宏 | 全大写 + `SHOT_` / `SCREENSHOT_` 前缀 | `SHOT_IFACE_CONFIGURABLE` |
| 返回值 | C 边界：`0` = 成功；内部 C++：`bool` | `return 0;` / `return false;` |
| 异常 | **绝不跨越 DLL 边界**；边界内用返回码 | |
| 职责归属 | 一个关注点一个类；新功能进自己的 `.h`/`.cpp`，不寄居在已有类里 | `HostServices` `Config` `PluginManager`（§6.6） |
| 类接口 | 类的**所有**具名函数都在 `.h` 声明；`.cpp` 不写头文件里没有的函数 | `PluginManager` 的 `private static`（§6.7） |
| 文档同步 | 改代码必须**同提交**改对应文档；中英 README 必须同步 | §9.1 的同步矩阵 |

---

## 1. 先理解结构：两个世界，一条边界

这个仓库不是"一个程序"，而是**宿主 + 插件契约**。写代码前先认清楚自己在改哪个世界：

| 世界 | 位置 | 语言 | 规则 |
| --- | --- | --- | --- |
| **契约（跨边界）** | `include/plugin_api.h`（汇总头，只 include）+ `include/plugin_api/*.h`（按主题的子头）、`plugin_api_export.h` | 纯 C（不依赖 STL） | 只追加、定宽类型、`struct_size` + `SHOT_HAS_FIELD`、异常不过界 |
| **插件侧辅助头（不走 ABI）** | `include/shot_plugin_helpers.h` + `include/shot_plugin_helpers/` | C++17，可用 STL | 只被插件源码包含；不跨边界、不参与宿主编译 |
| **宿主** | `src/`、`include/`（除契约头）、`include/PluginInstances/` | C++23 | 正常 C++，但**不得 delete 插件对象** |
| **插件** | `plugins/<name>/` | C++23，但出口是纯 C | 一个 DLL = 一个包；`shot_module_entry` 是唯一导出符号 |

**最重要的心智模型**：跨边界的一切都是"对方可能是别人（甚至老版本）编译的"。因此：

- 读任何跨边界结构体的字段之前，先 `SHOT_HAS_FIELD`；
- 跨边界结构体第一个字段永远是 `uint32_t struct_size`，由**构造方**填自己的 `sizeof`；
- 插件不提供某个能力（函数指针为 `NULL`）是**正常情况**，不是错误；
- **数据形状不进 ABI**：给 UI/宿主提供一批新数据（菜单项、目标列表、热键……）时，做法是
  **新 list id + JSON schema + `lists.*` 事件**，不是新结构体、新宿主服务；ABI 只描述能力与
  行为（见 `document/LISTS.md`）。

改契约之前必须读 `document/ABI_POLICY.md`，改元数据格式必须读 `document/METADATA.md`。

---

## 2. 文件与目录

### 2.1 文件命名：按层选，别发明第四种

| 层 | 规则 | 现状 |
| --- | --- | --- |
| 契约头（纯 C，跨 DLL） | snake_case | `plugin_api.h` `plugin_api_export.h` |
| 契约相邻的共享头（C++） | snake_case | `shot_plugin_helpers.h`（汇总头）、`shot_plugin_helpers/shot_logger.h` `shot_plugin_helpers/shot_config.h`（每类一个）、`plugin_meta.h` `plugin_requirement.h` `shot_semver.h` |
| 宿主内部头 | lowerCamelCase | `dllHandler.h` `hostServices.h` `pluginManager.h` `configItem.h` `hostRuntime.h` `hostListCatalog.h` |
| 宿主包装类 | PascalCase，放 `include/PluginInstances/` | `ImageProcessor.h` `LibProvider.h` `UIProvider.h`（`base.h` 是唯一例外，保留） |
| 插件目录 | snake_case，**目录名 = CMake 目标名 = DLL 名 = 命名空间名** | `plugins/example_processor/` → `example_processor.dll` → `namespace example_processor` |
| 插件源文件 | snake_case，一个能力一个文件 | `src/plugin_entry.cpp` `src/grayscale.cpp` `src/invert.cpp` |
| CMake 模块 | 新文件用 snake_case | `cmake/copy_runtime_deps.cmake` `cmake/ShotPlugin.cmake`（既有 PascalCase 保持不动） |
| 本地验证脚本（不入库产物） | snake_case；`.ps1` 一律 UTF-8 带 BOM（PowerShell 5.1 才认中文），公共函数进 `common.ps1` 不复制粘贴 | `tools/verify/build.sh` `tools/verify/common.ps1` `tools/verify/smoke_capture.ps1` |

> 文件命名就按上表的层来选，不要发明第四种拼法。`include/PluginInstances/base.h` 是唯一的小写文件名例外，保留不动。
> 头文件粒度按 §6.8：**一个类一个头文件**；`shot_plugin_helpers.h` 这类汇总头自身不声明类，只 include
> `shot_plugin_helpers/` 子目录里每类一个的头文件（避免 `include/` 根目录堆太多文件）。

### 2.2 插件目录的标准骨架

``` text
plugins/my_plugin/
├── CMakeLists.txt        # 固定样板，见 §7
├── metadata.json         # 构建期贴到 DLL 尾部，见 document/METADATA.md
├── README.md             # 中文，见 §9
├── include/              # 只有当头文件被本插件的多个 .cpp 共享时才建
│   └── processors.hpp
└── src/
    ├── plugin_entry.cpp  # 必须：唯一导出符号 + kEntries 索引目录
    └── <capability>.cpp  # 一个能力一个文件：状态 + kConfig + 回调 + 函数表
```

### 2.3 编码与换行

- **UTF-8 无 BOM**（注释、文档里有大量中文）；**LF**；文件末尾留一个换行。
- CRLF 有 13 个固定成员：`.gitignore`、`.vscode/` 下 4 个 json、`CMakeLists.txt`、`CMakePresets.json`、`CMakeSettings.json`、`app.rc.in`、`cmake/kill_running.bat`、`document/README_SDK.md`、`include/PluginInstances/base.h`、`include/version.h.in`。其余文本文件是 LF。**编辑既有 CRLF 文件时保持 CRLF**；新文件一律 LF。
- 不要用会改写编码的编辑器/工具保存中文文档。**提交前确认文件里没有 U+FFFD 替换字符**（历史上 `README.md` 就被写坏过一次「依赖驱动」），`git diff` 里出现 `�` 立刻修。

---

## 3. 命名

### 3.1 分层命名法（这是本仓库最容易踩错的一条）

同一个概念在不同层用不同拼法，是**故意的**，不是随手写的：

| 层 | 风格 | 例子 |
| --- | --- | --- |
| 契约结构体字段（C 头） | snake_case，靠对齐留白 | `struct_size` `pixel_format` `get_library_name` `post_to_plugin` |
| 插件侧自由函数 / ABI 回调 | snake_case | `create_instance` `destroy_instance` `module_init` `get_info` `state_of` `for_each_row` `grayscale_process` |
| 宿主 C++ 类方法 | lowerCamelCase | `getName` `getType` `nativeHandle` `queryInterface` `postToPlugin` `getLibraryVersion` |
| 插件 C++ 类方法 | lowerCamelCase | `SettingWindow::start/stop/post` |

记忆法：**C 边界 snake，C++ 方法 camel**。类型名两边都 PascalCase。

### 3.2 其余命名

- **成员**：类成员 `m_` + camelCase（`m_ops` `m_instance` `m_dllHandle` `m_initialized` `m_impl`），**不加**尾下划线；纯数据 `struct` 的字段**不加** `m_`（`PluginEntry::id`、`ShotImage::width`）。
- **常量**：`k` + PascalCase —— `kModule` `kEntries` `kEntryCount` `kIfaces` `kConfig` `kKeyMode` `kTailWindow` `kBlueWeight`。数值用 `constexpr`，字符串数组用 `const char kKeyXxx[]`。
- **全局**：`g_` + camelCase —— `g_hostServices` `g_eventQueue` `g_broadcastQueue`。只在 `.cpp` 内可见的可变对象用 `static` + camelCase（`static std::mutex logMutex;`）。
- **枚举**：宿主侧 `enum class` + PascalCase（`PluginType::ImageProcessor`）；**插件侧不定义枚举**，配置种类用契约宏 `SHOT_CONFIG_KIND_*`。
- **宏**：全大写，契约宏一律 `SHOT_` 前缀；导出宏是 `SCREENSHOT_API`（**不是** `SHOT_PLUGIN_EXPORT`）。
- **命名空间**：宿主插件域用 `Plugin` / `Host`（PascalCase），宿主基础设施用 `core`（全小写，如 `core::Config`）；插件 = 包名（snake_case，与目录同名）。不用 `a::b` 嵌套形式，匿名命名空间 `namespace { }` 收内部符号。

### 3.3 格式骨架

```cpp
namespace example_processor {      // 插件：包名
namespace {                        // 内部实现

const char kKeyMode[] = "mode";    // k 前缀常量

struct GrayscaleState {            // PascalCase 类型
    bool use_luma = true;          // 纯数据成员不加 m_
    int strength = 100;
};

std::int32_t grayscale_process(void* self, ShotImage* image) {  // 自由函数 snake_case
    ...
}

}  // namespace
}  // namespace example_processor
```

---

## 4. 格式与排版

- **4 空格**缩进，禁止 Tab（全仓源码目前无 Tab，保持）。
- **K&R 花括号**：`namespace X {` / `struct Y {` / `void f() {` / `if (x) {`，闭括号独占一行。
- **括号前有空格**：`if (` `for (` `while (` `switch (`。这是主流写法（实测 `if (` 205 处 vs `if(` 8 处）；那 8 处是历史遗留，**不要**扩散。
- **行宽**：目标 100 列。全仓源码最长 131 列（`base.h` 的构造函数），绝大多数 ≤120。超长时优先**换行对齐参数**，而不是硬憋。
- **闭括号注释**（2 个空格 + 注释）：
  - `}  // namespace example_processor`
  - `}  // namespace`（匿名）
  - `}  // extern "C"`
- **行尾不留空格**（现存 2 处，见 §11）；**不要**在文件中留下空行块（连续 3 个空行）。
- **包含顺序**（每组之间空一行）：
  1. 自身头文件（`src/HostServices.cpp` 的第一行是 `"hostServices.h"`）
  2. 标准库 `<...>`，**字母序**
  3. 项目头文件 `"..."`，**字母序**
  - 契约头永远写 `"plugin_api.h"`（靠 target include 目录找到），不写相对路径。
- **指定初始化器**（C++20，本仓高频用法）—— 一字段一行、`=` 对齐到同一列、按声明顺序、**末尾带逗号**：

```cpp
const ShotModule kModule = {
    .struct_size     = sizeof(ShotModule),
    .abi_major       = SHOT_ABI_MAJOR,
    .abi_minor       = SHOT_ABI_MINOR,
    .plugin_count    = kEntryCount,
    ...
};
```

> 能力函数表**不要手写** `struct_size`，用 `SHOT_OPS(...)` 宏（`include/shot_plugin_helpers.h`）。

- **尾部注释**：代码与 `//` 之间**两个空格**，且注释要短：`pixels[i + 0] = 255;  // B`。

---

## 5. 注释：本仓库最鲜明的风格

**这是最容易一眼认出"不是本仓库代码"的地方。**

1. **用中文散文写注释**，标识符/API 名保持英文。实测契约头（`include/plugin_api.h` 汇总头 + `plugin_api/*.h` 子头，共 1147 行）里 486 行含中文（42%），`document/ABI_POLICY.md` 142 行里 95 行含中文。
2. **解释"为什么"，不解释"是什么"**。反例：`// 遍历行`。正例（本仓真实写法）：
   - `// 插件分配的对象只能由插件自己释放，宿主绝不 delete。`
   - `// 顺序有讲究：先让插件实例走完自己的 destroy，再 shutdown 模块，最后才放手 DLL。`
   - `// stride 以字节计，可以为负（自底向上存储），也可以大于 width*channels（行尾填充），`
     `// 所以只能按行寻址；把 data 当连续块是这类插件最常见的越界来源。`
3. **记录踩过的坑与实测结论**——这是本仓注释的灵魂：
   - `// 放在各自定义前面会在第一次使用处报 "缺少类型说明符"（实测 MSVC C4430 + C2143）。`
   - `// 不能叫 requires：那是 C++20 关键字`
   - `// 插件没提供 init 时按"没有要做的初始化"处理，不算失败。`
4. **文件头放一段块注释**，说明这个文件解决什么问题、契约要点、算法公式。`plugins/example_processor/src/grayscale.cpp:1-7` 是范本：

```cpp
// 灰度：mode 选算法，strength 控制混合强度。
//
//   mode = luma    : Y = 0.299R + 0.587G + 0.114B（BT.601，×256 定点）
//   mode = average : (B + G + R) / 3
//   strength 0..100: 0 = 原图，100 = 完全灰度
```

5. **分隔 banner 有两种，按文件类型选**：
   - **头文件（契约类）**：两条 `=` 横线（76 列宽）夹住若干行说明：

     ```cpp
     // ============================================================================
     //  版本
     //
     //  abi_major：布局或语义不兼容时递增，宿主只接受 major 相同的插件。
     // ============================================================================
     ```

   - **`.cpp` 内部**：单行 `----` 短 banner，或阶段标记：
     `// ---- 阶段 3：拓扑排序（提供者先于依赖者）；无元数据的包排在最后 ----`

6. **公共 API 用 Doxygen 注释**：头文件里的类、结构体、枚举、公开/受保护函数、导出宏必须有
   `/** ... */`（§5.1）。散文注释继续负责"为什么"，Doxygen 负责"调用者必须知道的契约"。

7. **未完成的事用 `// TODO:` 写在代码里**：README 的路线图、"下一步"、📋/🚧 计划一律不进文档（§5.2）。

8. **不写 license/SPDX 头**（仓库尚未指定 license）。

### 5.1 Doxygen 注释（公共 API 必须写）

**这是硬要求。** 以前本仓库不写 Doxygen，现在补上——但**不是**把散文改成八股：
Doxygen 只写"调用者必须知道的东西"（约束、单位、返回码、线程安全、ABI 警告），
"为什么这么实现"仍用普通中文注释写。

- **必写**：头文件里的 `class` / `struct` / `enum class` / 公开与 protected 函数 / 公共常量 /
  导出宏 / 契约头 `include/plugin_api/*.h` 里的每个跨边界结构体与 `extern "C"` 入口。
- **不写**：`.cpp` 里的私有实现、局部 lambda——那里继续用普通注释。
- 格式：`/** ... */` 块；首行 `@brief` 一句话（中文、动宾），需要时补 1~3 行正文，
  再按需 `@param` / `@return` / `@note` / `@warning`。
- 标签只用这几个：`@brief`、`@param[in]`、`@param[out]`、`@param[in,out]`、`@return`、`@note`、`@warning`。
  不用 `\brief`、`@file`、`@defgroup` 等其它写法。
- 结构体字段用行尾 `/**< ... */`；纯 C 头也照此写。
- 参数说明写**约束/单位/能否为空**，不要复述类型：
  `@param[in] stride 字节为单位，可为负（自底向上存储）`。
- **保留原有 banner 的"为什么"**：能并进 Doxygen 正文就并进去，不要同一件事写两遍。

```cpp
/**
 * @brief 从 DLL 尾部读出 metadata.json，解析成插件清单。
 *
 * 只读文件尾部 64 KiB：元数据贴着文件尾，没必要读整个 DLL。
 *
 * @param[in]  dllPath 插件文件路径。
 * @param[out] error   失败时写入中文原因；成功时清空。
 * @return true = 解析成功；false = 没有元数据或格式不合法。
 * @note 只负责读取，语义校验在 cmake/ValidatePluginMeta.cmake。
 */
bool readFromFile(const std::string& dllPath, std::string& error);
```

### 5.2 TODO：未完成的事写在代码里

**不要**把未完成项写进 README 的路线图、`document/*.md` 或"下一步"段落。
文档只描述**现在是什么**；没做完的实现留在它将来要改的那段代码旁边。

```cpp
// TODO: 事件循环目前是空转轮询，也没有退出条件；等截图能力落地后改成阻塞等待 + 退出。
```

- 格式：`// TODO: 中文说明`，说明里写**缺什么、为什么先没做、怎样算做完**（至少含其一）。
- 位置：离缺口最近的代码处（函数体内、成员声明上方、入口点），不要集中成一个清单。
- 一个 TODO 只对应一件事；做完就**删掉那行 TODO**（代码就是进度表）。
- 不用 `FIXME` / `XXX`，统一用 `TODO`。

---

## 6. 代码写法

### 6.1 函数与参数

- **const 正确性**：只在确实需要改的对象上加非 const。边界上 `const ShotHostServices*`、`const ShotPluginInfo*`，图像是 `ShotImage*`（原地处理，故意非 const）。
- **引用只用于内部、且确定非空**（`const ShotImage&`）；边界一律用指针（`ShotImage*`、`ShotConfigDesc* out`）。
- **出参**：`T* out`，进入先查 `out != nullptr` 与 `out->struct_size` 够不够。
- **未使用的参数**：命名注释掉 `void* /*ctx*/`，或函数体里 `(void)flags;`。**不用** `[[maybe_unused]]`（本仓未使用）。
- **默认参数、虚函数、继承**：宿主包装类用 `virtual`（`PluginBase` 体系）；**插件侧不用**——插件只有 C 回调，没有类层次。
- **函数要短**：普遍 5~30 行；长函数靠 `std::function` lambda 分阶段（`PluginManager::loadPlugins` 就是 5 个阶段 + 局部 lambda）。

### 6.2 返回值与错误

- **C 边界只有返回码**：`0` = 成功；`1` = 参数/索引/格式不对；`2` = 对方结构太短；配置键不认识返回**负数**。**不要发明新的码值**，现有语义见 `plugin_api.h` 各结构体注释。
- **内部 C++ 用 `bool`**，边界处再映射：`return m_ops->process(m_instance, &image) == 0;`
- **异常绝不跨越 DLL 边界**。边界内的 `try/catch` 只出现在必须兜底的地方（`PluginManager.cpp:215-223` 加载 DLL、`plugins/settings_ui/src/settings_window.cpp` 的 `start()` 建线程），且 `catch (...)` 后就地转成返回码/`false`。
- **失败不炸整个宿主**：加载失败 = 跳过一个插件并打日志（`document/METADATA.md:105-117` 的六步策略）。

### 6.3 内存与资源

- **谁分配谁释放**：插件分配的对象只能由插件的 `destroy` 释放，**宿主绝不 `delete`**（`base.h:34-40`、`PluginBase` 析构里走 `m_module->destroy`）。
- 插件里分配：`new (std::nothrow)` + 判空，失败清理已分配部分（`plugin_entry.cpp:41-47`）。
- 宿主持有插件：`std::shared_ptr<DllHandler>` 共享 DLL 生命周期 + 裸 `void*` 不透明实例；`std::unique_ptr` 用于 PIMPL（`plugins/settings_ui/include/settings_window.h:72-73`）。
- `std::string` **只在同一个 DLL 内部用**；跨边界一律 `const char*` + 长度。
- Win32 类型（`HWND` 等）**只允许出现在 PIMPL 内部**，宿主永远看不到。

### 6.4 日志

- **统一用 spdlog**（vcpkg 依赖 `spdlog`，CMake 链接 `spdlog::spdlog`）：**不要**自己写日志框架、队列或文件轮转。
  宿主用 `spdlog::info/warn/error("...{}...", args)`；启动时由 `Host::Logging::init()`
  （`include/hostLogging.h` + `src/hostLogging.cpp`）初始化 `host` / `plugin` 两个异步 logger
  （控制台 + `<dataPath>/logs/host.log`），运行期的控制台/文件开关用"过滤 sink"实现，退出时 `spdlog::shutdown()`。
- **插件只能通过宿主日志**：`host->log(host->host_context, SHOT_LOG_INFO, msg, len)`，且调用前三重检查：
  `host != nullptr && SHOT_HAS_FIELD(host, ShotHostServices, log) && host->log != nullptr`。
  插件**不要**链接 spdlog：`HostServices::hostLog` 会把插件侧 `SHOT_LOG_*` 映射成 spdlog 等级并转到 `plugin` logger。
- **插件侧用 `shot::Logger` / `shot::Config`**（`include/shot_plugin_helpers.h`）：前者包 `host->log`（含老宿主降级），
  后者包宿主配置服务（`SHOT_HOST_IFACE_CONFIG` + fallback）。**不要**在每个调用点手写 `SHOT_HAS_FIELD` 判空或两次长度查询。
- 日志格式由 spdlog pattern 统一（`[时间] [级别] [host|plugin] 消息`），**不要**在消息里手写级别/时间前缀。
- 级别语义见 `include/plugin_api/basics.h` 的 `SHOT_LOG_*`（DEBUG / INFO / WARN / ERROR）。

### 6.5 降级（degradation）是常态，不是可选项

插件必须能在**较老的宿主**上跑，宿主必须能容忍**较新的插件**：

```cpp
// 本算法只对 BGRA8 有定义。宿主可能比本插件老、结构里还没有 pixel_format，
// 那种情况按当年的约定当成 BGRA8 处理。
if (SHOT_HAS_FIELD(image, ShotImage, pixel_format) &&
    image->pixel_format != SHOT_PIXEL_FORMAT_BGRA8) {
    return 1;
}
```

读到不认识的配置 key、宿主没给某个服务、结构体没有某个字段——**安静降级**，不要报错、不要崩。

### 6.6 职责归属：一个关注点一个类，新功能别寄居在已有类里

**反例（本仓库真实踩过）**：宿主服务表、事件队列、日志转发曾经全塞在 `PluginManager` 里——
它一边管扫描/依赖/加载，一边管日志和事件分发。结果是谁都不敢动：改日志要碰加载器，
类越滚越大，职责边界消失。

**正例（现在的拆法）**：

- `include/hostServices.h` + `src/HostServices.cpp`：宿主服务表、事件队列、日志/配置回调的转发；
- `include/config.h` + `src/config.cpp`、`src/configItem.cpp`：`core::Config` / `core::ConfigItem`，只管配置读写；
- `include/hostRuntime.h` + `src/hostRuntime.cpp`：运行期装配与生命周期锁，只做"把谁和谁装到一起、每轮做什么"；
- `include/hostListCatalog.h` / `hostConfigurable.h` / `hostPluginRegistry.h` / `hostLogging.h`：列表目录、宿主设置、插件注册表、日志各一组文件；
- `include/PluginManager.h` + `src/PluginManager.cpp`：只剩本职——扫描、依赖解析、加载/卸载。

**新代码放哪里的三问**：

1. 这段逻辑是"谁的职责"？是现有类的本职 → 放进去；是**新职责** → 新建类 + 自己的 `.h`/`.cpp`。
2. 把它删掉，原来的类还剩什么？如果剩下的东西和它没关系，说明它本来就不该待在那里。
3. 名字能一句话说清吗？`HostServices`、`Config`、`PluginManager` 能；`PluginManager::postToHost` 不能。

**落地规则**：

- **一个类一组文件**：`X.h` 声明接口、`X.cpp` 写实现；新增功能默认走这条路。
- 不允许"上帝类"：一个类同时承担 2 个以上互不相关的职责时，拆。
- 公共小工具放独立头（如 `include/shot_plugin_helpers.h`），不要挂到某个业务类的 private 区。
- 插件侧受 ABI 约束，但"一个能力一个 `.cpp`"（§7）是同一个思路。

### 6.7 头文件是这个类的完整契约：类里有什么函数，`.h` 里就写什么

- 类的**所有**具名函数都必须在 `.h` 里声明：`public` 接口、`protected`、`private` 辅助函数，
  包括只在 `.cpp` 里调用的 `private static` 工具。
- **尽量不在 `.cpp` 里写头文件没有声明的具名函数**：
  - 只在类内部复用的逻辑 → 写成 `private static` 成员，并在 `.h` 声明；
  - 确实与任何类无关、且只在本 TU 用的具名函数 → 放匿名命名空间，并注释说明为什么不属于任何类；
  - 跨 TU 使用的具名函数 → 必须能在某个头文件里找到声明。
- 判据：**接手的人只读 `.h` 就该知道这个类会做什么、用到哪些函数**；读 `.cpp` 时不该冒出
  "这函数从哪来的"。
- 反例（现存，待收口）：`src/PluginManager.cpp` 的 `ShotVersion hostCoreVersion()` 没有在
  `include/pluginManager.h` 声明 → 应改为 `PluginManager` 的
  `private static ShotVersion hostCoreVersion();`。
- 合法例外（必须在注释里说明为什么）：
  - 插件 DLL 的 ABI 回调（`create_instance` / `grayscale_process` …）签名由契约固定，只能是自由函数；
  - 入口文件 `src/main.cpp` 的一次性装配/演示代码；
  - lambda、模板与 `inline` 小工具（放头文件更合适，如 `include/shot_plugin_helpers.h`）。
- 构造函数/析构函数在 `.h` 声明、`.cpp` 定义（`plugins/settings_ui/include/settings_window.h` 的
  PIMPL 是范本）。

### 6.8 头文件与 .cpp 的拆分粒度：一个类一个头文件

**一个类一个头文件**：`X.h` 声明 `X`，`X.cpp` 实现 `X`；两个不相关的类不要塞进同一个头文件。
唯一例外：**一个基类 + 几个非常简单的子类**（子类只覆盖一两个虚函数、没有独立状态机）可以共用一个头文件。
反例（真实踩过）：`settings_ui.h` 曾同时声明 `SettingsPanel` 与 `SettingsWindow`，两个类各有自己的
线程/状态模型，结果是"改窗口代码要碰设置模型的头文件"，头文件不再是这个类的完整契约（违反 §6.7）。

- 纯数据 `struct` 如果只服务于同一个类（如 `InstalledPlugin` 之于 `PluginManager`），可以留在该类头文件里：
  它是"类的数据契约"，不是独立的类。纯 C 契约头 `plugin_api.h` 是结构体集合、没有类，不适用本条。
- **汇总头只做 include**：像 `shot_plugin_helpers.h` 这种"一次包含全部小工具"的头文件，自身不再声明类，
  只 `#include` 每个类各自的头文件；插件仍然可以只包含汇总头。这类**成组的小头文件放同一个子目录**
  （如 `include/shot_plugin_helpers/`），不要在 `include/` 根目录平铺一堆文件。
- **内部头也单独成文件**：类的私有实现结构（PIMPL 的 `Impl`）与只被多个 `.cpp` 共用的内部类型，
  放进单独的内部头（如 `settings_panel_impl.h`，仍可放在 `include/` 下，但头文件开头要注明
  "内部头，不属于对外接口"）；公共头只留稳定的公开接口。

**.cpp 同样按职责拆**：一个 `.cpp` 只承担一个关注点（数据模型 / 渲染 / 提交与事件 / 窗口平台代码……）。
判据是"能不能一句话说清这个文件负责什么"，而不是行数本身；经验上单文件超过约 500 行、或出现
"模型、渲染、提交"这类可以命名的分区时就要拆。拆分时共享的声明与内部状态进内部头，不要靠
`extern` 或重复定义跨文件传私有状态。

反例（真实踩过）：`plugins/settings_ui/src/settings_panel.cpp` 曾同时做模型刷新、7 种控件的 ImGui
渲染、应用/取消提交与事件处理（1100+ 行），后来按模型 / 表单 / 插件表 / 提交拆成多个 `.cpp`。

---

## 7. 新增一个插件：照抄这份骨架

1. `plugins/my_plugin/` 目录，名字 snake_case。
2. 复制 `plugins/example_processor/CMakeLists.txt` 的样板（三行核心，其余是独立构建的 guard）：

```cmake
set(SHOT_PLUGIN_NAME my_plugin)
shot_add_plugin(my_plugin
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp src/my_capability.cpp
    INCLUDE_DIRS include
)
```

3. 写 `metadata.json`（字段见 `document/METADATA.md`），`plugins[].id` 用 `作者.名字`，`interfaces` 与 `requires` 必须与代码一致。
4. 复制 `plugins/example_processor/src/plugin_entry.cpp`，改三处：`kEntries[]` 内容、`module_init`、`kModule` 字段。
5. 一个能力一个 `.cpp`：状态 `struct` + `kConfig[]` + 回调 + `SHOT_OPS` 函数表 + `kIfaces[]` + `const PluginEntry kXxx`。
6. 写 `README.md`（§9）。

**两条硬约束**：

- `kEntries[]` 的顺序 == `metadata.json` 里 `plugins[]` 的顺序 == `create(index)` 的索引。这是唯一的硬顺序要求。
- `void* self` 是 `PluginInstance*`，**不是**插件状态；回调里先 `state_of(self)`。直接把 `self` 当成状态指针是这类插件最典型的崩溃原因。

> 从零写插件时，`plugins/example_processor` 是**最完整的模板**；`plugins/opencv_provider` 只演示"零能力的库提供者"，**不要**拿它当结构范本（它手写了 interface 查询、也没做 ABI 检查，见 §11）。

---

## 8. 构建系统约定

- **加插件不用改根 `CMakeLists.txt`**：根目录 `file(GLOB plugins/*/CMakeLists.txt CONFIGURE_DEPENDS)`（`CMakeLists.txt:478-486`）自动收编。
- **插件一律走 `shot_add_plugin()`**（`cmake/ShotPlugin.cmake:53`），不要手写 `add_library`：它统一了 `/MD`、`/utf-8`、隐藏符号、产物目录、元数据贴尾、产物剪枝。关键字参数是 `METADATA` / `SOURCES` / `INCLUDE_DIRS` / `LINK_LIBRARIES`。
- **CMake 命名**：
  - 开关：`ENABLE_*`（不是 `SHOT_*`）—— 目前只有 `ENABLE_KILL_RUNNING`
  - 函数：小写 `shot_*` snake_case，内部临时变量 `_arg_*`
  - 变量：`UPPER_SNAKE_CASE`（`PROJECT_VERSION_*` `APP_OUTPUT_DIR` `SHOT_PLUGINS_OUTPUT_DIR`）
  - 传给插件的目录变量：`SHOT_*`（`SHOT_SDK_INCLUDE_DIR` 等）
- **CMake 最低版本**：根文件 `cmake_minimum_required(VERSION 3.20)`，且必须是文件里**第一条** CMake 命令（`CMakeLists.txt:18`）；插件与 `cmake/*.cmake` 同为 3.20；`CMakePresets.json` 要求 3.21（preset schema `"version": 3` 本身的要求，别降低它）。
- **SDK 说明只有一个源**：`document/README_SDK.md`；`export_sdk` 用 `configure_file(... COPYONLY)` 把它复制成 `sdk/README.md`。改 SDK 说明只改 document 那份，**不要**在 CMake 脚本里再硬编码一份（以前就是这么漂移的）。
- **产物布局是硬约束**（`README.md`「产物布局」）：`bin/` 只有 exe 与运行时库（Windows 禁 pdb）；`bin/plugins/` 只有插件自己的动态库（禁基础依赖）；`build/<cfg>/pdb/` 放符号；`sdk/` 是导出产物、**不入库**。动态库扩展名随平台（`.dll` / `.dylib` / `.so`，由 `SHOT_DYN_EXT` 统一），非 Windows 运行库靠 RPATH 指向 vcpkg `lib/`。
- **版本链**：`0.1.0` 来自 `CMakeLists.txt:32-34` → `version.h`（`include/version.h.in` 生成）→ `.rc`。`build_number.txt` 在**每次 configure 时自增并被写回**（`CMakeLists.txt:290-297`），所以它会一直脏，**不要**手工改它，也**不要**把它当成自己的改动提交（除非单独提交）。
- **不要手改生成物**：`build/generated/version.h`、`sdk/` 全是产物。`sdk/include/plugin_api.h`（及 `sdk/include/plugin_api/`）是**旧快照**（比 `include/` 少能力），永远以仓库里的 `include/` 为准。

---

## 9. 文档约定

- **`README.md` 中文为母版**，`README.en.md` 是手工镜像（首行互链：`**中文** | [English](README.en.md)`）。改一边必须改另一边，**别让它漂移**（历史上就漂移过一次，见 §11 第 14 条）。
- **`document/` 与插件 README 是纯中文**，不配英文版。
- 结构惯例（`document/METADATA.md`、`ABI_POLICY.md` 是范本）：
  1. `# 标题`
  2. 开头 2~4 行说明"这份文档解决什么问题"，并声明**改动前必读**
  3. `## 1. 小节` 编号小节，可再分 `### 4.1`
  4. 大量**表格**：字段 / 含义 / 是否必填；规则 / 做法 / 要不要动版本
  5. 专门一节写**「刻意不做的事」**并给理由（本仓特色，`METADATA.md:59-68`）
  6. 结尾 **检查清单** `- [ ]`
- 语气：**祈使、直接、给理由**。用 **加粗** 标不可违反的点（**绝不** / **必须** / **禁止**），关键数字与标识符用反引号。
- 代码块**标语言**（`bash` `c` `cpp` `cmake` `json`）；只有目录树用裸围栏。
- 状态只用 ✅ 标注「已可用」；**计划与未实现不进文档**——未完成项写成代码里的 `// TODO:`（§5.2）。
- 不放图片/徽章/架构图（全仓零处），需要说明就用表格或目录树。

### 9.1 改代码必须同步改文档

**文档过期就是缺陷**：只要你的改动会让现有文档里任何一句话变成假话，就必须在**同一个提交**里改掉它。
`README.md` 与 `README.en.md` 必须同步（母版 + 镜像），不允许只改一边。

| 你改了什么 | 必须同步的文档 |
| --- | --- |
| 契约 `include/plugin_api.h` + `include/plugin_api/*.h`（能力 id / 结构体 / 布局 / `SHOT_ABI_*`） | `document/ABI_POLICY.md`（登记表 + 布局锚点表 + `static_assert`）、`README.md`/`README.en.md` 的「插件契约」段 |
| `metadata.json` 的字段或语义 | `document/METADATA.md`、`cmake/ValidatePluginMeta.cmake`、相关插件的 `metadata.json` 与 `README.md` |
| 插件新增/删除/行为变化 | 该插件 `README.md`、`README.md`/`README.en.md` 的「当前状态」表与目录树 |
| 配置项、设置页、插件启停（热卸载）行为 | `document/SETTINGS.md`、相关插件 `README.md`、`README.md`/`README.en.md` 的状态表 |
| 新增列表 / 列表事件协议（list id、JSON schema、`lists.*` 负载） | `document/LISTS.md` 的登记表与 schema、`document/EVENT_BUS.md` §4、贡献方/消费方插件的 `README.md`、`README.md`/`README.en.md` 的状态表 |
| 插件启停级联 / 入口兜底 / `plugins.changed` 通知语义 | `document/SETTINGS.md` §3.1–§3.2、相关插件 `README.md`、`README.md`/`README.en.md` 的「设置页与插件启停」段 |
| 采集流水线 / `capture.*` 事件 / `--capture` 行为 | `document/CAPTURE.md`、`document/EVENT_BUS.md` §4、`README.md`/`README.en.md` 的运行说明与状态表 |
| 采集 / 保存 / 托盘插件的新增或行为变化 | `document/ABI_POLICY.md` 登记表、`document/CAPTURE.md`、该插件 `README.md`、`README.md`/`README.en.md` 的状态表与目录树 |
| 新增插件（clipboard / notify / hotkey / region_selector 等） | 该插件 `README.md`、`README.md`/`README.en.md` 的「当前状态」表与目录树；按能力同步 `document/CAPTURE.md`（消费方）或 `document/SETTINGS.md`（configurable） |
| `PluginType::Generic` 加载规则 / 纯订阅者插件约定 | `document/SETTINGS.md` §3.3、`document/README_SDK.md`、`README.md`/`README.en.md` 的状态表 |
| 区域截图 / `capture.request` 负载 / region 坐标与裁剪语义 | `document/CAPTURE.md` §3.1、§4、`document/EVENT_BUS.md` §4、`plugins/region_selector/README.md`、`README.md`/`README.en.md` 的状态表 |
| 截屏结果消费方（通知 / 剪贴板）行为 | `plugins/notify/README.md`、`plugins/clipboard/README.md`、`README.md`/`README.en.md` 的状态表 |
| 托盘图标来源 / 通知分工 | `plugins/tray/README.md`、`README.md`/`README.en.md` 的托盘说明（图标是宿主**命名资源** `IDI_ICON1`） |
| 多显示器 / DPI 行为 | `document/CAPTURE.md` §2.1、`document/SETTINGS.md` §6.1、`plugins/screen_capture/README.md`、`plugins/settings_ui/README.md`、`README.md`/`README.en.md` 的「多显示器与 DPI」 |
| CI / 跨平台构建 | `README.md` 的「构建」「平台」、`README.en.md` 对应段、`.github/workflows/cmake.yml` 注释、`document/README_SDK.md` 的编译说明 |
| 目录结构、文件改名、新增模块 | `README.md`/`README.en.md` 的目录树、本文 §2 |
| 构建目标 / 产物布局 / CMake 开关 / 最低版本 | `README.md` 的「构建」「产物布局」、`CMakeLists.txt` 注释、本文 §8 |
| SDK 导出内容 | `document/README_SDK.md`（唯一源；`export_sdk` 会复制成 `sdk/README.md`） |
| 对外行为变了但接口没变 | `README.md`/`README.en.md` 的状态表（✅）与对应 `document/*.md` |
| 约定本身变了 | 本文 `STYLE.md`（必要时补进 §11 历史问题） |

- 拿不准要不要改文档时，问自己：**半年后的人按文档操作会不会失败？** 会 → 改。
- 文档里尽量不写容易过期的数字（行号、行数、文件个数）；写了就必须同提交更新。
- 提交前过一遍 §12 清单。

---

## 10. 提交与协作

- 提交信息：**简短中文祈使句，无前缀、无 scope、无 issue 号、不写 body**。例：
  `修正示例插件的依赖` / `增加库提供插件，分离宿主的opencv模块，完善metadata语义`
- 一个提交只做一件事。分支只有 `main`，没有 tag/release 流程。
- **代码与文档同提交**：凡 §9.1 矩阵命中的文档，必须和代码在同一个提交里改完；
  只有代码、没有文档的提交视为未完成。
- **提交前**：`git status` 确认没有把 `build/`、`sdk/`、`build_number.txt` 的自动变化混进本次改动。
- 当前工作区可能处于**重构进行中**（存在未跟踪文件、刚移动位置的头文件、`plugin_api.h` 新增的能力）。接手第一件事：`git status --short`，先分清"哪些是未完成的重构"，不要把人家的半成品当成既定风格去模仿。

---

## 11. 历史问题（已修复，不要回退）

> 2026-11-05 一次性修掉下表 17 条，并通过 `cmake --build build/Windows` 编译，
> 以及运行 `build/Windows/bin/Plugin-shot.exe` 的端到端冒烟（3 个插件加载、abi 1.1、
> 配置读写、UI 事件往返、干净退出）。**新代码不要重新引入这些写法。**
> 之后新增的行按 #18 起继续追加。

| # | 位置 | 当时的错误 | 现在的正确写法 |
| --- | --- | --- | --- |
| 1 | `include/PluginInstances/base.h` | `postToPlugin` 空函数体、非 void 无返回 → MSVC **C4716**，整个工程编译不过 | 返回 `0`；子类用 `override` |
| 2 | `include/PluginInstances/UIProvider.h` | 返回 `int32_t` 却 `return false;`（= 0 = 成功）；且漏 `const`，`override` 失配（C3668） | `0` = 已入队、`-1` = 失败；签名 `const override` 与基类一致 |
| 3 | `include/hostServices.h`、`src/HostServices.cpp` | `boardcastEvent` / `g_boardcastQueue` 拼写错误 | `broadcastEvent` / `g_broadcastQueue` |
| 4 | `include/PluginInstances/dllHandler.h`（原 `dllHander.h`） | 文件名拼写错；成员区 14 行多缩进 4 空格 | 已更名 `dllHandler.h`（含 `base.h` 与两份 README）；缩进已对齐 |
| 5 | `pluginManager.h`、`plugin_meta.h`、`hostServices.h`、`base.h` | `namespace Plugin{` 缺空格、闭括号裸 `}` 或 `} //` | `namespace Plugin {` + `}  // namespace Plugin`（Host 同理） |
| 6 | `include/shot_semver.h` | `Compare` 用 PascalCase、`to_string()const` 缺空格 | `compare()`、`to_string() const` |
| 7 | `src/main.cpp` | 空 `while` 忙等 + Allman 花括号（半成品死代码） | 已删除；循环用 K&R |
| 8 | `processors.hpp`（旧窗口插件已删除） | 两个同名 `copy_config_value` 语义相反：一个溢出返回 -1（违反契约），一个只报告长度 | 现有实现统一为契约语义：`>= 0` 是长度，**负数只表示 key 不认识** |
| 9 | `plugins/opencv_provider/src/plugin_entry.cpp` | `g_module`；手写 interface 查询 + `const_cast`；`init` 不校验 `abi_major` | `kModule`；`shot_plugin_helpers.h` + `SHOT_OPS` + `kIfaces[]`；`init` 校验 `abi_major` |
| 10 | `src/plugin_meta.cpp`、`include/pluginManager.h` | 行尾多余空格 | 已清 |
| 11 | `README.md:35` | 中文被写坏成 `依���驱动` | 已修回「依赖驱动」；保存后确认无 U+FFFD |
| 12 | `CMakeLists.txt:26,28`、`vcpkg.json:2`、`.vscode/launch.json:8,20`、`cmake/kill_running.*` | `my-project` / "My Company" 模板残留 | 改为 `Plugin-shot` + 真实描述 |
| 13 | `CMakeLists.txt` | `ENABLE_UPGRADE` 指向不存在的 `upgrade/`，一打开就 configure 失败 | 该特性已整体删除；现有开关只剩 `ENABLE_KILL_RUNNING` |
| 14 | `README.md:32` / `README.en.md:35` | 状态表说"宿主尚未读取元数据"，与代码矛盾 | 改为"宿主加载前读取：预筛 + 依赖拓扑排序"，中英同步 |
| 15 | `document/README_SDK.md` ↔ `sdk/README.md` | 生成物硬编码且已漂移，还含假事实（"宿主链接 OpenCV"） | `document/README_SDK.md` 是唯一源，`export_sdk` 直接 COPYONLY 复制；假事实已删 |
| 16 | `CMakeLists.txt`、`CMakePresets.json`、插件 `CMakeLists.txt` | 最低版本三个值（3.15 / 3.21 / 3.20） | 根与插件/模块统一 **3.20**，presets **3.21**（schema v3 要求），README 已同步 |
| 17 | `src/main.cpp` | 跨边界结构体 `ShotEvent` 用位置初始化 | 一律用指定初始化器（`.struct_size = ...`） |
| 18 | 旧 Win32 窗口插件 | `WM_NCCREATE` 里直接 `return TRUE`，`DefWindowProcW` 拿不到 `CREATESTRUCTW::lpszName`，窗口标题为空 | 注册完 `GWLP_USERDATA` 后 `return DefWindowProcW(wnd, msg, wp, lp);` |

**这类问题的共同点**：它们都不是"风格偏好"，而是**编译不过 / 语义反了 / 文档在撒谎**。
修的时候顺手把 `git status` 看一遍，别把正在进行的重构半成品一起提交。

## 12. 新代码自检清单

- [ ] 我改的是"契约 / 宿主 / 插件"哪一侧？该侧的规则我看了吗？
- [ ] 命名符合 §3 的分层规则？（C 边界 snake，C++ 方法 camel，常量 `k`，成员 `m_`，全局 `g_`）
- [ ] 4 空格、K&R、`if (` 有空格、行宽 ≤120、行尾无空格、文件末尾有换行？
- [ ] 头文件 `#pragma once`？包含顺序是 自身 → 标准库 → 项目头？
- [ ] 跨边界结构体：第一个字段 `struct_size` 由我填了吗？读字段前 `SHOT_HAS_FIELD` 了吗？
- [ ] 插件回调里 `void* self` 走的是 `state_of(self)` 吗？`ShotImage` 是按行寻址的吗？
- [ ] 异常会不会跨出 `extern "C"`？失败路径是否都变成了返回码/`false`？
- [ ] 谁分配谁释放：插件对象由插件 `destroy` 了吗？宿主有没有 `delete` 插件对象？
- [ ] 中文注释解释的是"为什么"吗？有没有把踩坑结论写下来？
- [ ] 改契约了吗？→ 改 `document/ABI_POLICY.md` 的登记表 + `static_assert` + 版本号。
- [ ] 改 metadata 了吗？→ 顺序仍与 `kEntries[]` 一致吗？`cmake/ValidatePluginMeta.cmake` 会校验。
- [ ] 改 README 了吗？英文镜像同步了吗？目录树同步了吗？
- [ ] 我写的公共 API 有 Doxygen 注释吗？参数 / 返回值 / 约束写清楚了吗？（§5.1）
- [ ] 没做完的事写成代码里的 `// TODO:` 了吗？没有写进 README/文档吧？（§5.2）
- [ ] 这段逻辑进了它自己的类/文件吗？有没有硬塞进 `PluginManager` 之类的已有类？（§6.6）
- [ ] 类的新函数都在 `.h` 声明了吗？`.cpp` 里没有头文件没声明的具名函数吧？（§6.7）
- [ ] 一个头文件只声明一个类吗？`.cpp` 是不是同时干了几件事、该按 §6.8 拆开？
- [ ] 代码改了，§9.1 矩阵里对应的文档是否已在同一提交同步？0
- [ ] 我是不是不小心把 `build/`、`sdk/`、`build_number.txt` 的自动变化带进提交了？

---

## 13. 附录：接手者第一小时

0. **[`AGENTS.md`](AGENTS.md)（先读这份）**——构建/验证命令、本机环境、已踩过的真机坑、当前 backlog、
   多 agent 协作约定。它是"怎么干活"的手册，本文件是"代码长什么样"的约定，两者同等重要。

**读**（按顺序，约 40 分钟）：

1. `README.md` 的「设计理念 / 当前状态 / 目录结构」——知道这东西现在能干什么。
2. `document/ABI_POLICY.md`——只追加规则与 interface 登记表。
3. `include/plugin_api/basics.h` 开头的几段 banner——整个契约的三条纪律（版本、
   `plugin_api/plugin_info.h` 的元数据标记、`plugin_api/interfaces.h` 的 interface id 登记）。
4. `src/PluginManager.cpp` 的 `loadPlugins()`——五个阶段的加载流程。
5. `include/config.h`、`include/hostServices.h`、`include/hostRuntime.h`、`include/hostListCatalog.h`——宿主基础设施（配置 / 服务表 / 运行期装配 / 列表目录），每件一个类或一组文件，是 §6.6 的范本；日志直接用 spdlog，初始化在 `Host::Logging`（`src/hostLogging.cpp`）。
6. `plugins/example_processor/`（整个目录，尤其 `src/plugin_entry.cpp`）——最完整的插件模板。
7. `document/METADATA.md`——元数据为什么只是"声明"。
8. `document/SETTINGS.md`——设置页、`config_list` 与插件热卸载（宿主 + 设置插件两侧都涉及）。
9. `document/LISTS.md`——通用列表 + 事件驱动：新数据形状为什么用 list id + JSON schema，而不是加 ABI。
10. `document/CAPTURE.md`——采集 → 处理 → 保存流水线，`capture.*` 事件与 `--capture`。

**跑**：

```bash
export VCPKG_ROOT=/path/to/vcpkg        # Windows: set VCPKG_ROOT=D:\vcpkg
cmake -S . -B build/Windows -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows
cd build/Windows/bin && ./Plugin-shot.exe    # 必须在 bin/ 下启动（插件目录是相对路径 ./plugins）
```

**别信**：`sdk/`（旧快照）、`build/`（产物）、README 的目录树（可能滞后于重构）、工作区里的半成品（先看 `git status`）。
**代码是唯一事实。**
