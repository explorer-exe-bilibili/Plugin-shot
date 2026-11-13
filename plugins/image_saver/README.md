# image_saver —— 图像保存插件

把宿主流水线处理完的 `ShotImage` 落盘成 PNG/JPEG：格式、质量、输出目录、文件名
模板都是本插件自己的配置项，宿主不硬编码任何保存策略。编码用 vcpkg 的
`stb`（header-only）；除"取系统图片目录"要调 Windows 的 KnownFolder API（POSIX
只解析 XDG 配置文件、不引入第三方依赖）之外，其余只用标准库。

插件 id 是 `image_saver.main`，同时实现两个能力：`shot.image_saver`（保存一张图）
与 `shot.configurable`（声明并读写四个配置项）。

## 目录

```
plugins/image_saver/
├── CMakeLists.txt              # shot_add_plugin + find_package(Stb)/nlohmann_json
├── metadata.json               # 构建期贴到 DLL 尾部：id / abi 2.0 / interfaces
├── include/                    # 头文件与 .cpp 一一对应，一个类一个头（STYLE §6.8）
│   ├── plugin_entry.h          #   内部头：PluginEntry / PluginInstance / state_of
│   ├── image_saver.h           #   ImageSaver：保存能力 + 配置能力 + 语言表生命周期的主体
│   ├── image_encoder.h         #   ImageEncoder：BGRA→RGBA/RGB + stb 编码
│   ├── output_path.h           #   OutputPath：系统图片目录、模板展开、非法字符清洗、唯一文件名
│   └── saver_config.h          #   SaverConfig：四个配置项 + 线程安全读写
└── src/
    ├── plugin_entry.cpp        # 唯一导出符号 shot_module_entry + kEntries 索引目录
    ├── image_saver.cpp         # ImageSaver 实现 + 语言表 + ABI 回调/函数表 + 插件身份证
    ├── image_encoder.cpp       # stb_image_write 实现展开（全仓只在这里展开一次）
    ├── output_path.cpp         # 路径解析、系统图片目录与文件名规则
    └── saver_config.cpp        # 配置描述符（label/tooltip 写语言表 key）+ get/set
```

## 构建

宿主仓库内：由根 `CMakeLists.txt` 的 `file(GLOB plugins/*/CMakeLists.txt)` 自动
`add_subdirectory`，跟着整个工程一起构建；新增插件不需要改根构建脚本。

```bash
cmake --build build/Windows --target image_saver
```

离线 SDK 独立构建：

