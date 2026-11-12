# long_capture —— 手动滚动长截图（Windows P1）

用户自己滚动目标窗口，插件按固定间隔连续抓帧、在线拼接，HUD 实时显示缩略图与
「保存」「取消」按钮；Enter 保存 / Esc 取消；保存后发布 `capture.done`。

> **平台**：仅 Windows。前台窗口客户区、GDI HUD、`RegisterHotKey` / 低级键盘钩子都是
> Win32 实现；`CMakeLists.txt` 在非 Windows 上 `SHOT_PLUGIN_SKIPPED` 并 `return()`，
> 不产出伪实现。核心算法（画布 / 行指纹拼接 / PNG 编解码）保持平台无关。

## 1. 触发方式

| 方式 | 说明 |
| --- | --- |
| 托盘菜单 | `long.capture`（label key `capture.long`，event `long_capture.start`，order 5） |
| 配置热键 | `hotkey`，默认 `Ctrl+Alt+L`；插件自己 `RegisterHotKey`（`MOD_NOREPEAT`），可留空 |

一次只能有一个会话；会话期间再次触发会被忽略。

## 2. 一次会话做什么

1. 取前台窗口客户区（物理像素）。前台是本进程窗口（托盘隐藏窗 / 设置页 / 上一次的
   HUD）时，沿真实 Z 序找下一个非本进程、可见、未最小化、客户区非空的窗口；
   最小化 / 不可见 / 客户区为空会通过托盘气泡明确提示。
2. 在内容区外摆 HUD（右 → 左 → 下 → 上，与内容区留 12px、完全落在工作区内）；
   四个方向都放不下就不建窗口，降级为托盘气泡提示。
3. 注册会话按键 Enter / Esc（失败退回 `WH_KEYBOARD_LL`），开始循环：
   - 发布 `capture.request`（`{"mode":"region","region":{x,y,width,height}}`，
     坐标已减去虚拟桌面原点）；
   - 等 `capture.done`，用 `FrameLoader`（stb_image）读 PNG/JPEG；
   - **读完立即删除这个临时帧文件**；
   - 交给 `ScrollStitcher` 做灰度行指纹匹配，正位移追加、负位移前插；
   - 相邻两帧至少间隔 60ms；HUD 以 5Hz 刷新缩略图与状态行。
4. Enter / HUD「保存」：画布 → PNG 落盘 → 发布 `capture.done`；HUD 显示
   「已保存：路径」2 秒后关闭。
5. Esc / HUD「取消」：丢弃画布，不落盘。

拼接规则：重叠不足视口高度 30% 或匹配分数 < 0.85 记为缺口（不硬拼）；完全相同的帧
按重复丢弃；画布达到 `max_height` 后停止追加/前插，但保留 HUD 等用户保存或取消。

## 3. 配置

| key | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `hotkey` | STRING（`options="hotkey"`） | `Ctrl+Alt+L` | 触发组合键；空串 = 只用托盘菜单 |
| `max_height` | INT | `30000` | 长图高度上限（像素）；达到后停止拼接，仍需手动保存 |

## 4. HUD

- 无边框置顶工具窗：`WS_POPUP` + `WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE`；
  点击按钮不抢焦点，目标窗口保持前台，用户可以继续滚动。
- 自绘：标题、缩略图（`StretchBlt` + `HALFTONE`）、状态行/告警行、「保存」「取消」。
- 拖动：背景区域按下后手动搬窗口；每一步都钳制在内容区外且完全落在工作区内。
- 鼠标滚轮在 HUD 上时转发给目标窗口（坐标改成目标客户区中心，
  滚轮总是作用在内容上）。
- 内容区四周都放不下时不建 HUD，只用托盘气泡提示「滚动内容，Enter 保存 / Esc 取消」。

## 5. 全局按键

