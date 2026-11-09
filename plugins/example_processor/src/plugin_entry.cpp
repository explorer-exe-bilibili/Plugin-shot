// 唯一的导出符号 + 索引目录。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的索引，必须与 metadata.json 的
//     plugins[] 顺序一一对应；
//   · 某个插件"会什么"由它自己的能力目录回答，宿主用 query_interface 逐个询问，
//     因此这里完全不需要知道有几种插件类型。
//
// 宿主拿到的 instance 是一个不透明指针：插件自己分配、自己释放。
//
// 实例创建路径还演示"运行期贡献"：example.invert 实例化时发一次 lists.add，
// 让宿主托盘多出一条示例命令——数据形状不进 ABI，见下方 publish_tray_command。

#include <atomic>
#include <cstdint>
#include <iterator>
#include <new>

#include "processors.hpp"
#include "plugin_api.h"
#include "shot_plugin_helpers.h"

namespace {

using example_processor::PluginEntry;

const PluginEntry* const kEntries[] = {
    &example_processor::kInvert,
    &example_processor::kGrayscale,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

using example_processor::PluginInstance;

// ---- 运行期贡献：给托盘加一条命令 ----
//
// 插件完全不碰托盘代码，也不新增 ABI 结构体：托盘菜单是宿主聚合出来的
// tray_command_list，插件只在实例化时发一条 lists.add 事件声明自己的条目，
// 由宿主校验、排序后给托盘渲染；点击时托盘发布 capture.request，
// 和 core 自带的「截屏」走同一条采集流水线。
//
// owner 必须等于发布者插件 id（宿主按它校验，并在插件被禁用/卸载时按它清理贡献），
// 这里的 "example.invert" 就是 invert.cpp 里 kInvert.id 的值。
const char kListsAddTopic[] = "lists.add";
const char kTrayCommandPayload[] = R"json({
    "list": "tray_command_list",
    "owner": "example.invert",
    "items": [
        {
            "id": "example.capture",
            "label": "示例：截屏（示例插件）",
            "event": "capture.request",
            "data": "",
            "order": 20
        }
    ]
})json";

// 一个 DLL 里有两个插件，但这条贡献只属于 example.invert，别发两遍：
// 用 DLL 内的 static 标记保证只发一次。DLL 重载后 static 自然归零，
// 所以重新启用插件时会重新声明；宿主在卸载时已经按 owner 清掉了旧贡献。
static std::atomic<bool> trayCommandPublished{false};

void publish_tray_command(const ShotHostServices* host) {
    bool expected = false;
    if (!trayCommandPublished.compare_exchange_strong(expected, true)) {
        return;
    }
    shot::EventBus bus(host);
    if (!bus.publish(kListsAddTopic, kTrayCommandPayload)) {
        // 老宿主没有事件总线、或队列满：菜单项没了不致命，安静降级即可。
        shot::Logger(host).warn("lists.add 发布失败（无事件总线或队列满），托盘示例命令不可用");
        return;
    }
    shot::Logger(host).info("发布 lists.add：托盘命令「示例：截屏（示例插件）」");
}

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

    // 演示"插件向宿主请求服务"：用 shot::Logger 报一句自己的 id。
    // 老宿主可能没有 log 字段，shot::Logger 内部已经做了降级，这里不用再手写判空。
    shot::Logger(host).info(entry->id);

    // 运行期贡献只属于 example.invert（同一个 DLL 里的 grayscale 不需要）。
    // 用 entry 指针判断而不是再写一遍 id 字符串。
    if (entry == &example_processor::kInvert) {
        publish_tray_command(host);
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
    // 包内只禁用 example.invert 时 DLL 不会卸载（example.grayscale 还在用同一个 DLL），
    // 光靠"DLL 重载归零"会让重新启用后的 lists.add 不再发出、托盘项回不来。
    // 所以实例销毁时也把标记清掉：宿主禁用时已按 owner 清掉旧贡献，重新 create 再声明。
    if (instance->entry == &example_processor::kInvert) {
        trayCommandPublished.store(false);
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