```bash
cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

产物：`build/Windows/bin/plugins/image_saver.dll`（导入库在 `build/lib`，pdb 在
`build/pdb/plugins`）。`stb` 与 `nlohmann_json` 都是 header-only，只是编进本 DLL，
不产生运行时依赖（后者用于解析语言表）。

## 能力

### `shot.image_saver`（`ShotImageSaverV1`）

| 函数 | 行为 |
| --- | --- |
| `save(self, image, directory, basename, path, path_size)` | 按配置/覆盖参数把图像落盘，成功时把最终绝对路径写进 `path` |

参数与返回码按 `include/plugin_api.h` 的契约：

| 情形 | 返回码 | 副作用 |
| --- | --- | --- |
| 成功 | `0` | 文件已完整写出；`path` 是最终绝对路径（UTF-8，NUL 结尾） |
| `image` 为空、尺寸非法、非 BGRA8、`path` 为空 | `1` | 无 |
| `path` 缓冲装不下最终路径 | `2` | **不建目录、不写文件**（先算路径再确认容量） |
| 目录不可写、编码失败、同名后缀用尽 | `3` | 失败时删掉写了一半的文件，不留垃圾 |

`directory` / `basename` 是本次调用的覆盖参数：为 `NULL` 时用插件配置，非 `NULL`
时以传入值为准（设置页/命令行"另存为"会用到）。`basename` 按契约不含扩展名。

### `shot.configurable`（`ShotConfigurableV1`）

插件声明四个配置项，宿主据此生成设置表单、存盘、启动时回灌；插件只负责 get/set，
落盘归宿主（见 `document/SETTINGS.md`）。不认识的 key 返回负值、保持可用状态。

## 配置项

描述符里的 `label` / `tooltip` 不写中文，写的是语言表 key（文案本体见「多语言」一节），
设置页按 `display(owner, key)` 解析：

| 配置 key | label key | tooltip key |
| --- | --- | --- |
| `directory` | `config.directory.label` | `config.directory.tooltip` |
| `format` | `config.format.label` | `config.format.tooltip` |
| `quality` | `config.quality.label` | `config.quality.tooltip` |
| `filename` | `config.filename.label` | `config.filename.tooltip` |

| key | kind | 默认值 | 取值 | 说明 |
| --- | --- | --- | --- | --- |
| `directory` | PATH（`options="directory"`） | 空 | 任意目录 | 空 = 系统图片目录（Windows 的「图片」库、Linux 的 XDG 图片目录）；相对路径相对宿主工作目录；设置页「浏览…」弹的是**选目录**对话框 |
| `format` | ENUM | `format.png` | `format.png` / `format.jpeg` | PNG 无损（保留 alpha）；JPEG 有损、无 alpha。候选值本身就是文案 key，旧配置里的 `png` / `jpeg` 仍然接受 |
| `quality` | INT | `90` | `1`–`100`（step 1） | 只对 JPEG 生效；越界值夹到区间内 |
| `filename` | STRING | `shot_%Y%m%d_%H%M%S` | 任意模板 | 不含扩展名；支持 `%Y %m %d %H %M %S` 与 `%%` |

配置值一律是字符串（契约要求），非法值安静回退默认，不会让宿主或插件崩掉。

## 多语言（i18n）

> 语言表同时包含插件自身的 `plugin.name` / `plugin.description`：设置页的插件分区标题按
> `display(插件 id, "plugin.name")` 解析（查不到时回退成 key 本身）。

用户可见文案走 `shot::I18n`（`include/shot_plugin_helpers/shot_i18n.h`）。这条链路
零 ABI 改动：插件用 `lists.add` 把语言表发布到 `i18n_source_list`，宿主汇总后生成
只读的 `i18n_list`，所有消费方按同一条规则解析：

```text
display(owner, key) = i18n_list[(owner, key)] ?? key
```

- **语言表位置**：`src/image_saver.cpp` 文件作用域的 `kLangTables[]`，每种语言一个
  JSON 对象，`@meta` 是 BCP-47 语言标签（当前 `zh-CN` / `en-US`）。两张表的 key 集
  必须完全一致，每个 key 两种语言都要有文本。
- **加载时机**：`image_saver.main` 没有 `ui_provider` 的 init/deinit，
  `create(index, host)` 是唯一能拿到宿主服务表的时机；`create_instance` 在状态建好后
  调 `ImageSaver::start(host)` 构造 `std::unique_ptr<shot::I18n>` 并
  `loadTables(kLangTables, std::size(kLangTables))`，`destroy_state` 里 `stop()` 释放
  （`I18n` 析构会退订 `lists.changed`）。owner 用插件身份证 `kImageSaver.id`
  （`image_saver.main`），与 `metadata.json` 一致。
- **key 规范**：
  - 配置项：`config.<配置 key>.label` / `config.<配置 key>.tooltip`，见上面的配置表；
  - 插件自己的 UI 文案：`ui.*`（本插件没有窗口/对话框，因此暂无 `ui.*` key）；
  - ENUM 候选值 `format.png` / `format.jpeg` 本身就是文案 key（同时也是存储值），
    设置页按 `display(owner, 候选值)` 解析成「PNG（无损）」这类用户文案。
- **回退**：宿主拿不到语言表时（老宿主没有列表服务、语言表解析失败），设置页按
  上面的规则回退显示 key 本身；若将来新增 `i18n.tr(key, fallback)` 调用，
  `fallback` 必须写中文原文。本插件当前没有 `i18n.tr(...)` 调用点：用户可见文案只有
  四个配置项的 label/tooltip，由设置页按 `i18n_list` 解析；保存失败原因只进中文日志。
- **不翻译的东西**：日志（`shot::Logger`）保持中文——e2e 按中文日志断言；配置 key
  `directory` / `format` / `quality` / `filename`、ENUM 存储值 `format.png` /
  `format.jpeg`、事件主题也不翻译。
- **新增一条文案**：在两张语言表里同时加同一个 key，再在 `saver_config.cpp` 里用
  key 引用；只加一种语言时 `loadTables` 会记 WARN（各语言 key 数不一致），缺文本的
  语言按回退链取。

## 文件命名规则

1. 文件名模板按**本地时间**展开：`%Y` 四位年、`%m`/`%d`/`%H`/`%M`/`%S` 两位数字，
   `%%` 是字面 `%`；不认识的占位符原样保留。不用 `strftime` 是为了避开 MSVC 对
   非法占位符触发 invalid parameter handler 的行为——模板来自可被手改的配置。
2. 展开结果里的路径分隔符与 Windows 非法字符（`< > : " / \ | ? *`、控制字符）
   替换成 `_`，并去掉尾随空格/点、避让 `CON`/`NUL`/`COM1` 这类保留设备名。
   配置不是可信输入，不能让它把文件写到目录之外。
3. 目录为空时用系统图片目录（Windows 的「图片」库、Linux 的 XDG 图片目录），
   解析优先级：
   - **Windows**：`SHGetKnownFolderPath(FOLDERID_Pictures)`（跟随用户在「图片 →
     属性 → 位置」里的重定位）→ `%USERPROFILE%\Pictures` → 当前工作目录；
   - **POSIX**：`$XDG_PICTURES_DIR` → `$XDG_CONFIG_HOME/user-dirs.dirs`（缺省
     `$HOME/.config/user-dirs.dirs`）里的 `XDG_PICTURES_DIR`（展开 `$HOME`）→
     `$HOME/Pictures` → 当前工作目录。只接受绝对路径，相对路径视为不可用。
