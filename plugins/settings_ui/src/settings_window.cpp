// settings_window —— SettingsWindow 的生命周期与窗口线程：建/停窗口、GLFW 事件循环、
// 事件队列、显示/隐藏。窗口与 ImGui 的渲染在 settings_graphics.cpp，窗口几何在
// settings_geometry.cpp，ABI 胶水在 plugin_entry.cpp，文件对话框在 settings_dialog.cpp。
//
// 线程模型（对应 document/EVENT_BUS.md）：
//   · 宿主只调 init / deinit / post_to_plugin，从不接触窗口句柄；
//   · 事件总线回调只做"拷进队列 + glfwPostEmptyEvent 唤醒"，所有 GLFW / ImGui 调用
//     都在这一个 UI 线程上执行（GLFW 和 ImGui 都不是线程安全的）；
//   · X / Esc 只隐藏窗口，线程与 GL 上下文保留，下次 settings.open 直接显示；
//     只有 deinit（或宿主 shutdown）才退出事件循环、销毁窗口，因此宿主热卸载时
//     能 join 成功。
//
// TODO: POSIX（尤其 macOS）未验证。macOS 上 GLFW 要求窗口与事件循环在进程主线程，
// 而本插件的 UI 线程是自建线程；这里按"GLFW 可在任意线程 init/建窗"的 Windows/Linux
// 行为实现，等有 macOS 环境再决定是否把 UI 线程改为进程主线程驱动。

#include "settings_window.h"

#include <iterator>
#include <string>
#include <utility>

#include <GLFW/glfw3.h>  // 跨平台窗口库（不是平台头）：只用于窗口生命周期与事件循环

#include "imgui.h"
#include "plugin_entry.h"
#include "settings_dialog.h"
#include "settings_window_impl.h"
#include "shot_plugin_helpers.h"

