// 唯一的导出符号 + 索引目录 + ABI 胶水。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的索引，必须与 metadata.json 的
//     plugins[] 顺序一一对应（两者都只有 tray.main 一个条目）；
//   · 托盘"会什么"由它自己的能力目录回答：shot.ui_provider + shot.configurable，
//     宿主用 query_interface 逐个询问，不需要知道有几种插件类型；
//   · 宿主拿到的 instance 是一个不透明指针：插件自己分配、自己释放；
//   · ABI 胶水（状态对象 + 回调 + 能力目录 + 插件身份证）也放在这个文件——它才是
//     "入口"，托盘/配置实现文件不再混 ABI 代码（STYLE.md §6.8）。
//
// 匿名命名空间里的函数都是 ABI 回调 / 模块表回调：签名由契约固定，只能是自由函数
// （STYLE.md §6.7 的合法例外）。

#include <cstdint>
#include <iterator>
#include <memory>
#include <new>

#include "plugin_api.h"
#include "plugin_entry.h"
#include "shot_plugin_helpers.h"
#include "tray_config.h"
#include "tray_icon.h"

namespace {

using tray::PluginEntry;

const PluginEntry* const kEntries[] = {
    &tray::kTray,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

using tray::PluginInstance;

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

    // 用 shot::Logger 报一句自己的 id：宿主日志里能看到 tray.main 已被创建。
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
        return 0;  // 老宿主没有服务表，托盘仍然可以出现（只是发不了事件）
    }
    if (SHOT_HAS_FIELD(host, ShotHostServices, abi_major) && host->abi_major != SHOT_ABI_MAJOR) {
        return 1;
    }
    // 托盘依赖事件总线与列表服务（ABI 2.0 宿主提供）：没有总线时发布事件失败、
    // 没有列表服务时菜单退化成本地兜底「退出」，都由使用方记 WARN 并静默降级，
    // 不在这里拒绝加载（少一个服务不该让托盘图标整个消失）。
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
namespace tray {
namespace {

// 托盘自己的用户可见文案（配置项 label/tooltip、兜底退出、悬停提示）。
// 每语言一张 JSON 表，@meta 必填；key 与 tray_config.cpp / tray_menu.cpp 里的
// 引用一一对应。日志不在这里：日志一律中文（document/I18N.md）。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"托盘",)json"
    R"json("plugin.description":"托盘图标与菜单：菜单由插件贡献的数据驱动，点击只发布条目声明的事件",)json"
    R"json("config.icon.label":"托盘图标",)json"
    R"json("config.icon.tooltip":"选择 .ico 文件自定义托盘图标；留空使用宿主应用图标",)json"
    R"json("tray.fallback.quit":"退出",)json"
    R"json("tray.tooltip":"Plugin-shot 托盘"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"Tray",)json"
    R"json("plugin.description":"Tray icon and menu: the menu is driven by plugin-contributed data and clicks only publish the declared events",)json"
    R"json("config.icon.label":"Tray icon",)json"
    R"json("config.icon.tooltip":"Pick an .ico file to customize the tray icon; leave empty to use the host icon",)json"
    R"json("tray.fallback.quit":"Quit",)json"
    R"json("tray.tooltip":"Plugin-shot tray"})json",
};

/** @brief 插件状态：一个托盘图标 + 一个配置项（生命周期覆盖整个插件实例）。 */
struct TrayState {
    TrayConfig config;                 /**< shot.configurable：icon。 */
    TrayIcon icon;                     /**< shot.ui_provider：托盘线程 + 图标。 */
    std::unique_ptr<shot::I18n> i18n;  /**< 多语言：语言表 + i18n_list 解析。 */
};

void* create_state() {
    return new (std::nothrow) TrayState();
}

void destroy_state(void* self) {
    auto* state = static_cast<TrayState*>(self);
    if (state != nullptr) {
        try {
            state->icon.stop();  // 先收托盘线程/图标，再释放状态
        } catch (...) {
            // 析构路径也不能让异常越过 ABI 边界；继续释放内存。
        }
        state->i18n.reset();  // 析构会自动退订 lists.changed
        delete state;
    }
}

std::int32_t ui_init(void* self, const ShotHostServices* host) {
    // 异常绝不能越过 extern "C" 边界：start() 内部已兜住建线程，这里再兜一层。
    if (auto* state = static_cast<TrayState*>(state_of(self))) {
        try {
            // 先建 i18n 再起托盘线程：托盘线程重建菜单时会用 resolve 解析 label。
            if (!state->i18n) {
                state->i18n = std::make_unique<shot::I18n>(host, kTray.id);
                state->i18n->loadTables(kLangTables, std::size(kLangTables));
            }
            return state->icon.start(host, &state->config, state->i18n.get()) ? 0 : 1;
        } catch (...) {
            return 1;
        }
    }
    return 1;
}

std::int32_t ui_deinit(void* self) {
    // 返回前必须已退订事件总线、停线程、删图标（SETTINGS.md §3.1 的硬性要求）。
    if (auto* state = static_cast<TrayState*>(state_of(self))) {
        try {
            state->icon.stop();
            state->i18n.reset();  // 托盘线程已停，再退订/释放 i18n
        } catch (...) {
            return 1;
        }
    }
    return 0;
}

std::int32_t ui_post(void* self, const ShotEvent* ev) {
    auto* state = static_cast<TrayState*>(state_of(self));
    if (state == nullptr || ev == nullptr || ev->name == nullptr) {
        return -1;
    }
    if (!SHOT_HAS_FIELD(ev, ShotEvent, data_len)) {
        return -1;  // 老宿主的事件结构里没有 data_len，不猜
    }
    try {
        return state->icon.post(ev->name, ev->data, ev->data_len);
    } catch (...) {
        return -1;
    }
}

// ---- shot.configurable：icon 由 TrayConfig 拥有，ABI 层只做参数转发 ----

std::uint32_t config_count(void* self) {
    auto* state = static_cast<TrayState*>(state_of(self));
    return (state == nullptr) ? 0 : state->config.configCount();
}

std::int32_t config_desc(void* self, std::uint32_t index, ShotConfigDesc* out) {
    auto* state = static_cast<TrayState*>(state_of(self));
    return (state == nullptr) ? 1 : state->config.configDesc(index, out);
}

std::int32_t config_get(void* self, const char* key, char* buf, std::uint32_t buf_size) {
    auto* state = static_cast<TrayState*>(state_of(self));
    return (state == nullptr) ? -1 : state->config.getValue(key, buf, buf_size);
}

std::int32_t config_set(void* self, const char* key, const char* value) {
    auto* state = static_cast<TrayState*>(state_of(self));
    if (state == nullptr) {
        return -1;
    }
    const std::int32_t result = state->config.setValue(key, value);
    if (result == 0) {
        // 配置可能在宿主 init 的 apply_stored 阶段就被推过来（那时还没有
        // config.applied 事件），也可能来自设置页应用；两种路径都只需唤醒托盘
        // 线程"重读配置"——要不要真换图标由托盘线程按路径是否变化自己判断。
        state->icon.requestIconRefresh();
    }
    return result;
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

const PluginEntry kTray = {
    "tray.main", "Tray", "0.1.0", &create_state, &destroy_state, kIfaces,
};

}  // namespace tray
