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
│   ├── plugin_entry.cpp                # 唯一的导出符号 shot_module_entry + 索引目录
│   ├── invert.cpp                      # 插件 0：能力目录 + 配置 + 实现
│   └── grayscale.cpp                   # 插件 1：同上
└── README.md
```

一个插件一个 `.cpp`：能力声明、配置项、实现都在这一个文件里，想加插件就复制一份。

## 构建

宿主仓库内：由根 `CMakeLists.txt` 自动 `add_subdirectory`，跟着整个工程一起构建。

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

## 运行期贡献：给托盘加一条命令

示例插件还演示了「插件不碰宿主 UI 代码、也不新增 ABI 结构体，就能往宿主里加东西」：

- `example.invert` 实例化时（`src/plugin_entry.cpp` 的 `create_instance`）向事件总线
  发布一条 `lists.add` 事件，往 `tray_command_list` 里声明一个条目
  「示例：截屏（示例插件）」，点击时发布 `capture.request`。
- `owner` 必须等于发布者插件 id（`example.invert`）：宿主按它校验来源，并在插件被
  禁用/卸载时自动清理这条贡献，所以禁用 `example_processor` 后菜单项会消失、
  重新启用后回来。
- 托盘插件完全不认识这条命令，它只是把宿主聚合出来的 `tray_command_list` 渲染成菜单；
  点击后发布的 `capture.request` 和 core 自带的「截屏」走同一条采集流水线。
- 一个 DLL 里有两个插件，但这条贡献只属于 `example.invert`；DLL 内的 `static` 标记
  保证只发一次。标记在实例销毁时清掉，DLL 重载后也会自然重置，所以启停
  `example.invert`（哪怕 `example.grayscale` 还在、DLL 没卸载）都能重新声明。

数据形状（有哪些字段、谁排序）不进 ABI，协议见 `document/LISTS.md`。

## 加一个自己的插件

1. 在 `src/` 下新建一个 `.cpp`，照着 `invert.cpp` 写：定义状态结构、
   `process`、可选的配置读写，最后组装 `ShotImageProcessorV1` /
   `ShotConfigurableV1` 两个实例和一张 `kIfaces[]` 目录。
2. 导出一个 `const example_processor::PluginEntry`（名字随意，`plugin_entry.cpp` 引用它）。
3. 在 `plugin_entry.cpp` 的 `kEntries` 末尾加一项，并在 `metadata.json` 的
   `plugins[]` 末尾加对应的 id/name/version —— 两处顺序必须一致。