namespace settings_ui {
namespace {

// 设置页自己的用户可见文案（日志不在这里：日志一律中文，document/I18N.md）。
// 每语言一张 JSON 表、@meta 必填；key 与 tr("settings_ui.xxx", ...) 的引用一一对应。
// 长行按 88 字符切成相邻 raw 字面量，编译期拼回一条合法 JSON。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"设置页",)json"
    R"json("plugin.description":"设置页：按配置描述符生成表单，并管理插件的启用与禁用（ImGui + GLFW + OpenGL3）",)json"
    R"json("settings_ui.window_title":"设置",)json"
    R"json("settings_ui.apply":"应用",)json"
    R"json("settings_ui.cancel":"取消",)json"
    R"json("settings_ui.restore_all_defaults":"恢复全部默认",)json"
    R"json("settings_ui.pending_changes":"有未应用的修改",)json"
    R"json("settings_ui.no_pending_changes":"没有未应用的修改",)json"
    R"json("settings_ui.no_config_items":"宿主与插件都没有声明配置项。",)json"
    R"json("settings_ui.section_removed":"插件已卸载：本节只剩最后一次读到的值，不能编辑。",)json"
    R"json("settings_ui.section_empty":"该提供者没有声明任何配置项。",)json"
    R"json("settings_ui.column_item":"配置项",)json"
    R"json("settings_ui.column_value":"值",)json"
    R"json("settings_ui.restore_default":"恢复默认",)json"
    R"json("settings_ui.key_prefix":"键：",)json"
    R"json("settings_ui.write_failed":"写入失败：%s",)json"
    R"json("settings_ui.modified":"已修改",)json"
    R"json("settings_ui.enum_no_options":"（枚举没有候选值）",)json"
    R"json("settings_ui.browse":"浏览…",)json"
    R"json("settings_ui.unsupported_kind":"不支持的配置类型（kind=%u）",)json"
    R"json("settings_ui.name_with_id":"%s（%s）",)json"
    R"json("settings_ui.config_no_host":"宿主服务表为空：当前读不到配置目录。",)json"
    R"json("settings_ui.config_no_list_service":"宿主不支持列表服务（shot.host.plugin_lists）：当前宿主不能读插件配置，设置页只)json"
    R"json(剩插件管理。",)json"
    R"json("settings_ui.config_no_list":"宿主暂未提供 config_list：当前没有可读的配置项。",)json"
    R"json("settings_ui.config_bad_json":"config_list 不是合法的 JSON 数组：配置区暂时不可用（详见宿主日志）。",)json"
    R"json("settings_ui.host_unavailable_apply":"宿主服务不可用，无法应用",)json"
    R"json("settings_ui.nothing_to_apply":"没有需要应用的修改",)json"
    R"json("settings_ui.publish_update_failed":"发布 lists.update 失败：宿主事件总线不可用或队列已满",)json"
    R"json("settings_ui.publish_commit_failed":"发布 lists.commit 失败：配置值已写入，但可能没有落盘",)json"
    R"json("settings_ui.plugin_toggle_rejected":"宿主拒绝了这次启停请求",)json"
    R"json("settings_ui.busy_writing":"应用中…（写入配置）",)json"
    R"json("settings_ui.busy_saving":"应用中…（保存配置）",)json"
    R"json("settings_ui.busy_plugins":"应用中…（等待宿主完成插件启停）",)json"
    R"json("settings_ui.busy":"应用中…",)json"
    R"json("settings_ui.apply_rejected":"应用完成，但有 %d 项配置和 %d 个插件启停被拒绝",)json"
    R"json("settings_ui.apply_unconfirmed":"有 %d 项配置没有被宿主确认",)json"
    R"json("settings_ui.save_failed":"配置保存失败（值可能已生效，但没有落盘）",)json"
    R"json("settings_ui.applied":"已应用",)json"
    R"json("settings_ui.save_failed_with_error":"保存配置失败：%s",)json"
    R"json("settings_ui.write_config_failed":"写入配置失败：%s",)json"
    R"json("settings_ui.value_rejected":"该值不被接受",)json"
    R"json("settings_ui.plugin_error":"插件 %s：%s",)json"
    R"json("settings_ui.plugin_change_rejected":"有插件启停被拒绝，原因见插件管理表",)json"
    R"json("settings_ui.plugin_list_updated":"插件列表已更新",)json"
    R"json("settings_ui.timeout_update":"等待 lists.updated 超时：宿主没有确认配置写入（修改仍保留，可重试）",)json"
    R"json("settings_ui.timeout_applied":"等待 config.applied 超时：配置可能已生效，但落盘结果未知",)json"
    R"json("settings_ui.timeout_plugins":"等待插件启停超时：宿主没有发来 plugins.changed",)json"
    R"json("settings_ui.discarded":"已丢弃未应用的修改",)json"
    R"json("settings_ui.defaults_restored":"已恢复默认值，点「应用」后写回",)json"
    R"json("settings_ui.no_defaults":"没有可恢复默认值的配置项",)json"
    R"json("settings_ui.hotkey_unset":"（未设置）",)json"
    R"json("settings_ui.hotkey_record":"录入…",)json"
    R"json("settings_ui.hotkey_waiting":"等待按键…",)json"
    R"json("settings_ui.hotkey_press_new":"按下新的组合键",)json"
    R"json("settings_ui.hotkey_hint":"Ctrl / Alt / Shift / Win + 字母、数字、F1-F12 或 PrintScreen",)json"
    R"json("settings_ui.hotkey_cancel_hint":"Esc 取消；Backspace / Delete 清空（该动作不再有热键）",)json"
    R"json("settings_ui.hotkey_current":"当前组合键：%s",)json"
    R"json("settings_ui.hotkey_bare_hint":"提示：没有修饰键的热键会拦截正常输入",)json"
    R"json("settings_ui.hotkey_unsupported":"不支持的按键：只支持 A-Z / 0-9 / F1-F12 / PrintScreen（可加 Ctrl/Al)json"
    R"json(t/Shift/Win）",)json"
    R"json("settings_ui.color_custom":"自定义…",)json"
    R"json("settings_ui.color_pick_title":"自定义颜色",)json"
    R"json("settings_ui.color_custom_done":"确定",)json"
    R"json("settings_ui.color_hex_hint":"#RRGGBB，例如 #4C9AFF",)json"
    R"json("settings_ui.tab_general":"常规",)json"
    R"json("settings_ui.tab_plugins":"插件",)json"
    R"json("settings_ui.plugin_table_title":"插件管理（勾选后要点「应用」才生效）",)json"
    R"json("settings_ui.registry_unavailable":"宿主未提供插件注册表服务（shot.host.plugin_registry）：当前宿主不能管理插件启停)json"
    R"json(。",)json"
    R"json("settings_ui.no_plugins":"没有已安装的插件。",)json"
    R"json("settings_ui.column_enabled":"启用",)json"
    R"json("settings_ui.column_plugin":"插件",)json"
    R"json("settings_ui.column_version":"版本",)json"
    R"json("settings_ui.column_status":"状态",)json"
    R"json("settings_ui.entry_disabled_tooltip":"入口插件不能禁用自己（禁用会连设置页一起卸载）",)json"
    R"json("settings_ui.entry_marker":"（入口）",)json"
    R"json("settings_ui.pending_marker":"（待应用）",)json"
    R"json("settings_ui.state_unknown":"未知状态",)json"
    R"json("settings_ui.state_loaded":"已加载",)json"
    R"json("settings_ui.state_disabled":"已禁用",)json"
    R"json("settings_ui.state_failed":"加载失败",)json"
    R"json("settings_ui.dialog_all_files":"所有文件 (*.*)",)json"
    R"json("settings_ui.dialog_unavailable":"文件对话框不可用",)json"
    R"json("settings_ui.dialog_error_prefix":"：",)json"
    R"json("settings_ui.dialog_error_suffix":"，请直接输入路径"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"Settings",)json"
    R"json("plugin.description":"Settings window: builds the form from config descriptors and enables or disables plugins (ImGui + GLFW + OpenGL3)",)json"
    R"json("settings_ui.window_title":"Settings",)json"
    R"json("settings_ui.apply":"Apply",)json"
    R"json("settings_ui.cancel":"Cancel",)json"
    R"json("settings_ui.restore_all_defaults":"Restore all defaults",)json"
    R"json("settings_ui.pending_changes":"Unapplied changes",)json"
    R"json("settings_ui.no_pending_changes":"No unapplied changes",)json"
    R"json("settings_ui.no_config_items":"The host and plugins declare no config items.",)json"
    R"json("settings_ui.section_removed":"Plugin unloaded: these are the last known values and cann)json"
    R"json(ot be edited.",)json"
    R"json("settings_ui.section_empty":"This provider declares no config items.",)json"
    R"json("settings_ui.column_item":"Setting",)json"
    R"json("settings_ui.column_value":"Value",)json"
    R"json("settings_ui.restore_default":"Reset",)json"
    R"json("settings_ui.key_prefix":"Key: ",)json"
    R"json("settings_ui.write_failed":"Write failed: %s",)json"
    R"json("settings_ui.modified":"Modified",)json"
    R"json("settings_ui.enum_no_options":"(no enum choices)",)json"
    R"json("settings_ui.browse":"Browse…",)json"
    R"json("settings_ui.unsupported_kind":"Unsupported config kind (kind=%u)",)json"
    R"json("settings_ui.name_with_id":"%s (%s)",)json"
    R"json("settings_ui.config_no_host":"Host services are unavailable: cannot read the config cata)json"
    R"json(log.",)json"
    R"json("settings_ui.config_no_list_service":"The host has no list service (shot.host.plugin_lis)json"
    R"json(ts): plugin settings are unavailable; only plugin management remains.",)json"
    R"json("settings_ui.config_no_list":"The host has not provided config_list yet: no settings to )json"
    R"json(show.",)json"
    R"json("settings_ui.config_bad_json":"config_list is not a valid JSON array: settings are unava)json"
    R"json(ilable for now (see the host log).",)json"
    R"json("settings_ui.host_unavailable_apply":"Host services unavailable, cannot apply",)json"
    R"json("settings_ui.nothing_to_apply":"Nothing to apply",)json"
    R"json("settings_ui.publish_update_failed":"Failed to publish lists.update: event bus unavailab)json"
    R"json(le or queue full",)json"
    R"json("settings_ui.publish_commit_failed":"Failed to publish lists.commit: values were written)json"
    R"json( but may not be persisted",)json"
    R"json("settings_ui.plugin_toggle_rejected":"The host rejected this enable/disable request",)json"
    R"json("settings_ui.busy_writing":"Applying… (writing config)",)json"
    R"json("settings_ui.busy_saving":"Applying… (saving config)",)json"
    R"json("settings_ui.busy_plugins":"Applying… (waiting for plugin changes)",)json"
    R"json("settings_ui.busy":"Applying…",)json"
    R"json("settings_ui.apply_rejected":"Applied, but %d settings and %d plugin changes were reject)json"
    R"json(ed",)json"
    R"json("settings_ui.apply_unconfirmed":"%d settings were not confirmed by the host",)json"
    R"json("settings_ui.save_failed":"Saving failed (values may be applied but not persisted)",)json"
    R"json("settings_ui.applied":"Applied",)json"
    R"json("settings_ui.save_failed_with_error":"Failed to save config: %s",)json"
    R"json("settings_ui.write_config_failed":"Failed to write config: %s",)json"
    R"json("settings_ui.value_rejected":"This value was rejected",)json"
    R"json("settings_ui.plugin_error":"Plugin %s: %s",)json"
    R"json("settings_ui.plugin_change_rejected":"Some plugin changes were rejected; see the plugin )json"
    R"json(table",)json"
    R"json("settings_ui.plugin_list_updated":"Plugin list updated",)json"
    R"json("settings_ui.timeout_update":"Timed out waiting for lists.updated: the host did not conf)json"
    R"json(irm the write (changes kept, retry possible)",)json"
    R"json("settings_ui.timeout_applied":"Timed out waiting for config.applied: settings may be app)json"
    R"json(lied but persistence is unknown",)json"
    R"json("settings_ui.timeout_plugins":"Timed out waiting for plugin changes: the host sent no pl)json"
    R"json(ugins.changed",)json"
    R"json("settings_ui.discarded":"Discarded unapplied changes",)json"
    R"json("settings_ui.defaults_restored":"Defaults restored; click Apply to write them back",)json"
    R"json("settings_ui.no_defaults":"No settings have defaults to restore",)json"
    R"json("settings_ui.hotkey_unset":"(not set)",)json"
    R"json("settings_ui.hotkey_record":"Record…",)json"
    R"json("settings_ui.hotkey_waiting":"Waiting for a key…",)json"
    R"json("settings_ui.hotkey_press_new":"Press the new shortcut",)json"
    R"json("settings_ui.hotkey_hint":"Ctrl / Alt / Shift / Win + letter, digit, F1-F12 or PrintScre)json"
    R"json(en",)json"
    R"json("settings_ui.hotkey_cancel_hint":"Esc cancels; Backspace / Delete clears it (no shortcut)json"
    R"json( for this action)",)json"
    R"json("settings_ui.hotkey_current":"Current shortcut: %s",)json"
    R"json("settings_ui.hotkey_bare_hint":"Note: a shortcut without modifiers will swallow normal i)json"
    R"json(nput",)json"
    R"json("settings_ui.hotkey_unsupported":"Unsupported key: only A-Z / 0-9 / F1-F12 / PrintScreen)json"
    R"json( (optionally with Ctrl/Alt/Shift/Win)",)json"
    R"json("settings_ui.color_custom":"Custom…",)json"
    R"json("settings_ui.color_pick_title":"Custom color",)json"
    R"json("settings_ui.color_custom_done":"OK",)json"
    R"json("settings_ui.color_hex_hint":"#RRGGBB, e.g. #4C9AFF",)json"
    R"json("settings_ui.tab_general":"General",)json"
    R"json("settings_ui.tab_plugins":"Plugins",)json"
    R"json("settings_ui.plugin_table_title":"Plugins (changes apply after clicking Apply)",)json"
    R"json("settings_ui.registry_unavailable":"The host has no plugin registry service (shot.host.p)json"
    R"json(lugin_registry): plugins cannot be managed.",)json"
    R"json("settings_ui.no_plugins":"No plugins installed.",)json"
    R"json("settings_ui.column_enabled":"Enabled",)json"
    R"json("settings_ui.column_plugin":"Plugin",)json"
    R"json("settings_ui.column_version":"Version",)json"
    R"json("settings_ui.column_status":"Status",)json"
    R"json("settings_ui.entry_disabled_tooltip":"The entry plugin cannot disable itself (that would)json"
    R"json( unload this window)",)json"
    R"json("settings_ui.entry_marker":"(entry)",)json"
    R"json("settings_ui.pending_marker":"(pending)",)json"
    R"json("settings_ui.state_unknown":"Unknown",)json"
    R"json("settings_ui.state_loaded":"Loaded",)json"
    R"json("settings_ui.state_disabled":"Disabled",)json"
    R"json("settings_ui.state_failed":"Load failed",)json"
    R"json("settings_ui.dialog_all_files":"All files (*.*)",)json"
    R"json("settings_ui.dialog_unavailable":"File dialog unavailable",)json"
    R"json("settings_ui.dialog_error_prefix":": ",)json"
    R"json("settings_ui.dialog_error_suffix":"; please type the path directly"})json"
};

