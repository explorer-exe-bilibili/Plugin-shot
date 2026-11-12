# long_capture —— 手动滚动长截图（Windows P1）

用户先从托盘/热键触发，在 `region_selector` 覆盖层里点选目标窗口或拖出区域；插件用
**自带的 GDI 采集层**按目标区域连续抓帧、在线拼接，HUD 实时显示缩略图与「保存」
「取消」按钮；Enter 保存 / Esc 取消；**只有最终保存时发布一条 `capture.done`**。

> **平台**：仅 Windows。GDI 采集、GDI HUD、`RegisterHotKey` / 低级键盘钩子都是
> Win32 实现；`CMakeLists.txt` 在非 Windows 上 `SHOT_PLUGIN_SKIPPED` 并 `return()`，
> 不产出伪实现。核心算法（画布 / 行指纹拼接 / PNG 编码）保持平台无关。

## 1. 触发方式

| 方式 | 说明 |
| --- | --- |
| 托盘菜单 | `long.capture`（label key `capture.long`，event `long_capture.start`，order 5） |
| 全局热键 | 插件在 create 时向 `hotkey_list` 贡献 `start`（默认 `Ctrl+Alt+L`，event `long_capture.start`）；注册/改键/停用由 hotkey 插件统一管理 |

一次只能有一个会话；会话期间再次触发会被忽略。

## 2. 一次会话做什么

1. 发布 `region.select.request`，负载 `{"purpose":"long_capture"}`，等 `region_selector`
   进入选择模式。用户点选窗口（悬停吸附）或拖出区域后，收到 `region.selected`：
   `{"purpose":"long_capture","region":{x,y,width,height}}`（物理虚拟桌面像素）。
   60 秒没选、用户 Esc/右键（`cancelled:true`）或宿主退出都会回到空闲并提示。
2. 把 region 加回虚拟桌面原点，得到绝对屏幕坐标的目标内容区；在内容区外摆 HUD
   （右 → 左 → 下 → 上，与内容区留 12px、完全落在工作区内）；四周都放不下就不建
   窗口，降级为托盘气泡提示。
3. 注册会话按键 Enter / Esc（失败退回 `WH_KEYBOARD_LL`），开始循环：
   - `ScreenGrabber` 用 `GetDC(NULL)` + `BitBlt` 直接抓目标区域，BGRA → RGBA 进内存；
   - **不落盘、不发 `capture.request`、不发 `capture.done`**；
   - 交给 `ScrollStitcher` 做灰度行指纹匹配，正位移追加、负位移前插；
   - 相邻两帧至少间隔 60ms；HUD 以 5Hz 刷新缩略图与状态行（缩略图由自带采集的帧
     拼出的画布生成）。
4. Enter / HUD「保存」：画布 → PNG 落盘 → 发布**一条** `capture.done`；HUD 显示
   「已保存：路径」2 秒后关闭。
5. Esc / HUD「取消」：丢弃画布，不落盘、不发事件。

拼接规则：重叠不足视口高度 30% 或匹配分数 < 0.85 记为缺口（不硬拼）；完全相同的帧
按重复丢弃；画布达到 `max_height` 后停止追加/前插，但保留 HUD 等用户保存或取消。

## 3. 配置

| key | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `max_height` | INT | `30000` | 长图高度上限（像素）；达到后停止拼接，仍需手动保存 |

触发热键不再是本插件的配置项：`hotkey_list` 贡献里的 `combo` 只是默认值，用户改键后
由 hotkey 插件持久化（`hotkey.main/...`），本插件不读写、也不注册长期热键。

## 4. HUD

- 无边框置顶工具窗：`WS_POPUP` + `WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE`；
  点击按钮不抢焦点，目标窗口保持前台，用户可以继续滚动。
- 自绘：标题、缩略图（`StretchBlt` + `HALFTONE`）、状态行/告警行、「保存」「取消」。
- 拖动：背景区域按下后手动搬窗口；每一步都钳制在内容区外且完全落在工作区内。
- 选择结果只有区域、没有窗口句柄，所以 HUD 的滚轮转发自动跳过（`targetHwnd = 0`）；
  HUD 本来就在内容区外，用户滚动时鼠标应在内容上。
- 内容区四周都放不下时不建 HUD，只用托盘气泡提示「滚动内容，Enter 保存 / Esc 取消」。

## 5. 会话按键

