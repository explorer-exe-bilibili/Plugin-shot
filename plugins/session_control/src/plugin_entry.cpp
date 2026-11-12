// 唯一的导出符号 + 索引目录 + ABI 胶水。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的索引，必须与 metadata.json 的
//     plugins[] 顺序一一对应（本包只有一个插件，对应关系仍然保留）；
//   · 本插件只声明 shot.configurable，宿主按"没有更强能力"的 Generic 包装加载；
//   · 宿主拿到的 instance 是一个不透明指针：插件自己分配、自己释放。
//
// 匿名命名空间里的函数都是 ABI 回调 / 模块表回调：签名由契约固定，只能是自由函数
// （STYLE §6.7 的合法例外）。

#include <cstdint>
#include <iterator>
#include <new>

#include "plugin_api.h"
#include "plugin_entry.h"
#include "session_control_plugin.h"
#include "shot_plugin_helpers.h"

namespace {

using session_control::PluginEntry;
using session_control::PluginInstance;

const PluginEntry* const kEntries[] = {
    &session_control::kSessionControl,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

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

    // Generic 插件没有 ui_provider 的 init/deinit：create(index, host) 是唯一能拿到
    // 宿主服务表的时机，所以 D-Bus 服务在实例创建路径里启动（destroy 里停止）。
    if (entry == &session_control::kSessionControl && instance->state != nullptr) {
        try {
            static_cast<session_control::SessionControlPlugin*>(instance->state)->start(host);
        } catch (...) {
            // ABI 边界：线程/GLib 初始化异常只让本插件的 D-Bus 入口降级，
            // 不能让异常穿出 create。
            shot::Logger(host).warn("[session_control] 初始化异常，D-Bus 入口不可用");
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
        return 0;  // 老宿主没有服务表，仍然可以工作（只是没有事件总线可发布）
    }
    if (SHOT_HAS_FIELD(host, ShotHostServices, abi_major) && host->abi_major != SHOT_ABI_MAJOR) {
        return 1;
    }
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
// ABI 胶水：配置回调 + 能力目录 + 静态插件描述。
// 插件对象由插件 create / destroy，宿主只拿到一个不透明指针。
// ---------------------------------------------------------------------------
namespace session_control {
namespace {

// ---- shot.configurable 的 C 回调 -------------------------------------------
// 签名由契约固定，只能是自由函数（STYLE §6.7 的合法例外）；每个回调先 state_of
// 把 self（PluginInstance*）还原成插件状态，再转给类方法。

std::uint32_t config_count(void* self) {
    const auto* plugin = static_cast<const SessionControlPlugin*>(state_of(self));
    return (plugin == nullptr) ? 0 : plugin->configCount();
}

std::int32_t config_desc(void* self, std::uint32_t index, ShotConfigDesc* out) {
    const auto* plugin = static_cast<const SessionControlPlugin*>(state_of(self));
    return (plugin == nullptr) ? 1 : plugin->configDesc(index, out);
}

std::int32_t get_config(void* self, const char* key, char* buf, std::uint32_t bufSize) {
    const auto* plugin = static_cast<const SessionControlPlugin*>(state_of(self));
    return (plugin == nullptr) ? -1 : plugin->getConfig(key, buf, bufSize);
}

std::int32_t set_config(void* self, const char* key, const char* value) {
    auto* plugin = static_cast<SessionControlPlugin*>(state_of(self));
    if (plugin == nullptr) {
        return -1;
    }
    // 停/启 D-Bus 线程的路径可能抛 std::system_error：边界上兜住，按"值非法"返回，
    // 不影响其它配置项与宿主。
    try {
        return plugin->setConfig(key, value);
    } catch (...) {
        return -1;
    }
}

const ShotConfigurableV1 kConfigurable = SHOT_OPS(
    ShotConfigurableV1,
    .get_config_count = &config_count,
    .get_config_desc  = &config_desc,
    .get_config       = &get_config,
    .set_config       = &set_config);

const ShotIfaceEntry kIfaces[] = {
    {SHOT_IFACE_CONFIGURABLE, 1, &kConfigurable},
    {nullptr, 0, nullptr},
};

/** @brief 创建插件状态；失败返回 nullptr（宿主会跳过这个插件）。 */
void* create_state() {
    // 构造函数里会 new 服务实现状态（可能 bad_alloc）：ABI 边界上收敛成 nullptr。
    try {
        return new (std::nothrow) SessionControlPlugin();
    } catch (...) {
        return nullptr;
    }
}

/** @brief 释放插件状态：析构里的 stop() 会注销 D-Bus 名称并 join 服务线程。 */
void destroy_state(void* self) { delete static_cast<SessionControlPlugin*>(self); }

}  // namespace

// 插件身份证：id 必须与 metadata.json 的 plugins[].id 完全一致（kEntries 也靠它索引），
// 名称与版本只是展示信息。
const PluginEntry kSessionControl = {
    .id      = "session_control.main",
    .name    = "Session Control",
    .version = "0.1.0",
    .create  = &create_state,
    .destroy = &destroy_state,
    .ifaces  = kIfaces,
};

}  // namespace session_control
