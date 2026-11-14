# lib_provider —— 运行库提供者模板（`shot.lib_provider`）

> "我这个包不提供算法，只是把某个库带进进程"——这就是 `lib_provider` 的用途。
> 真实例子是仓库里的 `plugins/opencv_provider`：它让其它插件能 `requires: ["opencv>=4.x"]`。
> 本模板是它的**最小骨架**：宿主能认出"这是库提供者"，其它插件能按名字依赖它。

## 1. 定位

| 项 | 值 |
| --- | --- |
| 插件 id（占位符） | `{{PLUGIN_ID}}` |
| 能力 | `shot.lib_provider`：只回答"库名 + 运行期版本" |
| metadata | `provides: ["acme.mocklib"]`（**只有名字，不写版本**） |
| 依赖方写法 | `requires: ["acme.mocklib>=1.0.0"]`，宿主会先加载本包并校验运行期版本 |
| 状态 | 无配置项、无事件订阅；`create` 只分配一个空状态对象 |

## 2. 用到的能力 / 契约细节

| 名 | 说明 |
| --- | --- |
| `ShotLibProviderV1::get_library_name` | 返回**静态**字符串，必须与 metadata `provides[]` 的名字一致 |
| `ShotLibProviderV1::get_library_version` | 运行期报告；真实插件应从库的版本头/宏取，不要手写 |
| `metadata.plugins[].provides` | 名字列表（不带版本），宿主据此构建"提供者 → 依赖者"图 |
| `GetModuleHandle`/链接 | 真实插件在 CMake 里 `LINK_LIBRARIES`，库 DLL 随宿主环境发布，**不要**放进 `plugins/` |

为什么版本不在 metadata 里：库升级时只需改代码里一处（或直接读库宏），metadata
不会和真实运行版本打架——`document/METADATA.md` §3「刻意不写的东西」有完整理由。

## 3. 改成"真正带库"的插件

1. `metadata.json`：把 `provides` 换成真实库名（如 `opencv`）；`interfaces` 保持
   `["shot.lib_provider"]`；
2. `include/lib_provider.h`：`kLibraryName` / `kLibraryVersion` 换成真实值
   （版本优先 `#define` 取自库头，如 `CV_VERSION`）；
3. `CMakeLists.txt`：在 `shot_add_plugin(...)` 里加 `LINK_LIBRARIES`。SDK 的 OpenCV
   叠加包已按配置给出库目录，Release/Debug 库名不同，用生成表达式：

   ```cmake
   shot_add_plugin({{TARGET}}
       METADATA metadata.json
       SOURCES  src/plugin_entry.cpp
       INCLUDE_DIRS include
       LINK_LIBRARIES
           $<$<CONFIG:Debug>:opencv_core4d>
           $<$<NOT:$<CONFIG:Debug>>:opencv_core4>
   )
   ```

4. 如果库需要初始化/释放，把逻辑放进 `create_state` / `destroy_state` 或模块级
   `init` / `shutdown`（见 `ShotModule` 注释）。

## 4. 目录

```text
{{TARGET}}/
├── CMakeLists.txt
├── metadata.json
├── include/
│   └── lib_provider.h    # 库名 / 版本两个常量
└── src/
    └── plugin_entry.cpp  # 唯一导出符号 + 模块表 + lib_provider 函数表
```

本模板刻意只有一个 `.cpp`：lib_provider 没有状态、没有配置、没有线程，不需要
拆文件；真正带库时再把"库初始化/查询"拆成独立源文件。

## 5. 构建

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOT_SDK_ROOT=<SDK 根>
cmake --build build
:: 产物：build/bin/plugins/{{TARGET}}.dll
```

## 6. 自检

```bat
build\Windows\bin\Plugin-shot.exe --test-plugin build\bin\plugins\{{TARGET}}.dll --json-out selfcheck.json
```

期望 `结果: PASS（exit 0）`，插件检查里 `type=lib_provider`；宿主会校验
`provides` 与 `get_library_name` 一致、版本号可解析。

## 7. 安装到宿主

把 DLL 复制到宿主 `bin/plugins/`。注意：**库本身**（如 `opencv_core4.dll`）属于宿主
运行环境，放在宿主 `bin/` 或系统 PATH 里，不要塞进 `plugins/`——那个目录只该有
插件自己的动态库。

## 8. 已知限制 / 刻意不做

- 不实现任何配置项或 UI：提供者插件通常不需要用户配置。
- 不做运行时加载（`LoadLibrary`/`dlopen`）：模板直接链接库，让加载失败在宿主启动
  时就暴露；需要"按需加载"时再自己加。
- `provides` 不写版本是刻意的（见 §2）；不要为了让 `requires` 看起来"精确"而把版本
  塞进 metadata，宿主会拒绝。
