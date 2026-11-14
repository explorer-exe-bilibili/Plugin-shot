# Linux 编译手册

本文是 **Plugin-shot 0.1.0 在 Linux 上从源码编译到运行** 的完整步骤，面向 Ubuntu 24.04 / 26，
其它发行版只需要替换包名。Windows 构建见根 [README](../README.md) 的「构建」。

> **为什么不提供 Linux 预编译包**：Linux 的 `Plugin-shot` 与全部 `.so` 直接链接构建机上的
> **glibc、X11/Wayland、GLib、PipeWire、libnotify** 等系统库，插件还通过 `dlopen` 加载同一次
> 构建的 `.so`。不同发行版、甚至同一发行版的不同大版本之间，glibc 符号版本和系统库 ABI
> 都可能不兼容。仓库里的 `build/Linux` 只是维护者在 WSL Ubuntu 24.04 上的**本地构建产物**，
> 不是可分发包；把它拷到另一台机器上，最常见的结果是 `GLIBC_2.xx not found` 或
> `dlopen` 失败。**请在目标机器上重新编译。**
>
> 本仓库实测：Ubuntu 24.04（glibc 2.39）构建的产物最高只要求 `GLIBC_2.38`，可在 Ubuntu 24 / 26
> 运行；反过来，Ubuntu 26 构建的产物会要求 `GLIBC_2.43`，拷回 Ubuntu 24 会在 `dlopen` 阶段失败。

---

## 0. 30 秒速查

```bash
# 1) 系统依赖（Ubuntu 24.04 / 26）
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git curl zip unzip \
  autoconf autoconf-archive automake libtool libltdl-dev bison flex gperf \
  gettext autopoint python3-venv nasm yasm \
  libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev \
  libxkbcommon-dev libwayland-dev \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev \
  libxfixes-dev libxcomposite-dev \
  libglib2.0-dev libportal-dev libpipewire-0.3-dev libspa-0.2-dev libnotify-dev \
  libselinux1-dev libpam0g-dev

# 2) vcpkg
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=$HOME/vcpkg

# 3) 编译（在仓库的 Host/ 目录）
cd Host
cmake -S . -B build/Linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_TARGET_TRIPLET=x64-linux
cmake --build build/Linux

# 4) 运行（必须在 bin/ 下启动，插件目录是相对路径 ./plugins）
cd build/Linux/bin
./Plugin-shot --help
./Plugin-shot --capture
```

首次 configure 会以 vcpkg 清单模式安装 OpenCV、spdlog、nlohmann-json、GLFW、ImGui、
nativefiledialog-extended、stb 等依赖，**耗时较长**；之后的增量构建很快。

---

## 1. 已验证环境

| 项 | 值 |
| --- | --- |
| 构建 | Ubuntu 24.04.5 LTS / GCC 13.3.0 / x86_64 / CMake 3.28.3 / Ninja 1.11.1 / Release |
| 运行（X11） | Ubuntu 24.04 / X11 / 单显示器 / 单 X screen |
| 运行（Wayland） | Ubuntu 26 / Wayland / 单显示器 |
| 产物 | `build/Linux/bin/Plugin-shot` + `build/Linux/bin/plugins/*.so`（12 个；`long_capture` 仅 Windows） |
| 验证范围 | 整屏采集、区域框选、保存、剪贴板、通知、设置页、语言包、托盘；X11 全局热键；Wayland D-Bus 触发 |

**未验证**：X11 多显示器 / 多 X screen / 负坐标；Wayland 窗口悬停（协议限制）；macOS。
这些限制在 [平台矩阵](PLATFORMS.md) 里有逐项说明。

---

## 2. 系统依赖

### 2.1 构建工具

| 包 | 用途 |
| --- | --- |
| `build-essential` `cmake` `ninja-build` `pkg-config` | 编译器、CMake、Ninja、`pkg_check_modules` |
| `git` `curl` `zip` `unzip` | 拉源码、vcpkg 下载与解包 |
| `autoconf` `autoconf-archive` `automake` `libtool` `libltdl-dev` | vcpkg 构建 dbus 等端口 |
| `bison` `flex` `gperf` `gettext` `autopoint` `python3-venv` | vcpkg 构建工具链 |
| `nasm` `yasm` | 部分编解码端口（libjpeg-turbo 等） |

CMake 最低 **3.20**；使用 `CMakePresets.json` 时需要 **3.21**。Ubuntu 24.04 自带 3.28.3，够用。
如果发行版的 CMake 太旧，用 Kitware 的 apt 源或 `python3 -m venv` 装新版。

### 2.2 图形 / 桌面

