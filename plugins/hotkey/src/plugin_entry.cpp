// 唯一的导出符号 + 索引目录 + ABI 胶水。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的索引，必须与 metadata.json 的
//     plugins[] 顺序一一对应（两者都只有 hotkey.main 一个条目）；
//   · 热键插件"会什么"由它自己的能力目录回答：shot.ui_provider +
//     shot.configurable，宿主用 query_interface 逐个询问，不需要知道有几种插件类型；
//   · 宿主拿到的 instance 是一个不透明指针：插件自己分配、自己释放；
//   · ABI 胶水（状态对象 + 回调 + 能力目录 + 插件身份证）也放在这个文件——它才是
//     "入口"，热键/配置实现文件不再混 ABI 代码（STYLE.md §6.8）。
//
// 匿名命名空间里的函数都是 ABI 回调 / 模块表回调：签名由契约固定，只能是自由函数
// （STYLE.md §6.7 的合法例外）。

#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <utility>

#include "hotkey_config.h"
#include "hotkey_window.h"
#include "plugin_api.h"
#include "plugin_entry.h"
#include "shot_plugin_helpers.h"

namespace {

using hotkey::PluginEntry;

const PluginEntry* const kEntries[] = {
    &hotkey::kHotkey,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

using hotkey::PluginInstance;

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

    // 用 shot::Logger 报一句自己的 id：宿主日志里能看到 hotkey.main 已被创建。
    // 老宿主可能没有 log 字段，shot::Logger 内部已经做了降级，这里不用再手写判空。
    shot::Logger(host).info(entry->id);
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
        return 0;  // 老宿主没有服务表，热键仍可注册（只是发布事件会安静失败）
    }
    if (SHOT_HAS_FIELD(host, ShotHostServices, abi_major) && host->abi_major != SHOT_ABI_MAJOR) {
        return 1;
    }
    // 热键依赖事件总线（发布 capture.request / settings.open）：没有总线时注册照做、
    // 发布失败记 WARN 后安静降级；不在这里拒绝加载——少一个服务不该让热键整个消失。
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

// ---------------------------------------------------------------------------
// ABI 胶水：插件状态对象 + UI/配置回调 + 能力目录 + 静态插件描述。
// 插件对象由插件 create / destroy，宿主只拿到一个不透明指针。
// ---------------------------------------------------------------------------
namespace hotkey {
namespace {

/**
 * @brief 用户可见文案的语言表（每个语言一张 JSON 对象，键集必须完全一致）。
 *
 * 只放设置页会显示的内容：四个配置项的 label / tooltip 在 ShotConfigDesc 里存的是
 * key，设置页按 display(owner, key) 到 i18n_list 里取这里的文本。日志文案固定中文、
 * 不进表（e2e 按中文日志断言，见 STYLE.md §6.4）；事件主题与配置 key 也不是用户文案。
 *
 * 单张表远超 120 列，所以拆成相邻字符串字面量：编译期拼成一条 JSON，JSON 字符串
 * 内部没有裸换行（有裸换行就不是合法 JSON 了）。
 */
const char* const kLangTables[] = {
    // zh-CN：简体中文表，与下面 en-US 表的 key 集完全一致。
    R"json({"@meta":"zh-CN",)json"
    R"json("config.enabled.label":"启用全局热键",)json"
    R"json("config.enabled.tooltip":"关闭后撤销全部热键注册；热键线程与窗口保留，重新启用立即生效",)json"
    R"json("config.capture_hotkey.label":"截屏热键",)json"
    R"json("config.capture_hotkey.tooltip":"发布 capture.request（空负载）；语法 )json"
    R"json(Ctrl/Alt/Shift/Win + A-Z / 0-9 / F1-F12 / PrintScreen，大小写不敏感",)json"
    R"json("config.region_hotkey.label":"区域截屏热键",)json"
    R"json("config.region_hotkey.tooltip":"发布 capture.request，负载 {\"mode\":\"region\"}；语法同上",)json"
    R"json("config.settings_hotkey.label":"打开设置热键",)json"
    R"json("config.settings_hotkey.tooltip":"发布 settings.open（空负载）；语法同上"})json",
    // en-US：与 zh-CN 表一一对应。
    R"json({"@meta":"en-US",)json"
    R"json("config.enabled.label":"Enable global hotkeys",)json"
    R"json("config.enabled.tooltip":"Unregisters all hotkeys when off; )json"
    R"json(the hotkey thread and window stay alive, and re-enabling takes effect immediately",)json"
    R"json("config.capture_hotkey.label":"Capture hotkey",)json"
    R"json("config.capture_hotkey.tooltip":"Publishes capture.request (empty payload); syntax )json"
    R"json(Ctrl/Alt/Shift/Win + A-Z / 0-9 / F1-F12 / PrintScreen, case-insensitive",)json"
    R"json("config.region_hotkey.label":"Region capture hotkey",)json"
    R"json("config.region_hotkey.tooltip":"Publishes capture.request with payload )json"
    R"json({\"mode\":\"region\"}; same syntax as above",)json"
    R"json("config.settings_hotkey.label":"Open settings hotkey",)json"
    R"json("config.settings_hotkey.tooltip":"Publishes settings.open (empty payload); same syntax as above"})json",
};

