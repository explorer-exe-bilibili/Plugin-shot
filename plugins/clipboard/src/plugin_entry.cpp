// 唯一的导出符号 + 索引目录 + ABI 胶水。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的索引，必须与 metadata.json 的
//     plugins[] 顺序一一对应（本包只有一个插件，对应关系仍然保留）；
//   · 剪贴板"会什么"由它自己的能力目录回答：shot.configurable，宿主用
//     query_interface 逐个询问，不需要知道有几种插件类型；
//   · 宿主拿到的 instance 是一个不透明指针：插件自己分配、自己释放；
//   · ABI 胶水（状态创建 + 配置回调 + 能力目录 + 插件身份证）放在这个文件，
//     剪贴板写入与事件订阅在 clipboard_plugin.cpp / clipboard_writer.cpp
//     （STYLE.md §6.8）。
//
// 匿名命名空间里的函数都是 ABI 回调 / 模块表回调：签名由契约固定，只能是自由函数
// （STYLE.md §6.7 的合法例外）。

#include <cstdint>
#include <iterator>
#include <new>

#include "clipboard_plugin.h"
#include "plugin_api.h"
#include "plugin_entry.h"
#include "shot_plugin_helpers.h"

namespace {

using clipboard::PluginEntry;

const PluginEntry* const kEntries[] = {
    &clipboard::kClipboard,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

using clipboard::PluginInstance;
using clipboard::state_of;

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
    // 拿到宿主服务表的时机，所以 capture.done 的订阅在实例创建路径里建立；
    // 退订在 destroy_state（destroy 路径）里做。
    if (entry == &clipboard::kClipboard && instance->state != nullptr) {
        try {
            static_cast<clipboard::ClipboardPlugin*>(instance->state)->start(host);
        } catch (...) {
            // 订阅失败不影响插件存活：剪贴板功能降级为不可用，宿主继续截屏。
            shot::Logger(host).warn("[clipboard] 初始化事件订阅时异常，剪贴板功能不可用");
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
        return 0;  // 老宿主没有服务表，本插件只是不订阅事件
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

// ---------------------------------------------------------------------------
// ABI 胶水：插件状态对象 + 配置回调 + 能力目录 + 静态插件描述。
// 插件对象由插件 create / destroy，宿主只拿到一个不透明指针。
// ---------------------------------------------------------------------------
namespace clipboard {
namespace {

/** @brief 插件状态：一个 ClipboardPlugin（订阅 + 两个配置项）。 */
void* create_state() {
    return new (std::nothrow) ClipboardPlugin();
}

void destroy_state(void* self) {
    auto* state = static_cast<ClipboardPlugin*>(self);
    if (state != nullptr) {
        // 返回前必须退订事件总线：宿主收不到本实例的回调之后才能释放状态
        // （EVENT_BUS.md §5）。退订在异常边界内做，绝不让异常越过 extern "C"。
        try {
            state->stop();
        } catch (...) {
            // 退订失败也只能继续释放内存；宿主卸载后不会再分发到本实例。
        }
        delete state;
    }
}

// ---- shot.configurable：两个开关由 ClipboardConfig 拥有，ABI 层只做参数转发 ----

std::uint32_t config_count(void* self) {
    auto* state = static_cast<ClipboardPlugin*>(state_of(self));
    return (state == nullptr) ? 0 : state->configCount();
}

std::int32_t config_desc(void* self, std::uint32_t index, ShotConfigDesc* out) {
    auto* state = static_cast<ClipboardPlugin*>(state_of(self));
    return (state == nullptr) ? 1 : state->configDesc(index, out);
}

std::int32_t config_get(void* self, const char* key, char* buf, std::uint32_t buf_size) {
    auto* state = static_cast<ClipboardPlugin*>(state_of(self));
    return (state == nullptr) ? -1 : state->getConfig(key, buf, buf_size);
}

std::int32_t config_set(void* self, const char* key, const char* value) {
    auto* state = static_cast<ClipboardPlugin*>(state_of(self));
    return (state == nullptr) ? -1 : state->setConfig(key, value);
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

const PluginEntry kClipboard = {
    "clipboard.main", "Clipboard", "0.1.0", &create_state, &destroy_state, kIfaces,
};

}  // namespace clipboard
