// 唯一的导出符号 + 索引目录 + ABI 胶水。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的索引，必须与 metadata.json 的
//     plugins[] 顺序一一对应（本包只有 notify.main 一个条目）；
//   · 消息提示"会什么"由它自己的能力目录回答：shot.configurable，宿主用
//     query_interface 逐个询问，不需要知道有几种插件类型；
//   · 宿主拿到的 instance 是一个不透明指针：插件自己分配、自己释放；
//   · ABI 胶水（状态创建 + 配置回调 + 能力目录 + 插件身份证）放在这个文件，
//     通知气泡与线程在 notify_toast*.cpp、配置项在 notify_config.cpp
//     （STYLE.md §6.8）。
//
// 匿名命名空间里的函数都是 ABI 回调 / 模块表回调：签名由契约固定，只能是自由函数
// （STYLE.md §6.7 的合法例外）。

#include <cstdint>
#include <iterator>
#include <memory>
#include <new>

#include "notify_config.h"
#include "notify_toast.h"
#include "plugin_api.h"
#include "plugin_entry.h"
#include "shot_plugin_helpers.h"

// ---------------------------------------------------------------------------
// 插件状态与 shot.configurable 回调（放在 ABI 入口之前：create_instance 要用状态类型）
// ---------------------------------------------------------------------------
namespace notify {
namespace {

// 本插件的全部用户可见文案：配置项的 label/tooltip 是 key（由宿主 config_list
// 原样带给设置页，消费方再用 i18n_list 解析），气泡标题/正文在显示时按当前语言查。
// 每语言一张 JSON 表，@meta 是语言标签；两张表的 key 集必须完全一致。
// 表按相邻 raw 字符串字面量拼接（编译期合成一个 JSON 文本）：长文案可以换行，
// 既不会在 JSON 里留下裸换行，也不会出现超宽行。
// 配置 key 与 notify_config.cpp 的 kConfig 一一对应。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("config.enabled.label":"消息提示",)json"
    R"json("config.enabled.tooltip":)json"
    R"json("截屏成功后弹出系统通知显示保存路径；关掉后本插件的所有提示都不弹",)json"
    R"json("config.show_on_success.label":"截屏成功时提示",)json"
    R"json("config.show_on_success.tooltip":)json"
    R"json("成功时显示\"已保存到 <文件路径>\"（路径过长会截断中间部分）",)json"
    R"json("config.show_on_error.label":"截屏失败时提示",)json"
    R"json("config.show_on_error.tooltip":)json"
    R"json("失败时显示\"截图失败：<原因>\"；截屏结果提示统一由本插件负责（托盘不弹截屏气泡）",)json"
    R"json("config.sound.label":"提示音",)json"
    R"json("config.sound.tooltip":)json"
    R"json("通知气泡是否播放系统提示音；关掉后使用 NIIF_NOSOUND 静音",)json"
    R"json("toast.success_title":"截屏成功",)json"
    R"json("toast.failed_title":"截屏失败",)json"
    R"json("toast.saved_prefix":"已保存到 ",)json"
    R"json("toast.failed_prefix":"截图失败：",)json"
    R"json("toast.no_path":"已保存（保存插件未回报路径）",)json"
    R"json("toast.path_unavailable":"（路径无法显示）",)json"
    R"json("toast.reason_unknown":"原因未知",)json"
    R"json("toast.window_title":"Plugin-shot 消息提示",)json"
    R"json("toast.icon_tooltip":"Plugin-shot 消息提示（隐藏占位）"})json",
    R"json({"@meta":"en-US",)json"
    R"json("config.enabled.label":"Notifications",)json"
    R"json("config.enabled.tooltip":)json"
    R"json("Show the saved path in a system notification; off disables all notifications",)json"
    R"json("config.show_on_success.label":"Notify on success",)json"
    R"json("config.show_on_success.tooltip":)json"
    R"json("Show \"Saved to <path>\" on success (long paths are truncated)",)json"
    R"json("config.show_on_error.label":"Notify on failure",)json"
    R"json("config.show_on_error.tooltip":)json"
    R"json("Show \"Capture failed: <reason>\" (the tray does not show balloons)",)json"
    R"json("config.sound.label":"Sound",)json"
    R"json("config.sound.tooltip":)json"
    R"json("Play the system sound for the notification; off uses NIIF_NOSOUND",)json"
    R"json("toast.success_title":"Capture saved",)json"
    R"json("toast.failed_title":"Capture failed",)json"
    R"json("toast.saved_prefix":"Saved to ",)json"
    R"json("toast.failed_prefix":"Capture failed: ",)json"
    R"json("toast.no_path":"Saved (the saver did not report a path)",)json"
    R"json("toast.path_unavailable":"(path unavailable)",)json"
    R"json("toast.reason_unknown":"Unknown reason",)json"
    R"json("toast.window_title":"Plugin-shot notifications",)json"
    R"json("toast.icon_tooltip":"Plugin-shot notifications (hidden placeholder)"})json",
};

