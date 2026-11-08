// 唯一的导出符号 + 索引目录 + ABI 胶水。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的索引，必须与 metadata.json 的
//     plugins[] 顺序一一对应；
//   · 某个插件"会什么"由它自己的能力目录回答，宿主用 query_interface 逐个询问；
//   · 宿主拿到的 instance 是一个不透明指针：插件自己分配、自己释放；
//   · ABI 胶水（状态对象 + 三个回调 + 能力目录 + 插件身份证）也放在这个文件——它才是
//     "入口"，窗口实现文件不再混 ABI 代码（STYLE.md §6.8）。
//
// 匿名命名空间里的函数都是 ABI 回调 / 模块表回调：签名由契约固定，只能是自由函数
// （STYLE.md §6.7 的合法例外）。

#include <cstdint>
#include <iterator>
#include <new>

#include "plugin_api.h"
#include "plugin_entry.h"
#include "settings_window.h"
#include "shot_plugin_helpers.h"

namespace {

using settings_ui::PluginEntry;

const PluginEntry* const kEntries[] = {
    &settings_ui::kSettingsUi,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

using settings_ui::PluginInstance;

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
        return 0;  // 老宿主没有服务表，仍然可以工作（设置页安静降级）
    }
    if (SHOT_HAS_FIELD(host, ShotHostServices, abi_major) && host->abi_major != SHOT_ABI_MAJOR) {
        return 1;
    }
    // 本插件用到的两个宿主服务是 abi 1.3 才有的；老宿主照常加载（宿主可能没有它们），
    // 由 UI 侧的降级逻辑负责提示，不在这里拒绝加载。
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
// ABI 胶水：插件状态对象 + 三个回调 + 能力目录 + 静态插件描述。
// 与 plugins/setting_window 同一套写法：插件对象由插件 create / destroy，
// 宿主只拿到一个不透明指针。
// ---------------------------------------------------------------------------
namespace settings_ui {
namespace {

/** @brief 插件状态：单窗口单实例，一个 SettingsWindow 就够。 */
struct SettingsUiState {
    SettingsWindow window;
};

void* create_state() {
    return new (std::nothrow) SettingsUiState();
}

void destroy_state(void* self) {
    auto* state = static_cast<SettingsUiState*>(self);
    if (state != nullptr) {
        try {
            state->window.stop();  // 先收窗口线程，再释放状态
        } catch (...) {
            // 析构路径也不能让异常越过 ABI 边界；继续释放内存。
        }
        delete state;
    }
}

std::int32_t ui_init(void* self, const ShotHostServices* host) {
    // 异常绝不能越过 extern "C" 边界：start() 内部已兜住建线程，这里再兜一层
    if (auto* state = static_cast<SettingsUiState*>(state_of(self))) {
        try {
            return state->window.start(host) ? 0 : 1;
        } catch (...) {
            return 1;
        }
    }
    return 1;
}

std::int32_t ui_deinit(void* self) {
    // 返回前必须已退订事件总线、停线程、销毁窗口（SETTINGS.md §3.1 的硬性要求）。
    if (auto* state = static_cast<SettingsUiState*>(state_of(self))) {
        try {
            state->window.stop();
        } catch (...) {
            return 1;
        }
    }
    return 0;
}

std::int32_t ui_post(void* self, const ShotEvent* ev) {
    auto* state = static_cast<SettingsUiState*>(state_of(self));
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

const ShotUIProviderV1 kUiOps = SHOT_OPS(ShotUIProviderV1,
                                         .init           = &ui_init,
                                         .deinit         = &ui_deinit,
                                         .post_to_plugin = &ui_post);

const ShotIfaceEntry kIfaces[] = {
    { SHOT_IFACE_UI_PROVIDER, 1, &kUiOps },
    { nullptr, 0, nullptr },
};

}  // namespace

const PluginEntry kSettingsUi = {
    "settings_ui.main", "Settings", "0.1.0", &create_state, &destroy_state, kIfaces,
};

}  // namespace settings_ui