constexpr char kTopicSettingsOpen[] = "settings.open";
constexpr char kTopicConfigApplied[] = "config.applied";
constexpr char kTopicPluginsChanged[] = "plugins.changed";
constexpr char kTopicListsChanged[] = "lists.changed";
constexpr char kTopicListsUpdated[] = "lists.updated";
constexpr char kEventHostShutdown[] = "host.shutdown";

// GLFW 的错误回调没有 user 参数，只能用一个文件内全局记录"当前窗口实例的宿主服务"。
// 单窗口单实例所以够用；由 UI 线程在 glfwInit 之前设置、glfwTerminate 之后清空。
// 这两个函数与任何类无关、只在本 TU 用，所以放匿名命名空间（STYLE.md §6.7）。
std::mutex g_glfwErrorMutex;
const ShotHostServices* g_glfwErrorHost = nullptr;

/**
 * @brief 记录 GLFW 错误回调要用宿主服务表（只在 glfwInit 前/glfwTerminate 后调）。
 * @param[in] host 宿主服务表，可为 nullptr（= 不记日志）。
 */
void setGlfwErrorHost(const ShotHostServices* host) {
    std::lock_guard<std::mutex> lock(g_glfwErrorMutex);
    g_glfwErrorHost = host;
}

/** @brief 清掉 GLFW 错误回调的日志目标（glfwTerminate 之后调）。 */
void clearGlfwErrorHost() {
    std::lock_guard<std::mutex> lock(g_glfwErrorMutex);
    g_glfwErrorHost = nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// UI 线程主体
// ---------------------------------------------------------------------------
void SettingsWindow::Impl::run() {
    setGlfwErrorHost(host);

    // 三步初始化：GLFW 运行时 -> 隐藏窗口 + GL 上下文 -> ImGui 后端。
    // 任一步失败都走"收掉半成品 -> 报告失败"的路径，不留下半个窗口线程。
    if (!initGlfw() || !createWindow() || !initImGui()) {
        shutdownGraphics();
        shutdownGlfw();
        {
            std::lock_guard<std::mutex> lock(readyMutex);
            failed = true;
        }
        readyCv.notify_all();
        return;
    }

    // 窗口已就绪：先只做握手，模型不在这里读。init 发生时宿主的配置目录/注册表服务
    // 可能还没完全装配好（本函数是在宿主加载插件的过程中被调的），第一次真正读模型
    // 放在 settings.open（窗口显示）或收到事件时，那之前也没有人看得到界面。
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
    }
    readyCv.notify_all();
    shot::Logger(host).info("[settings_ui] 设置窗口线程就绪（窗口隐藏，等 settings.open）");

    messageLoop();

    // 宿主退出 / 热卸载会让事件循环直接结束：录入会话可能还开着，补一次 resume
    // （hotkey 插件那时可能已经卸载，发布到总线是安静的空操作；但漏发会让它的
    // 全局热键一直不注册）。
    panel.cancelHotkeyCapture();

    // 事件循环已退出（stop 或 host.shutdown）。宿主退出时窗口可能还开着，顺手把几何存下来，
    // 下次打开才能恢复到同样的位置（正常关窗时 hideWindow 已经存过一次）。
    if (visible) {
        saveGeometry();
        visible = false;
    }
    // 顺序有讲究：先关 ImGui，再销毁窗口/终止 GLFW。
    shutdownGraphics();
    shutdownGlfw();
}

