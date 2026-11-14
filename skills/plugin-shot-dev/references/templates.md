# 四个模板：选型、占位符、改哪里

> SDK `templates/` 由 `tools/new_plugin.cmake` 生成工程；占位符替换后直接可编。
> 不确定选哪个时先看 [能力决策表](capability-decision.md)。

## 1. 模板一览

| 模板 | 能力 id | 场景 | 关键文件 |
| --- | --- | --- | --- |
| `generic` | `shot.configurable`（+ 订阅者） | 归档/复制/通知/跑命令/托盘/热键；**默认起点** | `src/plugin_entry.cpp`、`src/generic_plugin.cpp`、`src/lang_tables.cpp` |
| `image_processor` | `shot.image_processor` + `shot.configurable` | 水印/打码/模糊/调色/边框 | `src/*processor*.cpp`（`process()` 实现） |
| `image_saver` | `shot.image_saver` + `shot.configurable` | 自定义保存格式/目录/命名/质量 | `src/*saver*.cpp`、编码器（stb） |
| `lib_provider` | `shot.lib_provider` | 把运行库（典型 OpenCV）带进进程 | `src/*provider*.cpp` + `metadata.provides` |

四个模板共同点（SDK 保证）：

- 只依赖 `<SDK>/include` 与 `<SDK>/cmake`：`list(APPEND CMAKE_MODULE_PATH <SDK>/cmake)` +
  `include(ShotPlugin)` + `shot_add_plugin(...)`；不需要 vcpkg、不 `find_package(OpenCV)`。
- SDK 模式由 `ShotPlugin.cmake` 按 `<SDK>/manifest.json` 自动判定，自动补
  `third_party/include`（nlohmann / stb）与可选 `opencv/` 头与库目录。
- 已处理 `/MD`、隐藏符号、产物目录（`build/bin/plugins/`）、metadata 校验与贴尾、产物剪枝。
- 单个 `src/plugin_entry.cpp` 是**唯一**导出 `shot_module_entry` 的编译单元。

## 2. 生成命令

```bash
cmake -DTEMPLATE=image_processor \
      -DDEST="$WORK/acme_watermark" \
      -DPLUGIN_ID=acme.watermark \
      -DPLUGIN_NAME="ACME 水印" \
      -DPLUGIN_DESCRIPTION="在截图右下角叠加时间水印" \
      -DVERSION=1.0.0 \
      -DOWNER="ACME" \
      -P "<SDK>/tools/new_plugin.cmake"
```

参数：`TEMPLATE`（必填，模板名或绝对路径）、`DEST`（必填，非空需 `-DFORCE=ON`）、
`PLUGIN_ID`（必填）、`PLUGIN_NAME`（缺省 = id）、`PLUGIN_DESCRIPTION`（缺省空）、
`VERSION`（缺省 `1.0.0`）、`OWNER`（缺省空）、`TEMPLATES_DIR`（一般不用传）。

## 3. 占位符

| 占位符 | 替换成 | 出现在 |
| --- | --- | --- |
| `{{TARGET}}` | CMake 目标名 / DLL 名（id 的 `.`→`_`） | `CMakeLists.txt`、源码注释、`README.md` |
| `{{PLUGIN_ID}}` | 插件 id（如 `acme.timestamp`） | `metadata.json`、源码、i18n、lists owner |
| `{{PLUGIN_NAME}}` | 显示名 | `metadata.json`、README |
| `{{PLUGIN_DESCRIPTION}}` | 一行描述 | `metadata.json`、README |
| `{{VERSION}}` | 插件版本 | `metadata.json` |
| `{{OWNER}}` | 作者/组织 | README/注释 |

脚手架会清掉模板里其它未知 `{{XXX}}`（替换为空串），但**不要让它们出现在必须改的字段里**。
生成后自查：`grep -R "{{" <DEST>` 应为空（README 里的说明除外，按模板实际内容判断）。

## 4. 每个模板改哪里

### 4.1 `generic`（Generic 插件，无宿主可调用能力）

1. `metadata.json`：`interfaces` 只声明 `shot.configurable`（订阅者不需要其它能力）；
   `requires: ["core>=0.1.0"]`；id/name/version。
2. `src/plugin_entry.cpp`：`create(index, host)` 里
   - 构造 `shot::Logger` / `shot::EventBus` / `shot::I18n`；
   - `subscribe("capture.done", &on_capture_done, this)`；
   - 需要托盘/热键时 `publish("lists.add", ...)`（owner = 自己的 id）。
   `destroy` 里：停线程并 `join` → 退订 → 释放实例。
