// 唯一的导出符号 + 索引目录 + ABI 胶水。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的索引，必须与 metadata.json 的
//     plugins[] 顺序一一对应；
//   · 某个插件"会什么"由它自己的能力目录回答，宿主用 query_interface 逐个询问；
//   · 宿主拿到的 instance 是一个不透明指针：插件自己分配、自己释放；
//   · 能力函数表（采集 + 配置）与插件身份证都放在这个文件——它才是"入口"，
//     采集实现文件不混 ABI 代码（STYLE.md §6.8）；实例状态类型因为两个能力
//     回调都要用，放内部头 screen_capture_state.h。
//
// 匿名命名空间里的函数都是 ABI 回调 / 模块表回调：签名由契约固定，只能是自由函数
// （STYLE.md §6.7 的合法例外）。

#include <cstdint>
#include <iterator>
#include <new>

#include "capture_settings.h"
#include "plugin_api.h"
#include "plugin_entry.h"
#include "screen_capture.h"
#include "screen_capture_state.h"
#include "shot_plugin_helpers.h"

namespace {

using screen_capture::PluginEntry;

const PluginEntry* const kEntries[] = {
    &screen_capture::kScreenCapture,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

using screen_capture::PluginInstance;

// 模块级宿主服务表。PluginEntry::create 的签名没有 host 参数（能力插件模板就是这样），
// 而截图失败时要写日志，所以在 module_init 里存一份给 create 用。宿主保证 init 先于
// create（src/PluginManager.cpp 的加载流程）；老宿主不调 init 时保持 nullptr，
// shot::Logger 会静默降级。
const ShotHostServices* g_hostServices = nullptr;

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

// 模块级初始化：拿宿主版本做一次协商，并把服务表存给 create 用。
// 返回非 0 = "这个 DLL 无法在本宿主上工作"，宿主会直接卸载它。
std::int32_t module_init(const ShotHostServices* host, std::uint32_t flags) {
    (void)flags;
    if (host == nullptr) {
        return 0;  // 老宿主没有服务表，仍然可以工作：截图照常，只是写不了日志
    }
    if (SHOT_HAS_FIELD(host, ShotHostServices, abi_major) && host->abi_major != SHOT_ABI_MAJOR) {
        return 1;
    }
    g_hostServices = host;
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
// ABI 胶水：插件状态对象 + 两个能力回调 + 能力目录 + 静态插件描述。
// 插件对象由插件 create / destroy，宿主只拿到一个不透明指针。
// ---------------------------------------------------------------------------
namespace screen_capture {
namespace {

void* create_state() { return new (std::nothrow) ScreenCaptureState(g_hostServices); }

void destroy_state(void* self) { delete static_cast<ScreenCaptureState*>(self); }

std::int32_t describe_screen(void* self, ShotCaptureInfo* out) {
    auto* state = static_cast<ScreenCaptureState*>(state_of(self));
    if (state == nullptr) {
        return 2;
    }
    try {
        return state->capture.describe(out);
    } catch (...) {
        // 异常绝不越过 DLL 边界（ABI 政策 §2.6）。describe 目前没有会抛的调用，
        // 这里只是边界兜底，不让未来改动把宿主拖崩。
        return 3;
    }
}

std::int32_t capture_screen(void* self, ShotImage* dst) {
    auto* state = static_cast<ScreenCaptureState*>(state_of(self));
    if (state == nullptr) {
        return 2;
    }
    try {
        // 配置快照在这里读一次：抓屏期间用户再改 include_cursor 也只影响下一张图。
        return state->capture.capture(dst, state->settings.includeCursor());
    } catch (...) {
        // capture 里的分配用 new (std::nothrow)，理论上也不抛；边界兜底仍然要有。
        return 3;
    }
}

const ShotScreenCaptureV1 kScreenOps = SHOT_OPS(ShotScreenCaptureV1,
                                                .describe = &describe_screen,
                                                .capture  = &capture_screen);

// 配置能力：声明 include_cursor，宿主据此生成设置页条目并在启动时回灌已存值。
// 回调实现与配置描述在 capture_settings.cpp（一个能力一个 .cpp）。
const ShotConfigurableV1 kConfigOps = SHOT_OPS(ShotConfigurableV1,
                                               .get_config_count = &capture_config_count,
                                               .get_config_desc  = &capture_config_desc,
                                               .get_config       = &capture_config_get,
                                               .set_config       = &capture_config_set);

const ShotIfaceEntry kIfaces[] = {
    { SHOT_IFACE_SCREEN_CAPTURE, 1, &kScreenOps },
    { SHOT_IFACE_CONFIGURABLE, 1, &kConfigOps },
    { nullptr, 0, nullptr },
};

}  // namespace

const PluginEntry kScreenCapture = {
    "screen_capture.main", "Screen Capture", "0.2.0", &create_state, &destroy_state, kIfaces,
};

}  // namespace screen_capture