bool SettingsWindow::Impl::initGlfw() {
    glfwSetErrorCallback(&Impl::onGlfwError);
    if (glfwInit() != GLFW_TRUE) {
        shot::Logger(host).error("[settings_ui] GLFW 初始化失败");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        glfwInitialized = true;
    }
    return true;
}

bool SettingsWindow::Impl::createWindow() {
    // 窗口先隐藏：等宿主的 settings.open 再显示（契约允许宿主在 init 之后随时投递事件）。
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    // macOS 只提供 3.2+ core profile，且必须是 forward-compatible 的上下文。
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    // 标题是窗口管理器/任务栏可见文案：按当前语言生成；GLFW 会拷贝字符串。
    const std::string title =
        std::string("Plugin-shot ") + tr("settings_ui.window_title", "设置");
    window = glfwCreateWindow(kDefaultWindowWidth, kDefaultWindowHeight, title.c_str(), nullptr,
                              nullptr);
    if (window == nullptr) {
        shot::Logger(host).error("[settings_ui] 创建 GLFW 窗口/OpenGL 上下文失败");
        return false;
    }
    glfwSetWindowSizeLimits(window, kMinimumWindowWidth, kMinimumWindowHeight, GLFW_DONT_CARE,
                            GLFW_DONT_CARE);
    // 键盘/关闭回调：GLFW 的窗口用户指针用来在回调里找回 Impl（static 回调没有 this）。
    glfwSetWindowUserPointer(window, this);
    glfwSetKeyCallback(window, &Impl::onKey);
    // 跨屏拖动 / 程序的 setPos 让窗口换到另一块 DPI 不同的显示器时，GLFW 会回调这里；
    // 回调只置脏标记，真正重建 ImGui 样式在下一帧（见 settings_graphics.cpp）。
    glfwSetWindowContentScaleCallback(window, &Impl::onContentScale);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // 有垂直同步就开；没有也只是多画几帧，不影响功能
    return true;
}

