// 本插件的用户可见文案：托盘 label、配置项 label、HUD 文案、选择目标窗口提示。
//
// 配置描述符（long_capture_config.cpp）里写的是这些 key，设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析；hotkey_list 贡献里的
// label 也是 key（由 hotkey 插件解析）。key 拼写必须与使用处严格一致。
//
// 每语言一张 JSON 对象，@meta 必填，两张表的 key 集必须一致；JSON 文本里不能有
// 裸换行，所以用相邻 raw 字面量拼接——拼出来仍是一行。

#include "long_capture_i18n.h"

#include <iterator>

namespace long_capture {
namespace {

const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("capture.long":"长截图",)json"
    R"json("plugin.name":"长截图",)json"
    R"json("plugin.description":"手动滚动长截图：选目标窗口后连续抓帧并拼接，Enter 保存（仅 Windows）",)json"
    R"json("long.config.hotkey.label":"触发快捷键",)json"
    R"json("long.config.algorithm.label":"拼接算法",)json"
    R"json("long.config.algorithm.tooltip":)json"
    R"json("hash=逐行哈希（旧基线，快但易被相似行带偏）；correlate=全宽灰度相关（默认，推荐代码编辑器）；phase=行亮度相位相关（适合长文本页）",)json"
    R"json("algorithm.hash":"逐行哈希（快）",)json"
    R"json("algorithm.correlate":"全宽相关（推荐）",)json"
    R"json("algorithm.phase":"相位相关",)json"
    R"json("long.config.max_height.label":"长图高度上限",)json"
    R"json("long.config.max_height.tooltip":)json"
    R"json("达到上限后停止拼接，仍需手动保存；单位像素。画布内存约 宽×高×4 字节：默认 30000 高、1020 宽约 122MB，调到 120000 约 490MB，请按内存余量设置",)json"
    R"json("long.hud.title":"长截图",)json"
    R"json("long.hud.waiting":"等待画面…",)json"
    R"json("long.hud.status":"帧 {frames} · 高度 {height} px",)json"
    R"json("long.hud.gaps":"缺口 {gaps} 处 · 滚动过快（触控板惯性更明显），可回滚补齐",)json"
    R"json("long.hud.duplicates":"重复帧 {duplicates}",)json"
    R"json("long.hud.limit":"已达高度上限 {height} px，Enter 保存",)json"
    R"json("long.hud.save":"保存",)json"
    R"json("long.hud.cancel":"取消",)json"
    R"json("long.hud.saved":"已保存：{path}",)json"
    R"json("long.hud.failed":"保存失败：{reason}",)json"
    R"json("long.hud.cancelled":"已取消",)json"
    R"json("long.tray.select_timeout":"60 秒内没有选择目标窗口，长截图已取消",)json"
    R"json("long.tray.select_cancelled":"已取消目标选择",)json"
    R"json("long.tray.hint_title":"长截图已开始",)json"
    R"json("long.tray.hint_text":"请滚动窗口内容；Enter 保存 / Esc 取消",)json"
    R"json("long.tray.failed_title":"长截图未能开始",)json"
    R"json("long.tray.error_title":"长截图失败",)json"
    R"json("long.tray.saved_title":"长截图已保存",)json"
    R"json("long.tray.limit_text":"已达到高度上限，Enter 保存 / Esc 取消"})json",
    R"json({"@meta":"en-US",)json"
    R"json("capture.long":"Long capture",)json"
    R"json("plugin.name":"Long capture",)json"
    R"json("plugin.description":"Manual scrolling capture: pick a window, grab and stitch frames live, Enter to save (Windows only)",)json"
    R"json("long.config.hotkey.label":"Trigger hotkey",)json"
    R"json("long.config.algorithm.label":"Stitching algorithm",)json"
    R"json("long.config.algorithm.tooltip":")json"
    R"json(hash = per-row hashing (legacy, fast but fooled by similar rows); )json"
    R"json(correlate = full-width NCC (default, best for code editors); )json"
    R"json(phase = row-brightness phase correlation (best for long text pages)",)json"
    R"json("algorithm.hash":"Row hash (fast)",)json"
    R"json("algorithm.correlate":"Full-width correlation (recommended)",)json"
    R"json("algorithm.phase":"Phase correlation",)json"
    R"json("long.config.max_height.label":"Maximum image height",)json"
    R"json("long.config.max_height.tooltip":)json"
    R"json("Stop stitching at this height (pixels); you still save manually. Canvas memory is about width*height*4 bytes: 30000x1020 is ~122MB, 120000x1020 is ~490MB",)json"
    R"json("long.hud.title":"Long capture",)json"
    R"json("long.hud.waiting":"Waiting for frames…",)json"
    R"json("long.hud.status":"Frames {frames} · {height} px",)json"
    R"json("long.hud.gaps":"{gaps} gap(s) · scrolled too fast (trackpad inertia makes it worse); scroll back to fill in",)json"
    R"json("long.hud.duplicates":"Duplicates {duplicates}",)json"
    R"json("long.hud.limit":"Height limit reached ({height} px); Enter to save",)json"
    R"json("long.hud.save":"Save",)json"
    R"json("long.hud.cancel":"Cancel",)json"
    R"json("long.hud.saved":"Saved: {path}",)json"
    R"json("long.hud.failed":"Save failed: {reason}",)json"
    R"json("long.hud.cancelled":"Cancelled",)json"
    R"json("long.tray.select_timeout":"No target selected in 60 seconds; long capture cancelled",)json"
    R"json("long.tray.select_cancelled":"Target selection cancelled",)json"
    R"json("long.tray.hint_title":"Long capture started",)json"
    R"json("long.tray.hint_text":"Scroll the window; Enter to save, Esc to cancel",)json"
    R"json("long.tray.failed_title":"Long capture could not start",)json"
    R"json("long.tray.error_title":"Long capture failed",)json"
    R"json("long.tray.saved_title":"Long capture saved",)json"
    R"json("long.tray.limit_text":"Height limit reached; Enter to save, Esc to cancel"})json",
};

}  // namespace

const char* const* languageTables(std::size_t& count) noexcept {
    count = std::size(kLangTables);
    return kLangTables;
}

}  // namespace long_capture