3. `src/generic_plugin.cpp`：实现业务（归档/复制/通知…）。事件回调在 main 线程，
   **只解析 payload + 入队**，文件操作放自己的 worker。
4. `src/lang_tables.cpp`：`plugin.name` / `plugin.description` / 配置项文案。
5. `README.md`：功能、配置表、验证步骤、限制。

### 4.2 `image_processor`

1. `metadata.json`：`interfaces: ["shot.image_processor","shot.configurable"]`。
2. `src/*processor*.cpp` 的 `process(self, ShotImage* image)`：
   - 校验 `pixel_format == SHOT_PIXEL_FORMAT_BGRA8`、`channels == 4`、宽高 ≥ 1；
   - 逐行 `uint8_t* row = image->data + y * image->stride;`，只写 `width*4` 字节；
     `stride` 可为负（`data` 指向最后一行）；
   - **不改** `image->width/height/stride/data` 等结构体字段；
   - 返回 `0` 成功/非 0 失败；处理时间要短（worker 串行，慢会拖住截图）。
3. 配置项（强度/颜色/边距）在 `get_config_desc` 声明、`get_config/set_config` 读写；
   跨线程共享的配置值加锁或原子量。
4. 水印类需要字体：模板内置简洁点阵方案（无第三方字体文件）；不建议找系统字体路径。

### 4.3 `image_saver`

1. `metadata.json`：`interfaces: ["shot.image_saver","shot.configurable"]`。
2. `src/*saver*.cpp` 的 `save(self, image, directory, basename, path, path_size)`：
   - `directory`/`basename` 为 `NULL` 时用插件配置；`basename` 不含扩展名；
   - 成功时把**最终绝对路径**写进 `path`（NUL 结尾；缓冲不够返回 `2`）；
   - 返回码：`0` 成功 / `1` 参数非法 / `2` 缓冲不足 / `3` 保存失败；
   - PNG/JPEG 编码用 SDK `third_party` 的 stb（`#include "stb_image_write.h"`，
     在一个 TU 里定义 `STB_IMAGE_WRITE_IMPLEMENTATION`）。
3. `README` 写清"内置保存器仍会先落盘，本插件要生效需禁用内置保存器"这一事实与做法。

### 4.4 `lib_provider`

1. `metadata.json`：`interfaces: ["shot.lib_provider"]`，`provides: ["opencv"]`，
   `requires: ["core>=0.1.0"]`；需要 OpenCV 时链接 `opencv_core4` 等。
2. `src/*provider*.cpp` 实现 `get_library_name` / `get_library_version`（静态字符串，
   版本用 `CV_VERSION` 之类真实版本，不要写死与 SDK 不一致的值）。
3. 这只是"把库带进进程"，不实现图像处理；消费者用 `requires: ["opencv>=4.12"]`。

## 5. 模板之外：示例工程

SDK 随包提供两个完整示例（在 `demos/`）：

- `demos/watermark`：截图右下角时间水印（`image_processor` + 配置项 + i18n）。
- `demos/archive`：按日期归档（Generic 订阅 `capture.done`，移动/复制 + 配置 + 防自归档）。

示例用于"跑通 + 抄结构"，不是 API 事实源；API 以 `include/` 与 `docs/AUTHORING.md` 为准。

## 6. 多插件 DLL（一个 DLL 多个插件）

- `metadata.json` 的 `plugins[]` 顺序必须与入口表数组顺序一致（`create(index)` 的索引）。
- `query_interface` 用 `shot_query_interface(kIfaces, id, version)` 按实例查能力目录。
- 每个实例的 `id/name` 由 `get_info(index)` 报告，与 metadata 对应。
- 包里"只发一次"的贡献（托盘/热键）用静态标记 + 实例销毁时重置（DLL 未必卸载）。
- 部分插件被禁用时 DLL 不卸载：重新启用必须能重新 `create` 并重新贡献。

## 7. 模板生成的工程还要检查

- [ ] `metadata.json` 的 `abi` 与 SDK `manifest.abi` 一致（major 相等、minor ≤）
- [ ] `interfaces` 与 `query_interface` 实际回答一致
- [ ] `requires` 至少含 `core>=<host_min_version>`
- [ ] i18n key 与 `ShotConfigDesc` 的 key 一致（`config.<key>.label`）
- [ ] 没有 `find_package(opencv)` / vcpkg 残留
- [ ] `src/plugin_entry.cpp` 是唯一导出符号的 TU