void SettingsWindow::Impl::shutdownGlfw() {
    // 对话框库是懒初始化的：用过才需要收尾（没用过时是空操作）。
    shutdownFileDialogs();
    if (window != nullptr) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        glfwInitialized = false;
    }
    glfwTerminate();  // 未初始化时调用是安全的空操作（GLFW 文档保证）
    clearGlfwErrorHost();
}

void SettingsWindow::Impl::messageLoop() {
    // 窗口就绪前入队的事件（例如宿主在 init 返回后立刻发的 settings.open）在这里补处理。
    drainEvents();
    while (!shouldQuit()) {
        if (visible) {
            // 可见时按约 60Hz 重绘：有事件立即返回（交互跟手），没事件最多等 16ms。
            glfwWaitEventsTimeout(0.016);
        } else {
            // 隐藏时不渲染：阻塞等事件，空闲期不占 CPU。宿主投递用 glfwPostEmptyEvent 唤醒。
            glfwWaitEvents();
        }
        drainEvents();
        if (shouldQuit()) {
            break;
        }
        if (window != nullptr && glfwWindowShouldClose(window) != GLFW_FALSE) {
            // 点 X 只是隐藏：把 GLFW 的"要关闭"标记吃掉，窗口与线程都保留。
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            hideWindow();
        }
        if (escHideRequested) {
            escHideRequested = false;
            hideWindow();
        }
        if (visible) {
            renderFrame();
        }
    }
}