/** @brief 插件状态：一个语言表 + 一个消息提示器 + 四个配置项（覆盖整个实例生命周期）。 */
struct NotifyState {
    // I18n 不可拷贝/移动，用 unique_ptr 放状态里；声明在最前面，析构时最后释放，
    // 保证通知线程（读 tr）在它之前停下。create() 没有 host 参数，实际构造在
    // create_instance 里拿到服务表之后。
    std::unique_ptr<shot::I18n> i18n;
    NotifyConfig config; /**< shot.configurable：enabled / show_on_success / show_on_error / sound。 */
    NotifyToast toast;   /**< 通知后端（Windows 隐藏窗口图标线程 / Linux libnotify）。 */
};

void* create_state() {
    return new (std::nothrow) NotifyState();
}

void destroy_state(void* self) {
    auto* state = static_cast<NotifyState*>(self);
    if (state != nullptr) {
        // 返回前必须退订事件总线并释放平台通知后端（Windows 停线程/删隐藏图标，
        // Linux notify_uninit）：宿主之后可能 FreeLibrary 本 DLL（EVENT_BUS.md §5 /
        // SETTINGS.md §3.1 的生命周期要求）。
        try {
            state->toast.stop();
        } catch (...) {
            // 收尾路径也不能让异常越过 ABI 边界；继续释放内存。
        }
        // 通知线程已经 join，tr 不会再被调用；语言表先于状态释放（它析构时退订
        // lists.changed，宿主之后可能 FreeLibrary 本 DLL，必须在这里清干净）。
        state->i18n.reset();
        delete state;
    }
}

// ---- shot.configurable：四个开关由 NotifyConfig 拥有，ABI 层只做参数转发 ----

std::uint32_t config_count(void* self) {
    auto* state = static_cast<NotifyState*>(state_of(self));
    return (state == nullptr) ? 0 : state->config.configCount();
}

std::int32_t config_desc(void* self, std::uint32_t index, ShotConfigDesc* out) {
    auto* state = static_cast<NotifyState*>(state_of(self));
    return (state == nullptr) ? 1 : state->config.configDesc(index, out);
}

std::int32_t config_get(void* self, const char* key, char* buf, std::uint32_t buf_size) {
    auto* state = static_cast<NotifyState*>(state_of(self));
    return (state == nullptr) ? -1 : state->config.getValue(key, buf, buf_size);
}

std::int32_t config_set(void* self, const char* key, const char* value) {
    auto* state = static_cast<NotifyState*>(state_of(self));
    return (state == nullptr) ? -1 : state->config.setValue(key, value);
}

const ShotConfigurableV1 kConfigOps = SHOT_OPS(ShotConfigurableV1,
                                               .get_config_count = &config_count,
                                               .get_config_desc  = &config_desc,
                                               .get_config       = &config_get,
                                               .set_config       = &config_set);

const ShotIfaceEntry kIfaces[] = {
    { SHOT_IFACE_CONFIGURABLE, 1, &kConfigOps },
    { nullptr, 0, nullptr },
};

}  // namespace
}  // namespace notify