| 包 | 用途 |
| --- | --- |
| `libgl1-mesa-dev` `libglu1-mesa-dev` `libegl1-mesa-dev` | OpenGL / EGL（设置页、覆盖层） |
| `libxkbcommon-dev` `libwayland-dev` | GLFW 的 Wayland/X11 后端 |
| `libx11-dev` `libxrandr-dev` `libxinerama-dev` `libxcursor-dev` `libxi-dev` `libxext-dev` | X11 采集、热键、框选、剪贴板 |
| `libxfixes-dev` | X11 光标合成（XFixes） |
| `libxcomposite-dev` | X11 框选在合成器下的背景快照；**可选**，缺了只 WARN（灰遮罩） |

### 2.3 插件专用

| 包 | 用途 | 缺失后果 |
| --- | --- | --- |
| `libglib2.0-dev` | `tray` 的 StatusNotifierItem / dbusmenu、`session_control` 的 GDBus | 这两个插件无法编译 |
| `libportal-dev` | Wayland ScreenCast / Screenshot 门户客户端 | Wayland 采集/框选无法编译 |
| `libpipewire-0.3-dev` | 消费 ScreenCast 的 PipeWire 帧 | 同上 |
| `libspa-0.2-dev` | PipeWire 帧元数据（`spa_meta`） | 同上 |
| `libnotify-dev` | Linux 桌面通知 | `notify` 无法编译 |
| `pkg-config` | 探测 gio / libportal / libpipewire | configure 直接失败 |

> 版本随发行版走：项目统一用 `pkg_check_modules(... REQUIRED)` 探测，不写死版本号。
> 如果只想编译不含 Wayland 采集的最小版本，也建议先装齐，避免 CMake 跳过目标后误以为“功能缺失”。

### 2.4 运行时（桌面环境）

- **Wayland**：`xdg-desktop-portal` + 对应后端（GNOME 是 `xdg-desktop-portal-gnome`）。
  Ubuntu 桌面默认自带；最小安装或容器里需要手动装并启动用户服务。
- **托盘**：需要 `org.kde.StatusNotifierWatcher`。Ubuntu GNOME 默认带
  **Ubuntu AppIndicators** 扩展；纯净 GNOME 需安装 `gnome-shell-extension-appindicator` 并启用。
- **通知**：`org.freedesktop.Notifications` 服务（Ubuntu 桌面自带）；可用 `notify-send` 自测。
- **OpenGL**：桌面版通常自带 Mesa；无 GPU/远程会话可尝试 `LIBGL_ALWAYS_SOFTWARE=1`。

---

## 3. 获取源码与 vcpkg

```bash
git clone https://github.com/explorer-exe-bilibili/Plugin-shot.git
cd Plugin-shot/Host

# vcpkg 放在仓库外，避免被 git 跟踪
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=$HOME/vcpkg

# 建议写进 shell 配置
echo 'export VCPKG_ROOT=$HOME/vcpkg' >> ~/.bashrc
```

项目使用 **vcpkg 清单模式**：`vcpkg.json` 声明依赖，`vcpkg-configuration.json` 钉 baseline；
首次 configure 会自动把依赖装到 `build/Linux/vcpkg_installed/x64-linux`。
不要手动改 baseline，也不要混用系统 OpenCV/GLFW，否则容易出现两份运行时。

---

## 4. 配置与编译

### 4.1 标准 Release 构建

```bash
cd Plugin-shot/Host
export VCPKG_ROOT=$HOME/vcpkg

cmake -S . -B build/Linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_TARGET_TRIPLET=x64-linux

cmake --build build/Linux
```

- 需要调试符号时把 `Release` 换成 `Debug`；Debug 产物体积大、运行慢，但日志更全。
- Ninja 默认并行；内存较小（< 4 GB）的机器可加 `-- -j2` 限制并行度。
- 首次构建会先编译 vcpkg 的 OpenCV/GLFW/ImGui 等依赖，可能 10–30 分钟；之后增量构建通常几十秒。

### 4.2 用 CMake preset

```bash
cmake --preset release -DVCPKG_TARGET_TRIPLET=x64-linux
cmake --build --preset release
```

preset 的产物目录同样是 `build/Linux`。如果之前用普通命令配置过，建议删掉
`build/Linux/CMakeCache.txt` 再切 preset，避免缓存里的工具链路径冲突。

### 4.3 跳过 OpenCV（可选）

```bash
cmake -S . -B build/Linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DSHOT_ENABLE_OPENCV=OFF
```

只跳过 `opencv_provider` 与 `example_processor` 两个插件目标，**不会**让 vcpkg 少装 OpenCV
（清单模式不支持按选项剔除依赖）。没有 OpenCV 的机器可以用这个开关缩短插件编译时间。