void SettingsWindow::Impl::drainEvents() {
    std::queue<QueuedEvent> local;
    {
        std::lock_guard<std::mutex> lock(mutex);
        local.swap(queue);
    }
    while (!local.empty()) {
        const QueuedEvent& event = local.front();
        if (event.name == kEventHostShutdown) {
            // 宿主热卸载/退出：让事件循环退出。stop() 随后 join，窗口在 run() 收尾销毁。
            shot::Logger(host).info("settings_ui：收到 host.shutdown，窗口线程退出");
            requestQuit();
        } else if (event.name == kTopicSettingsOpen) {
            showWindow();
        } else if (event.name == kTopicConfigApplied) {
            panel.onConfigApplied();
        } else if (event.name == kTopicPluginsChanged) {
            panel.onPluginsChanged(event.payload);
        } else if (event.name == kTopicListsChanged) {
            panel.onListsChanged(event.payload);
            // i18n_list 变化 = 语言可能变了：标题也要跟着刷新（窗口可能正开着）。
            updateWindowTitle();
        } else if (event.name == kTopicListsUpdated) {
            panel.onListsUpdated(event.payload);
        }
        // 其它主题安静忽略：事件总线是共享的，收到不认识的主题不是错误。
        local.pop();
    }
}

void SettingsWindow::Impl::enqueue(const char* name, const char* data, std::uint32_t len) {
    if (name == nullptr) {
        return;
    }
    std::string payload;
    if (data != nullptr && len > 0) {
        payload.assign(data, len);  // 契约要求：返回前把 data_len 字节拷走
    }
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!acceptingEvents) {
            return;
        }
        queue.push(QueuedEvent{name, std::move(payload)});
        wake = glfwInitialized;
    }
    if (wake) {
        // 唤醒阻塞在 glfwWaitEvents 里的 UI 线程；唤醒失败也不丢数据——事件循环下一轮
        // 会再 drain 一次。
        glfwPostEmptyEvent();
    }
}

bool SettingsWindow::Impl::shouldQuit() {
    std::lock_guard<std::mutex> lock(mutex);
    return quitRequested;
}

