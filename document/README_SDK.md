# 插件开发 SDK

离线 SDK，供第三方插件在**不安装 OpenCV** 的机器上编译链接。

## 目录

| 目录 | 内容 | 用途 |
| --- | --- | --- |
| `include/` | `plugin_api.h`、`plugin_api_export.h`、`shot_plugin_helpers.h`、`opencv4/` | 契约头 + 插件侧小工具 + OpenCV 头 |
| `lib/` | release 版 `*.lib` 导入库 | Release 构建链接期使用 |
| `lib/debug/` | debug 版 `*.lib`（`opencv_*4d.lib` 等） | Debug 构建链接期使用 |
| `bin/` | release 版 `*.dll` 运行时库 | **不要拷进插件目录**，见下 |
| `bin/debug/` | debug 版 `*.dll`（`opencv_*4d.dll` 等） | Debug 宿主/插件运行时使用 |

## 编译

最省事的方式是用 SDK 自带的构建模块（宿主仓库的 `plugins/example_processor` 是完整样板）：

```cmake
list(APPEND CMAKE_MODULE_PATH "<SDK>/cmake")
include(ShotPlugin)

shot_add_plugin(my_plugin
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp src/my_processor.cpp
    INCLUDE_DIRS include
)
```

`shot_add_plugin()` 统一处理 /MD、隐藏符号、产物目录、元数据贴尾与产物剪枝。
手工搭建时的等价最小配置：

```
include_directories(<SDK>/include <SDK>/include/opencv4)
link_directories(<SDK>/lib)
target_link_libraries(my_plugin PRIVATE opencv_core4 opencv_imgproc4 ...)
```

## 必须遵守的 ABI 约定

1. **运行库必须是 /MD（MultiThreadedDLL）**
   MSVC 下用 `set_property(TARGET my_plugin PROPERTY
   MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")`。
   宿主用 /MD 动态运行时，插件若用 /MT 静态运行时会得到两份 CRT，
   跨 DLL 传递 `std::string`/`cv::Mat` 会在释放时崩溃。

2. **Debug/Release 必须与宿主一致**
   Debug 构建的插件只能加载进 Debug 宿主，反之亦然。

3. **插件不得自带基础依赖**
   `opencv_*.dll` 等基础依赖放在宿主 exe 旁边的 `bin/` 里（由
   `cmake/copy_runtime_deps.cmake` 整目录布置；宿主本身不链接 OpenCV）。
   插件目录里再放一份会造成「同一进程加载两个 OpenCV 实例」——
   类型信息、全局状态、内存分配器全部割裂，表现为莫名崩溃或
   `cv::Exception`。插件产物目录只应出现插件自己的 DLL。

4. **`abi_major` 必须与宿主一致**
   版本不匹配的插件会被宿主拒绝加载。升级 SDK 后请整体替换
   `include/`，不要新旧混用。

5. **只导出 `shot_module_entry`，只加字段不加符号**
   整个插件包只有一个导出符号，其余全是 `ShotModule` 里的字段；加能力靠往结构体
   末尾追加字段（读字段前必须检查 `struct_size`）。细节见宿主仓库的
   `document/ABI_POLICY.md`。

6. **能力用 interface id 查询**
   `query_interface(instance, "shot.image_processor", 1)` 返回函数表，
   没有的能力返回 `NULL`。已注册的 id：`shot.image_processor`、`shot.configurable`、
   `shot.lib_provider`（不提供算法、只把运行库带进进程的包用这个身份）。

## debug 变体（重要）

`lib/debug/` 与 `bin/debug/` 是 Debug 构建专用的一套变体。
**绝对不要把两个变体的 DLL 放进同一个目录**：`jpeg62.dll`、`turbojpeg.dll`、
`liblzma.dll`、`libwebp*.dll` 这些文件名在 debug 与 release 下【完全相同】，
混放会互相覆盖且不会有任何报错。运行时的规则是：宿主 exe 所在目录里的那一套
必须与你编译插件时用的那一套一致（Debug 宿主缺 `opencv_*4d.dll` 会启动失败）。

## 注意

本 SDK **不包含** `*.pdb`（调试符号）与 `pkgconfig` 元数据。
需要调试符号请直接使用构建机上的 `vcpkg_installed` 目录。