- 会话期间 `RegisterHotKey(NULL, id, 0, VK_RETURN / VK_ESCAPE)`；会话结束立即注销。
- 任一注册失败则整体退回 `WH_KEYBOARD_LL`（同样只在会话期间安装，回调只置原子标志）。
- **已知代价**：会话期间 Enter 被全局吃掉，任何程序里的 Enter 都不会再到达它们自己。
  这是「Enter 保存」这个交互的必然代价；想避免只能改用带修饰键的组合。

## 6. 保存位置与事件

- 目录：读 `config_list` 里 `owner = image_saver.main`、`id = directory` 的当前值；
  读不到用系统图片目录（`%USERPROFILE%\\Pictures`）。
- 文件名：`shot_long_YYYYMMDD_HHMMSS.png`；同名追加 `_1`/`_2`…，绝不覆盖。
- 保存成功后发布**唯一一条**：

```json
{"path":"C:\\\\Users\\\\me\\\\Pictures\\\\shot_long_20261112_193000.png",
 "width":1920,"height":8400,"processors":0}
```

## 7. 选择握手（与 region_selector）

| 主题 | 方向 | 负载 |
| --- | --- | --- |
| `region.select.request` | long_capture → region_selector | `{"purpose":"long_capture"}` |
| `region.selected` | region_selector → long_capture | `{"purpose":"long_capture","region":{x,y,width,height}}` |
| `region.selected` | region_selector → long_capture | `{"purpose":"long_capture","cancelled":true}` |

`purpose` 不匹配的 `region.selected` 安静忽略（共享总线上可能有别的消费者）。由
`capture.request` 触发的普通区域截屏完全不受影响，仍然走宿主流水线并发布
`capture.done`。

## 8. 已知问题 / 取舍

1. **Enter 抢占**：见 §5。
2. **非 Windows 未实现**：CMake 直接跳过；`metadata.json` 仍保留，便于将来补平台实现。
3. **HUD 不显示**：内容区四周放不下时降级为托盘气泡；explorer 未运行时气泡也可能失败，
   只能看日志。
4. **高度上限**：达到 `max_height` 后停止拼接，但不会自动保存，仍等用户按 Enter / 点保存。
5. **会话期间目标区域不要移动/缩放**：宽度变化会中止会话；位置变化不会被检测到，
   后续帧仍按开始时的 region 抓取，可能拼进错误内容。
6. **目标区域被别的窗口遮挡时**：GDI BitBlt 抓的是屏幕像素，遮挡物会被一起截进去；
   长截图期间请让目标窗口保持在最前面。
7. **选择超时后覆盖层不会自动关闭**：60 秒超时本插件回到空闲，但 region_selector 的
   覆盖层仍等用户 Esc/右键；用户取消后不会再启动会话。

## 9. 构建

```bash
bash tools/verify/build.sh --target long_capture
```

产物：`build/Windows/bin/plugins/long_capture.dll`。插件只声明 `shot.configurable`，
宿主按 Generic 插件加载；托盘命令、hotkey_list 贡献、HUD、会话按键都从
`create(index, host)` 建立，`destroy` 里停会话、销毁并 `UnregisterClassW`、退订事件
总线、join 全部线程。

## 10. 源码布局

| 文件 | 职责 |
| --- | --- |
| `src/plugin_entry.cpp` | 唯一导出符号、能力目录、配置转发、create/destroy |
| `src/long_capture.cpp` | 生命周期、配置、事件订阅、命令标志、会话调度、选择握手 |
| `src/long_capture_session.cpp` | 一次会话：等选择 → HUD/气泡 → 自带采集 → 拼接 → 保存/取消 |
| `src/long_capture_output.cpp` | 最终保存、config_list 目录解析、文案与托盘气泡 |
| `src/long_capture_i18n.cpp` | 中英文语言表 |
| `src/capture_windows.cpp` | Windows GDI 采集层（BitBlt 目标区域、BGRA→RGBA、DPI 兜底） |
| `src/stitch_canvas.cpp` | 定宽可双端生长的 RGBA 画布 |
| `src/scroll_stitcher.cpp` | 灰度行指纹、带符号位移、去重/缺口判定 |
| `src/long_image_saver.cpp` | stb_image_write 编码 PNG（UTF-8 路径、唯一文件名） |
| `src/long_capture_config.cpp` | `max_height` 配置项 |
| `src/hud_windows.cpp` | HUD 窗口线程、GDI 绘制、按钮/拖动/滚轮 |
| `src/global_keys_windows.cpp` | 会话内 RegisterHotKey、低级键盘钩子 |
| `src/tray_hint_windows.cpp` | 托盘气泡提示 |