void SettingsWindow::Impl::requestQuit() {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        quitRequested = true;
        wake = glfwInitialized;
    }
    if (wake) {
        glfwPostEmptyEvent();
    }
}

void SettingsWindow::Impl::updateWindowTitle() {
    if (window == nullptr) {
        return;
    }
    const std::string title =
        std::string("Plugin-shot ") + tr("settings_ui.window_title", "设置");
    glfwSetWindowTitle(window, title.c_str());
}

void SettingsWindow::Impl::showWindow() {
    if (window == nullptr || !imguiInitialized) {
        return;
    }
    if (!visible) {
        // 创建窗口时 i18n_list 可能还没生成：显示前按当前语言重算一次标题。
        updateWindowTitle();
        restoreGeometry();
        // 恢复几何可能把窗口搬到另一块显示器（150% → 100%）：按目标显示器的
        // content scale 重建 ImGui 样式/字号，再显示，第一帧就是对的缩放。
        refreshDpiScale();
        glfwShowWindow(window);
        glfwFocusWindow(window);
        visible = true;
        shot::Logger(host).info("settings_ui：设置窗口已显示");
    }
    // 每次打开都重读一遍：窗口隐藏期间配置可能被别的路径改过（config.applied / 热加载）。
    panel.refresh(host);
}

void SettingsWindow::Impl::hideWindow() {
    if (window == nullptr) {
        return;
    }
    // 录入中关窗：必须先结束录入补发 hotkey.resume，否则 hotkey 插件会一直停在
    // 暂停状态（全局热键在 resume 之前都不会注册）。
    panel.cancelHotkeyCapture();
    // 关闭（X / Esc）只隐藏：线程、窗口、GL 上下文都保留，下次 settings.open 直接显示。
    saveGeometry();
    glfwHideWindow(window);
    visible = false;
    shot::Logger(host).info("settings_ui：设置窗口已隐藏（几何已存 host config）");
}

void SettingsWindow::Impl::onBusEvent(void* user, const char* topic, const void* data,
                                      std::uint32_t len) {
    auto* self = static_cast<Impl*>(user);
    if (self == nullptr) {
        return;
    }
    self->enqueue(topic, static_cast<const char*>(data), len);
}

void SettingsWindow::Impl::onGlfwError(int errorCode, const char* description) {
    const char* text = (description != nullptr) ? description : "(no description)";
    const ShotHostServices* logHost = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_glfwErrorMutex);
        logHost = g_glfwErrorHost;
    }
    shot::Logger(logHost).error("[settings_ui] GLFW 错误 " + std::to_string(errorCode) + "：" + text);
}

void SettingsWindow::Impl::onKey(GLFWwindow* glfwWindow, int key, int /*scancode*/, int action,
                                 int /*mods*/) {
    auto* self = static_cast<Impl*>(glfwGetWindowUserPointer(glfwWindow));
    if (self == nullptr) {
        return;
    }
    // Esc 只隐藏窗口；文本框正在输入 / 正在录入热键时把 Esc 让给 ImGui
    // （取消输入 / 取消录入，而不是把整个设置页藏起来）。
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS &&
        ImGui::GetCurrentContext() != nullptr && !ImGui::GetIO().WantTextInput &&
        !self->panel.isCapturingHotkey()) {
        self->escHideRequested = true;  // 回调里只置位，隐藏动作放到事件循环里做
    }
}

// ---------------------------------------------------------------------------
// 公共接口
// ---------------------------------------------------------------------------
SettingsWindow::Impl::~Impl() = default;

const char* SettingsWindow::Impl::tr(const char* key, const char* fallback) const {
    // i18n 为空（老宿主 / stop 之后）时保留中文原文，界面行为与改造前一致。
    return (i18n != nullptr) ? i18n->tr(key, fallback) : fallback;
}

const char* SettingsWindow::Impl::resolve(const char* owner, const char* textOrKey) const {
    return (i18n != nullptr) ? i18n->resolve(owner, textOrKey) : textOrKey;
}

SettingsWindow::SettingsWindow() : m_impl(std::make_unique<Impl>()) {}