### 4.4 导出离线插件 SDK（可选）

```bash
cmake --build build/Linux --target export_sdk
```

产物在仓库根的 `sdk/`（不入库），第三方插件可以只依赖它编译。

### 4.5 产物

```text
build/Linux/bin/Plugin-shot          # 宿主
build/Linux/bin/plugins/*.so         # 12 个插件（long_capture 仅 Windows）
build/Linux/bin/files/lang/          # 官方语言包（含 zh-TW）
```

---

## 5. 运行

```bash
cd build/Linux/bin

./Plugin-shot --help       # 打印用法，退出码 0（无头环境也能跑）
./Plugin-shot --capture    # 截一张、打印路径后退出
./Plugin-shot --settings   # 打开设置页后常驻（调试入口）
./Plugin-shot              # 托盘常驻
```

> **必须在 `bin/` 下启动**：插件目录按相对路径 `./plugins` 解析。用绝对路径启动也要先
> `cd` 到 `bin/`，或者用下面第 7 节的 launcher。

运行期路径：

| 项 | 路径 |
| --- | --- |
| 配置 | `~/.plugin-shot/config.json` |
| 日志 | `~/.plugin-shot/logs/host.log` |
| 截图 | XDG 图片目录（`$XDG_PICTURES_DIR` → `user-dirs.dirs` → `$HOME/Pictures`；中文桌面通常是 `~/图片`） |

`--capture` 成功时日志有 `Capture saved: <path>`，退出码 0；采集/保存失败退出码 1，
未知参数 2。插件加载失败不会让宿主崩溃，但对应功能会缺失，先看 `host.log` 里的
`Successfully loaded module` / `plugin.error.*`。

---

## 6. Wayland 与 X11 的差异

先确认会话类型：

```bash
echo "$XDG_SESSION_TYPE"   # wayland / x11
echo "$WAYLAND_DISPLAY"    # Wayland 下非空
echo "$DISPLAY"            # X11 或 XWayland 下非空
```

### 6.1 Wayland

- **采集**：走 `xdg-desktop-portal` 的 ScreenCast（PipeWire）。首次截图桌面会弹授权框，
  选择“共享整个屏幕”或某个窗口；每次新建会话是否免弹窗取决于桌面与 portal 版本，不要假设一定免弹。
- **区域框选**：先用门户抓一张定格图，再在 Plugin-shot 自己的窗口里拖选；Wayland **没有**
  窗口悬停吸附（协议不允许读其它窗口几何）。
- **全局热键不可用**：Wayland 没有可移植的全局热键 API，`hotkey` 插件会 WARN 并降级为
  “只提供列表与配置项、不注册”。用 `session_control` 的 D-Bus 入口 + 桌面自定义快捷键：
  ```bash
  gdbus call --session \
    --dest org.pluginshot.Control \
    --object-path /org/pluginshot/Control \
    --method org.pluginshot.Control.Capture
  ```
  把同一条命令绑到 GNOME「设置 → 键盘 → 自定义快捷键」（或 KDE / Sway 的对应配置）。
  可选方法：`Capture` / `CaptureRegion` / `OpenSettings` / `CaptureLong`。
- **剪贴板**：经 XWayland 的 X11 selection 实现；没有 XWayland 的纯 Wayland 会话不可用。
- **托盘**：StatusNotifierItem + dbusmenu，需要 AppIndicator 扩展（见 §2.4）。
- **通知**：libnotify → `org.freedesktop.Notifications`，Wayland/X11 都可用。

### 6.2 X11

- **采集**：`XShmGetImage` 优先，失败回退 `XGetImage`；XFixes 合成光标。
- **区域框选**：GLFW 覆盖层 + XComposite 背景快照；悬停窗口吸附、单击截窗、拖选、Esc 取消。
  缺 `libxcomposite-dev` 时只 WARN，合成器下可能看到灰遮罩。
- **热键**：`XGrabKey`，默认 `Ctrl+Alt+S` / `Ctrl+Alt+A` / `Ctrl+Alt+O`。
  X11 没有 `MOD_NOREPEAT` 等价物，按住热键会按服务器自动重复连续触发，这是已知差异。
- **托盘**：同一份 SNI + dbusmenu 实现，X11 下也可用。

### 6.3 单显示器限制

本轮真机验证都是**单显示器**。X11 的多显示器 / 多 X screen / 负坐标尚未验证；
Wayland 的多屏 / HiDPI 拼接也尚未实现。多屏用户请先看
[`document/PLATFORMS.md`](PLATFORMS.md) 的矩阵，不要按“全平台可用”预期。

