// 唯一的导出符号 + 索引目录 + ABI 胶水。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的索引，必须与 metadata.json 的
//     plugins[] 顺序一一对应（本包只有一个插件 long_capture.main）；
//   · 插件"会什么"由它自己的能力目录回答：本插件只声明 shot.configurable；
//   · 宿主拿到的 instance 是不透明指针：插件自己分配、自己释放。
//
// 本插件没有 ui_provider 的 init/deinit：create(index, host) 是唯一能拿到宿主服务
// 表的时机，所以语言表、事件订阅、托盘命令、全局按键与会话线程都在实例创建路径里
// 建立（LongCapture::start），destroy 路径里统一收干净（LongCapture::stop）。
//
// 匿名命名空间里的函数都是 ABI 回调 / 模块表回调：签名由契约固定，只能是自由函数
// （STYLE.md §6.7 的合法例外）。

#include <cstdint>
#include <iterator>
#include <new>

#include "long_capture.h"
#include "plugin_api.h"
#include "plugin_entry.h"
#include "shot_plugin_helpers.h"

namespace long_capture {
namespace {

// ---- 插件状态与 shot.configurable 回调（ABI 层只做参数转发） ----

void* create_state() { return new (std::nothrow) LongCapture(); }

void destroy_state(void* self) {
    auto* state = static_cast<LongCapture*>(self);
    if (state == nullptr) {
        return;
    }
    // stop() 幂等：停会话线程、注销 Enter/Esc 与触发键、销毁 HUD/按键/提示窗口类、
    // 退订事件总线并释放语言表。宿主随后可能 FreeLibrary 本 DLL，必须在这里做完。
    try {
        state->stop();
    } catch (...) {
        // 收尾路径也不能让异常越过 ABI 边界；继续释放内存。
    }
    delete state;
}

std::uint32_t config_count(void* self) {
    auto* state = static_cast<LongCapture*>(state_of(self));
    return (state == nullptr) ? 0 : state->configCount();
}

std::int32_t config_desc(void* self, std::uint32_t index, ShotConfigDesc* out) {
    auto* state = static_cast<LongCapture*>(state_of(self));
    return (state == nullptr) ? 1 : state->configDesc(index, out);
}

std::int32_t config_get(void* self, const char* key, char* buf, std::uint32_t buf_size) {
    auto* state = static_cast<LongCapture*>(state_of(self));
    return (state == nullptr) ? -1 : state->getConfig(key, buf, buf_size);
}

std::int32_t config_set(void* self, const char* key, const char* value) {
    auto* state = static_cast<LongCapture*>(state_of(self));
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

/** @brief 插件 long_capture.main 的静态描述：只声明 shot.configurable + 一个配置项。 */
const PluginEntry kLongCapture = {
    .id      = "long_capture.main",
    .name    = "Long Capture",
    .version = "0.1.0",
    .create  = &create_state,
    .destroy = &destroy_state,
    .ifaces  = kIfaces,
};

}  // namespace long_capture

namespace {

using long_capture::PluginEntry;

const PluginEntry* const kEntries[] = {
    &long_capture::kLongCapture,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

using long_capture::PluginInstance;
using long_capture::state_of;

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

    shot::Logger(host).info(entry->id);
    if (entry == &long_capture::kLongCapture && instance->state != nullptr) {
        try {
            if (!static_cast<long_capture::LongCapture*>(instance->state)->start(host)) {
                // 会话线程创建失败时实例仍可当纯配置插件用；
                // 设置页仍能改 max_height。
                shot::Logger(host).warn("[long_capture] 插件初始化不完整，长截图功能不可用");
            }
        } catch (...) {
            // 异常绝不越过 ABI 边界：状态保留，长截图降级为不可用。
            shot::Logger(host).warn("[long_capture] 插件初始化异常，长截图功能不可用");
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

std::int32_t module_init(const ShotHostServices* host, std::uint32_t flags) {
    (void)flags;
    if (host == nullptr) {
        return 0;  // 老宿主没有服务表，仍然可以工作
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
