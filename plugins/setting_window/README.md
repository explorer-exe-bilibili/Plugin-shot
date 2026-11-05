# setting_window —— 插件自带窗口的示例（`shot.ui_provider`）

最小的**插件自有 UI** 工程：窗口归插件，宿主完全不知情。

它要证明的一件事：宿主和插件之间不需要共享任何 Win32 类型。宿主看不到 `HWND`，
也不知道插件开了几个线程；两边只交换 `init` / `deinit` / `post_to_plugin` 三个
函数调用，事件用不透明字节传。

## 目录

```
plugins/setting_window/
├── CMakeLists.txt                  # 找 shot_add_plugin、声明源文件与元数据
├── metadata.json                   # 插件清单：构建期被贴到 DLL 尾部
├── include/setting_window.h        # PluginEntry / PluginInstance / SettingWindow 声明
├── src/
│   ├── plugin_entry.cpp            # 唯一的导出符号 shot_module_entry + 索引目录
│   └── setting_window.cpp          # 能力目录 + Win32 窗口线程 + 事件队列
└── README.md
```

## 构建

宿主仓库内：由根 `CMakeLists.txt` 自动 `add_subdirectory`，跟着整个工程一起构建。

离线 SDK 独立构建：

```bash
cmake -S . -B build -DCMAKE_MODULE_PATH=<SDK>/cmake
cmake --build build
```

产物：`build/bin/plugins/setting_window.dll`。

## 关键点

1. **窗口线程是插件自己的**。`init` 起线程并等窗口真正建好才返回；`deinit` 发
   `WM_CLOSE`、等消息循环退出、`join` 线程后才返回。所以 `deinit` 返回时窗口
   一定已经不在了 —— 宿主不需要（也没办法）自己去关窗口。

2. **`HWND` 不出 DLL**。它是 `SettingWindow::Impl` 的私有成员，头文件里连
   `<windows.h>` 都没包含。PIMPL 不是为了"编译快"，是为了让"Win32 类型顺着契约
   漏出去"这件事在物理上不可能发生。

3. **事件是"宿主 -> 插件"单向投递**。宿主在任意线程调 `post_to_plugin`，插件把
   事件名和 `data_len` 字节拷进队列，再 `PostMessage` 唤醒窗口线程，立即返回。
   拷贝是必须的：投递方的缓冲区在调用返回后就不归插件管了。

4. **`ShotEvent::name` 不拷贝**。它约定是编译期静态字符串，活到模块卸载，
   所以队列里只存 `std::string(name)` 是安全的（`data` 则必须拷字节）。

5. **0 = 成功**。三个函数统一用 `int32_t` 返回、`0` 表示成功，与契约其余部分一致。
   `deinit` 的返回值是宿主唯一能知道"插件没关干净"的渠道，所以它不能只写在析构里。

6. **异常不跨越 DLL 边界**。`std::thread` 构造可能抛（线程资源耗尽），这里
   `try/catch` 兜住并返回失败，而不是让异常穿到宿主。