---

## 7. 安装到系统（可选）

项目的 `cmake --install` 目前只安装宿主与 `files/`（到 `<prefix>/bin`），**不安装
`plugins/`**；在安装规则补齐前，推荐“整目录 + launcher”的方式，保留 `plugins/` 相对布局：

```bash
sudo mkdir -p /opt/plugin-shot
sudo cp -a build/Linux/bin/. /opt/plugin-shot/

sudo tee /usr/local/bin/plugin-shot >/dev/null <<'EOF'
#!/usr/bin/env bash
cd /opt/plugin-shot && exec ./Plugin-shot "$@"
EOF
sudo chmod +x /usr/local/bin/plugin-shot
```

桌面入口（可选）：

```ini
[Desktop Entry]
Type=Application
Name=Plugin-shot
Comment=万物皆插件的截图工具
Exec=/usr/local/bin/plugin-shot
Icon=applications-graphics
Terminal=false
Categories=Graphics;Utility;
```

保存为 `~/.local/share/applications/plugin-shot.desktop`。图标可以先用主题里的通用图标，
后续再补品牌 PNG。

> 不要直接把 `build/Linux` 拷到另一台发行版机器上使用；请在那台机器上重新编译（原因见开头）。

---

## 8. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| configure 报 `gio-2.0` / `pkg-config` 找不到 | 装 `libglib2.0-dev`、`pkg-config`；`pkg-config --modversion gio-2.0` 自测 |
| configure 报 `libportal` / `libpipewire` 找不到 | 装 `libportal-dev`、`libpipewire-0.3-dev`、`libspa-0.2-dev` |
| `cannot find -lXcomposite` | 装 `libxcomposite-dev`（X11 框选背景快照，可选） |
| `GLIBC_2.xx not found` / `dlopen` 失败 | 在目标发行版重新编译；不要跨发行版复制 `.so` |
| `--capture` 在 WSL / 无头环境失败 | 没有真实的 X11/Wayland 会话；`--help` 可用，截图请用桌面会话 |
| Wayland 下 `--capture` 没弹授权框 / 超时 | 确认 `xdg-desktop-portal` 与 `xdg-desktop-portal-gnome` 在运行：`systemctl --user status xdg-desktop-portal`；看日志里的 portal 进度 |
| Wayland 下热键没反应 | 预期行为；用 `session_control` 的 D-Bus + 桌面自定义快捷键 |
| 托盘图标不显示 | 装/启用 `gnome-shell-extension-appindicator`；确认 `org.kde.StatusNotifierWatcher` 存在 |
| 通知不显示 | 装 `libnotify-bin` 用 `notify-send` 自测；确认通知服务存在 |
| 热键注册 `BadAccess` | 组合键被其它程序占用；在设置页换一个组合键 |
| 设置页黑屏 / 无 GL | 装 Mesa；远程会话可试 `LIBGL_ALWAYS_SOFTWARE=1 ./Plugin-shot --settings` |
| 改了头文件后行为诡异 / 链接旧对象 | `cmake --build build/Linux --clean-first` 全量重建（见 AGENTS 的坑） |
| 构建到一半内存不足 | `cmake --build build/Linux -- -j2` 降并行度 |
| vcpkg 下载慢 / 失败 | 配代理；保留 `build/Linux/vcpkg_installed` 与 vcpkg 下载缓存，不要反复删 |

---

## 9. 构建后自检清单

```bash
cd build/Linux/bin

# 1) 宿主能启动（无头也可以）
./Plugin-shot --help; echo "exit=$?"

# 2) 桌面会话里截一张
./Plugin-shot --capture; echo "exit=$?"

# 3) 看日志关键行
grep -E "Host runtime ready|Capture saved|Successfully loaded module" \
  ~/.plugin-shot/logs/host.log | tail -20

# 4) 确认插件都在
ls plugins/*.so | wc -l    # 应为 12（不含仅 Windows 的 long_capture）

# 5) 看二进制依赖（排查缺库）
ldd ./Plugin-shot | grep "not found" || echo "no missing libs"
```

如果 2) 在 Wayland 下弹了授权框、保存成功、剪贴板/通知也正常，基本链路就通了。
区域框选、托盘、热键再按第 6 节分别验证。

---

## 10. 参考

- 根 [README](../README.md) 的「构建」「平台」
- [平台矩阵](PLATFORMS.md)（逐插件、逐平台状态与未验证项）
- [SETTINGS.md](SETTINGS.md)（设置页与插件启停）
- [CAPTURE.md](CAPTURE.md)（采集流水线、区域截图）
- [I18N.md](I18N.md)（语言包与 zh-TW）
