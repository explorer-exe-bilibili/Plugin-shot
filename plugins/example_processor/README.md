# example_processor —— 示例插件工程

一个最小的**完整**插件工程：一个 DLL 里装两个 ImageProcessor（invert / grayscale），
用来演示契约、目录结构、元数据贴尾和构建规则。

## 目录

```
plugins/example_processor/
├── CMakeLists.txt                      # 只做三件事：找到 shot_add_plugin、声明源文件、声明元数据
├── metadata.json                       # 插件清单：构建期被贴到 DLL 尾部
├── include/example_processor/
│   └── processors.hpp                  # 行寻址工具 + 两个处理器的声明
├── src/
│   ├── plugin_entry.cpp                # 导出 4 个 C 函数，索引表在这里
│   ├── invert.cpp                      # 插件 0
│   └── grayscale.cpp                   # 插件 1
└── README.md
```

## 构建

宿主仓库内：由根 `CMakeLists.txt` 自动 `add_subdirectory`，跟着整个工程一起构建。

离线 SDK 独立构建：

```bash
cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

产物：`build/bin/plugins/example_processor.dll`（导入库在 `build/lib`，pdb 在 `build/pdb/plugins`）。

## 关键点

1. **索引即契约**：`src/plugin_entry.cpp` 里 `kEntries` 的顺序就是
   `plugin_create(index)` 的索引，必须与 `metadata.json` 的 `plugins[]` 顺序一致。
   宿主不读元数据也能加载，但读元数据（后续版本）就能把索引对回 id/名称。

2. **按行寻址**：`Image.stride` 可以为负、也可以大于 `width * channels`，
   所以 `for_each_row()` 逐行算出有效区间，绝不假设 `data` 连续。

3. **alpha 不取反**：`channels == 4` 时第 4 字节是 alpha，invert 会跳过它。

4. **元数据贴尾**：`shot_add_plugin()` 在链接后把 `metadata.json` 追加到 DLL 尾部，
   用 `plugin_api.h` 里的 `PLUGIN_META_START_MARKER` / `PLUGIN_META_END_MARKER` 分隔。
   标记字面量只有契约头那一份，构建脚本在构建期直接读头文件取宏值。

## 加一个自己的插件

1. 复制本目录，改目录名与 `shot_add_plugin()` 的目标名。
2. 在 `metadata.json` 里替换 `plugins[]`（id 用 `作者.名字` 形式，避免撞名）。
3. 在 `kEntries` 里按同样顺序登记，实现 `int process(void*, Image*)`。