SettingsWindow::~SettingsWindow() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool SettingsWindow::start(const ShotHostServices* host) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->thread.joinable()) {
        return true;  // 已经在跑：重复 init 是空操作
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = true;
        m_impl->glfwInitialized = false;
        m_impl->quitRequested = false;
        std::queue<QueuedEvent>().swap(m_impl->queue);
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->readyMutex);
        m_impl->ready = false;
        m_impl->failed = false;
    }
    m_impl->host = host;

    // 多语言：先构造并发布本插件的语言表，再把只读解析器借给面板。窗口线程只读它，
    // 语言切换由宿主 i18n_list 变化驱动（消费方 resolve 时自动取新缓存）。
    m_impl->i18n = std::make_unique<shot::I18n>(host, kSettingsUi.id);
    m_impl->i18n->loadTables(kLangTables, std::size(kLangTables));
    m_impl->panel.setI18n(m_impl->i18n.get());

    // 先订阅再起线程：订阅之后到达的 settings.open 不会漏；窗口还没就绪时事件先入队，
    // 事件循环启动后统一 drain。
    shot::EventBus bus(host);
    if (bus.available()) {
        bool subscribedAny = false;
        subscribedAny =
            bus.subscribe(kTopicSettingsOpen, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicConfigApplied, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicPluginsChanged, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicListsChanged, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        subscribedAny =
            bus.subscribe(kTopicListsUpdated, &Impl::onBusEvent, m_impl.get()) || subscribedAny;
        m_impl->busSubscribed = subscribedAny;
        if (!subscribedAny) {
            shot::Logger(host).warn("settings_ui：订阅设置页事件失败，窗口只能靠 post_to_plugin 打开");
        }
    } else {
        // 旧版宿主没有事件总线：安静降级，设置窗口不会自动弹出。
        shot::Logger(host).warn("settings_ui：宿主未提供事件总线（旧版宿主？），设置窗口不会自动打开");
    }

    try {
        m_impl->thread = std::thread(&Impl::run, m_impl.get());
    } catch (...) {
        // 线程资源耗尽等：异常绝不跨越 DLL 边界。
        if (m_impl->busSubscribed) {
            shot::EventBus unsubscribeBus(host);
            unsubscribeBus.unsubscribe(m_impl.get());
            m_impl->busSubscribed = false;
        }
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(m_impl->readyMutex);
        m_impl->readyCv.wait(lock, [this]() { return m_impl->ready || m_impl->failed; });
        if (!m_impl->failed) {
            return true;
        }
    }
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();  // 已经 failed 返回了，这里不会阻塞
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
    }
    return false;
}

void SettingsWindow::stop() {
    if (!m_impl) {
        return;
    }
    // 1. 先退订事件总线：deinit 返回后宿主不再回调本插件（EVENT_BUS.md §5）。
    if (m_impl->busSubscribed) {
        shot::EventBus bus(m_impl->host);
        bus.unsubscribe(m_impl.get());
        m_impl->busSubscribed = false;
    }
    // 2. 拒绝后续投递，并让事件循环退出（隐藏时它阻塞在 glfwWaitEvents 上）。
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->acceptingEvents = false;
    }
    m_impl->requestQuit();
    // 3. join：返回前窗口一定已经销毁（销毁在 run() 收尾 / 失败路径里做）。
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    // 4. 窗口线程已经停下：先解除面板对解析器的引用，再销毁解析器（析构自动退订
    //    lists.changed）。顺序反了会留下悬空指针。
    m_impl->panel.setI18n(nullptr);
    m_impl->i18n.reset();
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->quitRequested = false;
        m_impl->glfwInitialized = false;
        std::queue<QueuedEvent>().swap(m_impl->queue);
    }
}

std::int32_t SettingsWindow::post(const char* name, const char* data, std::uint32_t len) {
    if (!m_impl || name == nullptr) {
        return -1;
    }
    try {
        std::string payload;
        if (data != nullptr && len > 0) {
            payload.assign(data, len);  // 契约要求：返回前把 data_len 字节拷走
        }
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (!m_impl->acceptingEvents) {
                return 1;  // 窗口不在（还没起好 / 正在关闭）：丢弃
            }
            m_impl->queue.push(QueuedEvent{name, std::move(payload)});
            wake = m_impl->glfwInitialized;
        }
        if (wake) {
            // glfwPostEmptyEvent 返回 void（"唤醒"本身没有失败返回值可用），事件已在队列里，
            // 即使这次唤醒丢了，事件循环下一轮也会 drain 到。
            glfwPostEmptyEvent();
        }
        return 0;
    } catch (...) {
        return 2;  // 分配失败等：异常绝不跨越 DLL 边界
    }
}

}  // namespace settings_ui
