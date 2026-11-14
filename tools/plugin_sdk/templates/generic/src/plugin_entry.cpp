// 唯一导出符号 + 索引目录 + ABI 胶水（Generic 模板）。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的下标，必须与 metadata.json 的
//     plugins[] 顺序一一对应（本模板只有一个插件，对应关系仍然保留）；
//   · 插件"会什么"由它自己的能力目录回答，宿主用 query_interface 逐个询问；
//   · 宿主拿到的 instance 是一个不透明指针（PluginInstance）：插件自己分配、
//     自己释放，宿主绝不 delete。
//
// 匿名命名空间里的函数都是 ABI 回调 / 模块表回调：签名由契约固定，只能是自由
// 函数（STYLE.md §6.7 的合法例外）。

#include <cstdint>
#include <iterator>
#include <new>
#include <string>

#include "generic_plugin.h"
#include "plugin_api.h"
#include "plugin_entry.h"
#include "shot_plugin_helpers.h"

namespace generic_template {

void* state_of(void* self) {
    auto* instance = static_cast<PluginInstance*>(self);
    return instance == nullptr ? nullptr : instance->state;
}

namespace {

/** @brief 分配插件状态：宿主 create(index) 的第一步。 */
void* create_state() {
    return new (std::nothrow) GenericPlugin();
}

/** @brief 释放插件状态：先退订事件总线，再 delete（析构里也会兜底 stop）。 */
void destroy_state(void* state) {
    auto* plugin = static_cast<GenericPlugin*>(state);
    if (plugin != nullptr) {
        plugin->stop();
        delete plugin;
    }
}

/** @brief 绑定宿主：发布语言表 + 订阅 capture.done + 贡献托盘命令。 */
bool start_state(void* state, const ShotHostServices* host) {
    auto* plugin = static_cast<GenericPlugin*>(state);
    return plugin != nullptr && plugin->start(host);
}

// ---- shot.configurable：ABI 层只做参数转发，配置逻辑在 GenericPlugin ----

std::uint32_t config_count(void* self) {
    auto* plugin = static_cast<GenericPlugin*>(state_of(self));
    return plugin == nullptr ? 0 : plugin->configCount();
}

std::int32_t config_desc(void* self, std::uint32_t index, ShotConfigDesc* out) {
    auto* plugin = static_cast<GenericPlugin*>(state_of(self));
    return plugin == nullptr ? 1 : plugin->configDesc(index, out);
}

std::int32_t config_get(void* self, const char* key, char* buf, std::uint32_t bufSize) {
    auto* plugin = static_cast<GenericPlugin*>(state_of(self));
    return plugin == nullptr ? -1 : plugin->getConfig(key, buf, bufSize);
}

std::int32_t config_set(void* self, const char* key, const char* value) {
    auto* plugin = static_cast<GenericPlugin*>(state_of(self));
    return plugin == nullptr ? -1 : plugin->setConfig(key, value);
}

const ShotConfigurableV1 kConfigOps = SHOT_OPS(ShotConfigurableV1,
                                               .get_config_count = &config_count,
                                               .get_config_desc  = &config_desc,
                                               .get_config       = &config_get,
                                               .set_config       = &config_set);

// 能力目录：宿主按 id + version 查询；必须 {nullptr, 0, nullptr} 结尾。
const ShotIfaceEntry kGenericIfaces[] = {
    {SHOT_IFACE_CONFIGURABLE, 1, &kConfigOps},
    {nullptr, 0, nullptr},
};

}  // namespace

// 本模板唯一插件的静态描述：id / name / version 必须与 metadata.json 一致。
// 三者由 new_plugin.cmake 从命令行替换（{{PLUGIN_ID}} 等），不要只改一边。
const PluginEntry kGeneric = {
    .id      = "{{PLUGIN_ID}}",
    .name    = "{{PLUGIN_NAME}}",
    .version = "{{VERSION}}",
    .ifaces  = kGenericIfaces,
    .create  = &create_state,
    .destroy = &destroy_state,
    .start   = &start_state,
    .configCount = &config_count,
    .configDesc  = &config_desc,
    .configGet   = &config_get,
    .configSet   = &config_set,
};

}  // namespace generic_template

namespace {

using generic_template::PluginEntry;
using generic_template::PluginInstance;

const PluginEntry* const kEntries[] = {
    &generic_template::kGeneric,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

/**
 * @brief 创建第 index 个插件实例。
 * @param[in] index 0..kEntryCount-1。
 * @param[in] host  宿主服务表；create 是插件唯一能拿到它的时机。
 * @return 不透明实例指针；失败返回 nullptr，宿主跳过该插件。
 */
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

    // 用 shot::Logger 报一句自己的 id；老宿主没有 log 服务时 helper 内部安静降级。
    shot::Logger(host).info(entry->id);

    // 初始化只降级、不失败：订阅不到事件总线时插件仍可提供配置项。
    // 异常绝不穿过 extern "C" 边界，所以这里兜底 catch (...)。
    if (entry->start != nullptr) {
        try {
            entry->start(state, host);
        } catch (...) {
            shot::Logger(host).warn(std::string(entry->id) + " 初始化时异常，部分功能不可用");
        }
    }
    return instance;
}

/** @brief 销毁插件实例：先让插件自己收资源，再释放句柄。 */
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

/** @brief 按能力 id 查询本实例的函数表；没有这个能力返回 nullptr（宿主安静跳过）。 */
void* query_instance(void* handle, const char* iface_id, std::uint32_t version) {
    auto* instance = static_cast<PluginInstance*>(handle);
    if (instance == nullptr) {
        return nullptr;
    }
    return shot_query_interface(instance->entry->ifaces, iface_id, version);
}

/** @brief 填插件身份证；宿主不创建实例也能列清单。 */
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

/** @brief 模块级初始化：拿宿主 ABI 版本做一次协商。 */
std::int32_t module_init(const ShotHostServices* host, std::uint32_t flags) {
    (void)flags;
    if (host == nullptr) {
        return 0;  // 老宿主没有服务表，仍然可以工作（只是没有事件订阅）
    }
    if (SHOT_HAS_FIELD(host, ShotHostServices, abi_major) && host->abi_major != SHOT_ABI_MAJOR) {
        return 1;  // 返回非 0 = 本 DLL 无法在这个宿主上工作，宿主会直接卸载它
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
 * @return 指向插件内静态存储的模块表，DLL 卸载前一直有效。
 */
SCREENSHOT_API const ShotModule* shot_module_entry(void) { return &kModule; }

}  // extern "C"
