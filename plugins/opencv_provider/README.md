# opencv_provider —— 运行库提供者插件

一个**不提供算法、只把 OpenCV 带进进程**的插件包，演示三件事：

1. 能力 `shot.lib_provider`：插件用这个身份表明"我是运行库提供者"，宿主据此把它
   登记成 `PluginType::Lib`（宿主侧包装类 `LibProvider`），而不是一个坏掉的图像处理器。
2. 插件工程**自己声明依赖**：`find_package(OpenCV QUIET)` 写在插件自己的
   `CMakeLists.txt` 里，不依赖宿主有没有 `find_package` 过 OpenCV。找不到时也能编，
   只是报不出库版本（源码里用 `OPENCV_PROVIDER_HAS_OPENCV` 开关）。
3. 元数据里的 `provides`：`metadata.json` 写 `"provides": ["opencv"]`，
   别的包将来可以用 `"requires": ["opencv"]` 表达依赖。

## 目录

```
plugins/opencv_provider/
├── CMakeLists.txt        # shot_add_plugin + 自己 find_package(OpenCV)
├── metadata.json         # provides: ["opencv"]；interfaces: ["shot.lib_provider"]
├── src/plugin_entry.cpp  # 模块表 + shot.lib_provider 能力
└── README.md
```

构建说明：根工程默认构建本插件；configure 时加 `-DSHOT_ENABLE_OPENCV=OFF` 会跳过它
（CI / 没有 OpenCV 的环境用这个开关；默认 `ON`）。

## 运行时行为

```
    [plugin] [OpenCVProvider] initialized, opencv 4.12.0
  + OpenCV Environment Provider
Successfully loaded module: ./plugins\opencv_provider.dll (1 plugins, abi 2.0)
Loaded 2 image processor plugins.
Loaded 1 lib provider plugins.
  ~ OpenCV Environment Provider 4.12.0
```

## 关键点

- `query_interface` 只回答 `"shot.lib_provider"`，其它 id 一律 `NULL`。
  宿主先问 `"shot.image_processor"` 拿不到，再问这个才拿到。
- 能力的返回值（库名、版本）都是插件内的静态字符串，有效期到 DLL 卸载，宿主只读不释放。
- **不要**把 `opencv_*.dll` 放进插件目录：它会和宿主 exe 旁边那一份冲突，
  同一进程里出现两个 OpenCV。`cmake/prune_output_dir.cmake` 会在 POST_BUILD 把
  与 vcpkg 运行时 DLL 同名的基础依赖从插件目录清走（vcpkg applocal 会拷进来）。
- 想真正把 OpenCV 的接口开放给别的插件，做法是**新增一个能力 id**
  （例如 `shot.opencv` + 一个 `ShotOpenCvV1` 结构体）并在 `query_interface` 里多回答一个分支：
  纯新增数据，不需要动 ABI 版本。