4. 目标文件已存在时依次尝试 `name-1`、`name-2`…（最多 1000 次），**绝不覆盖**
   用户已有文件；用尽返回 `3`。
5. 最终路径由 `std::filesystem::absolute` 绝对化后写入 `path`；Windows 上路径经
   `char8_t` 构造转宽字符，中文目录可用。

## 编码流程

1. 校验图像：`data` 非空、宽高为正、`channels == 4`（BGRA8）、`|stride|` 装得下
   一行；老宿主的 `ShotImage` 没有 `pixel_format` 时按 `channels == 4` 当 BGRA8。
2. 按 `stride` **逐行**把 BGRA 转成紧凑缓冲：PNG 转 RGBA（保留 alpha），JPEG 转
   RGB（丢弃 alpha）。`stride` 可为负（自底向上存储）、可带行尾填充，绝不把
   `data` 当连续块。
3. 用 `stbi_write_png_to_func` / `stbi_write_jpg_to_func` 编码到自己的
   `std::ofstream`（而不是 stb 的按文件名接口），这样路径可以先变成宽字符，跨平台
   支持 UTF-8 路径。

## 线程模型

- `save` 只在宿主调用线程执行，同一实例不会被并发调用；
- `set_config` / `get_config` 可能在任意线程被调用，与 `save` 并发读写同一份配置，
  所以 `SaverConfig` 用一把互斥锁保护，`save` 先取快照再编码，不在持锁期间做
  文件 IO。

## 平台说明

- Windows：系统图片目录走 `SHGetKnownFolderPath(FOLDERID_Pictures)`（链接
  `shell32` / `ole32`），默认配置下的落盘路径已在真机验证；用户把「图片」库
  重定位到别的盘后会自动跟随（KnownFolder API 的语义），但**本机没有改过图片库
  位置，未实测重定位场景**。
- POSIX：XDG 解析（`$XDG_PICTURES_DIR` / `user-dirs.dirs` / `$HOME/Pictures` 回退）
  在 WSL 里用独立探针验证（见 `build/verify/`），未在真实桌面会话里跑过插件；
  `localtime_r` 分支按标准库/CRT 语义编写，本机未实际运行验证。
- 非 BGRA8 的像素格式（例如纯 RGB）不属于 v1 契约，`save` 返回 `1`。

## 刻意不做的事

| 不做 | 原因 |
| --- | --- |
| 把配置项写进 `metadata.json` | 真实 schema 在 `shot.configurable` 里，写两遍必然漂移（`document/METADATA.md` §3） |
| 自己写配置文件 | 存盘键是 `<plugin_id>/<key>`，落盘与启动回灌归宿主（`document/SETTINGS.md`） |
| 覆盖同名文件 | 截图场景下"再来一张"很常见，静默覆盖用户已有文件是不可接受的副作用 |
| 为取 XDG 图片目录引入 GLib 等第三方依赖 | 只要读一个 `user-dirs.dirs` 键，自己解析十几行就够；新增依赖会改变插件的部署面 |
| 自己猜图片目录（硬拼 `%USERPROFILE%\Pictures` / `$HOME/Pictures`） | Windows 的「图片」库可被用户重定位、Linux 的目录名会本地化（`~/图片`），猜出来的路径是错的 |
| 区域采集 / 光标 / 剪贴板 / 图床上传 | 属于采集或其他保存策略，不属于这个插件的 v1 范围 |
| 在 `save` 里起线程/异步写盘 | 契约明确 `save` 同步落盘，异步会让宿主拿到路径时文件还不存在 |

## 检查清单

- [ ] 改配置项只改 `saver_config.cpp` 的 `kConfig[]` 与 `SaverConfig`，没有改宿主代码？
- [ ] 新增配置项时，在 `kLangTables` 的 `zh-CN` / `en-US` 两张表里同时补
      `config.<key>.label` / `config.<key>.tooltip`，并让 `kConfig` 引用这些 key？
- [ ] 新增能力/字段前先读 `document/ABI_POLICY.md`，`struct_size` 与 `SHOT_HAS_FIELD` 用对了吗？
- [ ] `metadata.json` 的 `plugins[]` 顺序仍与 `kEntries` 一致、`interfaces` 与代码一致？
- [ ] 图像只按 `stride` 逐行访问，负 stride 与行尾填充都考虑了吗？
- [ ] 失败路径都收敛成返回码了吗？异常有没有跨出 `extern "C"`？
- [ ] 公共头里的类/函数有 Doxygen 注释吗？未完成项写成代码里的 `// TODO:` 了吗？