namespace {

using notify::PluginEntry;

const PluginEntry* const kEntries[] = {
    &notify::kNotify,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

using notify::PluginInstance;
using notify::state_of;

void* create_instance(std::uint32_t index, const ShotHostServices* host) {
    if (index >= kEntryCount) {
        return nullptr;
    }
    const PluginEntry* entry = kEntries[index];
    void* state = entry->create ? entry->create() : nullptr;
    if (state == nullptr) {
        return nullptr;
    }
    auto* instance = new (std::nothrow) PluginInstance{entry, state};
    if (instance == nullptr) {
        if (entry->destroy) {
            entry->destroy(state);
        }
        return nullptr;
    }

    // 用 shot::Logger 报一句自己的 id。老宿主可能没有 log 字段，shot::Logger 内部
    // 已经做了降级，这里不用再手写判空。
    shot::Logger(host).info(entry->id);

    // 本插件没有 ui_provider 的 init/deinit 生命周期：create(index, host) 是唯一能
    // 拿到宿主服务表的时机，所以语言表 + 事件订阅 + 平台通知后端都在实例创建路径里
    // 建立；退订/停后端/释放语言表在 destroy_state（destroy 路径）里做。
    if (entry == &notify::kNotify && instance->state != nullptr) {
        auto* notifyState = static_cast<notify::NotifyState*>(instance->state);
        // 语言表必须在通知线程启动前加载：气泡标题/正文是线程运行时按 key 查的。
        // 初始化失败不致命，tr 的 fallback 仍是中文原文，气泡照常显示。
        try {
            notifyState->i18n = std::make_unique<shot::I18n>(host, entry->id);
            notifyState->i18n->loadTables(notify::kLangTables, std::size(notify::kLangTables));
        } catch (...) {
            notifyState->i18n.reset();
            shot::Logger(host).warn("[notify] 初始化多语言失败，界面文案回退中文");
        }
        try {
            notifyState->toast.start(host, &notifyState->config, notifyState->i18n.get());
        } catch (...) {
            // 初始化失败不影响插件存活：消息提示降级为不可用，宿主继续截屏。
#if defined(_WIN32)
            shot::Logger(host).warn("[notify] 初始化通知线程时异常，消息提示不可用");
#else
            shot::Logger(host).warn("[notify] 初始化通知后端时异常，消息提示不可用");
#endif
        }
    }
    return instance;
}

void destroy_instance(void* handle) {
    auto* instance = static_cast<PluginInstance*>(handle);
    if (instance == nullptr) {
        return;
    }
    if (instance->entry->destroy) {
        instance->entry->destroy(instance->state);
    }
    delete instance;
}

void* query_instance(void* handle, const char* iface_id, std::uint32_t version) {
    auto* instance = static_cast<PluginInstance*>(handle);
    if (instance == nullptr) {
        return nullptr;
    }
    return shot_query_interface(instance->entry->ifaces, iface_id, version);
}

std::int32_t get_info(std::uint32_t index, ShotPluginInfo* out) {
    if (out == nullptr || index >= kEntryCount) {
        return 1;
    }
    // 先看宿主给的这块内存够不够长，再决定能填到哪。
    if (out->struct_size < SHOT_REQUIRES(ShotPluginInfo, version)) {
        return 2;
    }
    const PluginEntry* entry = kEntries[index];
    out->abi_minor = SHOT_ABI_MINOR;
    out->id = entry->id;
    out->name = entry->name;
    out->version = entry->version;
    return 0;
}

// 模块级初始化：拿宿主版本做一次协商。
// 返回非 0 = "这个 DLL 无法在本宿主上工作"，宿主会直接卸载它。
std::int32_t module_init(const ShotHostServices* host, std::uint32_t flags) {
    (void)flags;
    if (host == nullptr) {
        return 0;  // 老宿主没有服务表，消息提示只是不订阅事件
    }
    if (SHOT_HAS_FIELD(host, ShotHostServices, abi_major) && host->abi_major != SHOT_ABI_MAJOR) {
        return 1;
    }
    // 事件总线是 ABI 2.0 宿主的标准服务；没有总线时 start() 会 Warn 并降级，
    // 不在这里拒绝加载（少一个服务不该让插件整个消失）。
    return 0;
}

const ShotModule kModule = {
    .struct_size     = sizeof(ShotModule),
    .abi_major       = SHOT_ABI_MAJOR,
    .abi_minor       = SHOT_ABI_MINOR,
    .plugin_count    = kEntryCount,
    .init            = &module_init,
    .shutdown        = nullptr,  // 没有模块级资源要收
    .get_info        = &get_info,
    .create          = &create_instance,
    .destroy         = &destroy_instance,
    .query_interface = &query_instance,
};
}  // namespace

extern "C" {

/**
 * @brief 唯一导出符号，返回静态模块表。
 */
SCREENSHOT_API const ShotModule* shot_module_entry(void) { return &kModule; }

}  // extern "C"

namespace notify {

const PluginEntry kNotify = {
    "notify.main", "Notify", "0.1.0", &create_state, &destroy_state, kIfaces,
};

}  // namespace notify