- 会话期间 `RegisterHotKey(NULL, id, 0, VK_RETURN / VK_ESCAPE)`；会话结束立即注销。
- 任一注册失败则整体退回 `WH_KEYBOARD_LL`（同样只在会话期间安装，回调只置原子标志）。
- **已知代价**：会话期间 Enter 被全局吃掉，任何程序里的 Enter 都不会再到达它们自己。
  这是「Enter 保存」这个交互的必然代价；想避免只能改用带修饰键的组合。

## 6. 保存位置与事件

- 目录：读 `config_list` 里 `owner = image_saver.main`、`id = directory` 的当前值；
  读不到用系统图片目录（`%USERPROFILE%\Pictures`）。
- 文件名：`shot_long_YYYYMMDD_HHMMSS.png`；同名追加 `_1`/`_2`…，绝不覆盖。
- 保存成功后发布：

```json
{"path":"C:\\Users\\me\\Pictures\\shot_long_20261112_193000.png",
 "width":1920,"height":8400,"processors":0}
```

## 7. 已知问题 / 取舍

1. **每帧临时文件**：P1 走宿主采集流水线，每帧都会在 `image_saver` 目录里落一个文件；
   插件读完立即删除，避免污染输出目录。删除失败（被其它消费者短暂占用）只记 DEBUG。
   更彻底的做法是插件自带采集层（直接调 `screen_capture`），不在本期范围。
2. **每帧 `capture.done` 会被其它订阅者看到**：`notify` / `clipboard` 等插件订阅的是
   同一个主题，会话期间可能对每一帧都弹提示/写剪贴板。
   这是复用宿主流水线的副作用；
   后续如果做插件自带采集层，应改用私有事件或直接读像素来消除。
3. **Enter 抢占**：见 §5。
4. **非 Windows 未实现**：CMake 直接跳过；`metadata.json` 仍保留，便于将来补平台实现。
5. **HUD 不显示**：内容区四周放不下时降级为托盘气泡；explorer 未运行时气泡也可能失败，
   只能看日志。
6. **高度上限**：达到 `max_height` 后停止拼接，但不会自动保存，仍等用户按 Enter / 点保存。
7. **会话期间目标窗口不要移动/缩放**：宽度变化会中止会话；位置变化不会被检测到，
   后续帧仍按开始时的 region 抓取，可能拼进错误内容。

## 8. 构建

```bash
bash tools/verify/build.sh --target long_capture
```

产物：`build/Windows/bin/plugins/long_capture.dll`。插件只声明 `shot.configurable`，
宿主按 Generic 插件加载；托盘命令、HUD、全局按键都从 `create(index, host)` 建立，
`destroy` 里停会话、注销热键、销毁并 `UnregisterClassW`、退订事件总线、join 全部线程。

## 9. 源码布局

| 文件 | 职责 |
| --- | --- |
| `src/plugin_entry.cpp` | 唯一导出符号、能力目录、配置转发、create/destroy |
| `src/long_capture.cpp` | 生命周期、配置、事件订阅、命令标志、会话调度、保存 |
| `src/long_capture_session.cpp` | 一次会话：找窗口 → HUD/气泡 → 抓帧 → 拼接 → 保存/取消 |
| `src/stitch_canvas.cpp` | 定宽可双端生长的 RGBA 画布 |
| `src/scroll_stitcher.cpp` | 灰度行指纹、带符号位移、去重/缺口判定 |
| `src/frame_loader.cpp` | stb_image 解码（UTF-8 路径） |
| `src/long_image_saver.cpp` | stb_image_write 编码 PNG（UTF-8 路径、唯一文件名） |
| `src/long_capture_config.cpp` | 两个配置项 |
| `src/window_target_windows.cpp` | 前台窗口 / 客户区 / 虚拟桌面原点 |
| `src/hud_windows.cpp` | HUD 窗口线程、GDI 绘制、按钮/拖动/滚轮 |
| `src/global_keys_windows.cpp` | 组合键解析、RegisterHotKey、低级键盘钩子 |
| `src/tray_hint_windows.cpp` | 托盘气泡提示 |