/** @brief 插件状态：一个热键窗口 + 四个配置项（生命周期覆盖整个插件实例）。 */
struct HotkeyState {
    HotkeyConfig config; /**< shot.configurable：enabled + 三个热键字符串。 */
    HotkeyWindow window; /**< shot.ui_provider：隐藏消息窗口 + 热键线程。 */
    std::unique_ptr<shot::I18n> i18n; /**< 多语言：ui_init 构造并发布语言表，deinit/destroy 释放。 */
};

void* create_state() {
    return new (std::nothrow) HotkeyState();
}

void destroy_state(void* self) {
    auto* state = static_cast<HotkeyState*>(self);
    if (state != nullptr) {
        try {
            state->window.stop();  // 先收热键线程/窗口，再释放状态
            state->i18n.reset();   // 析构自动退订 lists.changed，不能留给 delete 之后的回调
        } catch (...) {
            // 析构路径也不能让异常越过 ABI 边界；继续释放内存。
        }
        delete state;
    }
}

std::int32_t ui_init(void* self, const ShotHostServices* host) {
    // 异常绝不能越过 extern "C" 边界：语言表解析与 start() 内的建线程都可能分配内存，
    // 这里统一兜一层。
    if (auto* state = static_cast<HotkeyState*>(state_of(self))) {
        try {
            // ui_init 是第一个拿得到宿主服务表的生命周期回调：在这里构造 I18n 并把语言表
            // 发布到 i18n_source_list（设置页靠它把配置项 label/tooltip 解析成当前语言）。
            // 先构造到局部变量，loadTables 抛异常时 state 里不会留下半初始化的对象。
            if (state->i18n == nullptr) {
                auto i18n = std::make_unique<shot::I18n>(host, "hotkey.main");
                i18n->loadTables(kLangTables, std::size(kLangTables));
                state->i18n = std::move(i18n);
            }
            return state->window.start(host, &state->config) ? 0 : 1;
        } catch (...) {
            return 1;
        }
    }
    return 1;
}

std::int32_t ui_deinit(void* self) {
    // 返回前必须已撤销全部热键、停线程、退订事件总线（SETTINGS.md §3.1 的硬性要求）。
    if (auto* state = static_cast<HotkeyState*>(state_of(self))) {
        try {
            state->window.stop();
            // 语言表随实例一起释放；宿主按 owner 清掉本插件贡献的 i18n_source_list 条目。
            state->i18n.reset();
        } catch (...) {
            return 1;
        }
    }
    return 0;
}

std::int32_t ui_post(void* self, const ShotEvent* ev) {
    auto* state = static_cast<HotkeyState*>(state_of(self));
    if (state == nullptr || ev == nullptr || ev->name == nullptr) {
        return -1;
    }
    if (!SHOT_HAS_FIELD(ev, ShotEvent, data_len)) {
        return -1;  // 老宿主的事件结构里没有 data_len，不猜
    }
    try {
        return state->window.post(ev->name, ev->data, ev->data_len);
    } catch (...) {
        return -1;
    }
}

// ---- shot.configurable：四个配置项由 HotkeyConfig 拥有，ABI 层只做参数转发 ----

std::uint32_t config_count(void* self) {
    auto* state = static_cast<HotkeyState*>(state_of(self));
    return (state == nullptr) ? 0 : state->config.configCount();
}

std::int32_t config_desc(void* self, std::uint32_t index, ShotConfigDesc* out) {
    auto* state = static_cast<HotkeyState*>(state_of(self));
    return (state == nullptr) ? 1 : state->config.configDesc(index, out);
}

std::int32_t config_get(void* self, const char* key, char* buf, std::uint32_t buf_size) {
    auto* state = static_cast<HotkeyState*>(state_of(self));
    return (state == nullptr) ? -1 : state->config.getValue(key, buf, buf_size);
}

std::int32_t config_set(void* self, const char* key, const char* value) {
    auto* state = static_cast<HotkeyState*>(state_of(self));
    if (state == nullptr) {
        return -1;
    }
    const std::int32_t rc = state->config.setValue(key, value);
    // 值变了就请求重注册：宿主启动的 apply_stored 直接 set_config、**不发**
    // config.applied；只靠事件会让"启动时读到的已存热键"不生效。设置页应用
    // （lists.update → set_config，随后 lists.commit → config.applied）走同一条捷径，
    // 事件到达时再重注册一次是幂等的。这里只入队 + PostMessage，不等结果。
    if (rc == 0) {
        try {
            state->window.requestReapply();
        } catch (...) {
            // 唤醒失败不影响配置值本身；下一次 config.applied / 重新加载时还会重试。
        }
    }
    return rc;
}

const ShotUIProviderV1 kUiOps = SHOT_OPS(ShotUIProviderV1,
                                         .init           = &ui_init,
                                         .deinit         = &ui_deinit,
                                         .post_to_plugin = &ui_post);

const ShotConfigurableV1 kConfigOps = SHOT_OPS(ShotConfigurableV1,
                                               .get_config_count = &config_count,
                                               .get_config_desc  = &config_desc,
                                               .get_config       = &config_get,
                                               .set_config       = &config_set);

const ShotIfaceEntry kIfaces[] = {
    { SHOT_IFACE_UI_PROVIDER, 1, &kUiOps },
    { SHOT_IFACE_CONFIGURABLE, 1, &kConfigOps },
    { nullptr, 0, nullptr },
};

}  // namespace

const PluginEntry kHotkey = {
    "hotkey.main", "Hotkey", "0.1.0", &create_state, &destroy_state, kIfaces,
};

}  // namespace hotkey
