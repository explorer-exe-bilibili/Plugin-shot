# tools/package —— 发行物料打包

只放"把构建产物变成可发布 zip"的脚本；构建与验证脚本在 `tools/verify/`。

| 脚本 | 产物 |
| --- | --- |
| `make_host_package.ps1` | `dist/Plugin-shot-<版本>-win64.zip` + `.sha256`（宿主便携包） |

SDK 的两个 zip（核心包 / OpenCV 叠加包）由 CMake 目标 `package_sdk` 生成，见
`cmake/package_sdk.cmake` 与 `document/README_SDK.md`。

## 发布流程（Windows 本机）

```bat
:: 1. Release 构建（独立构建目录，复用已安装的 vcpkg 依赖）
cmake -S . -B build\Windows-release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build\Windows-release

:: 2. 宿主便携包
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package\make_host_package.ps1 -Version 0.2.0

:: 3. SDK 两个 zip
cmake --build build\Windows-release --target package_sdk
```

发布前检查：

- `dist/` 下三个 zip 都有对应 `.sha256`；
- 便携包解压到**干净目录**后能 `Plugin-shot.exe --capture` 落盘（不依赖构建目录）；
- SDK 核心包在没有 vcpkg 的机器上能 `cmake` 构建 `templates/generic`；
- `document/PLUGIN_TEST.md` 里的自检命令在新包上跑通。
