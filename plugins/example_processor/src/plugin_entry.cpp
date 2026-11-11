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
// 实例创建路径还演示两件事：
//   · 多语言：create() 拿不到宿主服务表，create_instance 在拿到 host 后调
//     entry->start()，由各插件的 start 构造 shot::I18n 并发布本包共用的语言表；
//   · 运行期贡献：example.invert 实例化时发一次 lists.add，让宿主托盘多出一条
//     示例命令——数据形状不进 ABI，见下方 publish_tray_command。

#include <atomic>
#include <cstdint>
#include <iterator>
#include <new>
#include <string>

#include "processors.hpp"
#include "plugin_api.h"
#include "shot_plugin_helpers.h"

// ---------------------------------------------------------------------------
// 用户可见文案（本包两个插件共用一张语言表）
//
// invert / grayscale 各用自己的实例 id 作为 owner 各发布一次同一份表：设置页按
// (owner, key) 解析配置项的 label/tooltip，托盘按 (example.invert, example.capture)
// 解析菜单文字。把表放在包入口而不是各插件文件里，是因为它同时覆盖两个插件和
// 托盘命令，分开写三份必然漂移。
//
// 日志一律保持中文、不进语言表（e2e 按中文日志断言）。key 规范：
//   · 配置项：config.<配置 key>.label / config.<配置 key>.tooltip；
//   · 托盘命令：直接用命令 id 当 key。
// 单张表一行远超 120 列，所以拆成相邻 raw 字面量：编译期拼成一条合法 JSON，
// JSON 文本里没有裸换行（值里有换行会把文案拆成多行，也不再是合法 JSON）。
// ---------------------------------------------------------------------------
namespace example_processor {
namespace {

const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("config.keep_alpha.label":"保留 alpha",)json"
    R"json("config.keep_alpha.tooltip":"取反时跳过 A 通道",)json"
    R"json("config.mode.label":"模式",)json"
    R"json("config.mode.tooltip":"灰度算法",)json"
    R"json("config.strength.label":"强度",)json"
    R"json("config.strength.tooltip":"0 = 原图，100 = 完全灰度",)json"
    R"json("example.capture":"示例：截屏（示例插件）"})json",
    R"json({"@meta":"en-US",)json"
    R"json("config.keep_alpha.label":"Keep alpha",)json"
    R"json("config.keep_alpha.tooltip":"Skip the alpha channel when inverting",)json"
    R"json("config.mode.label":"Mode",)json"
    R"json("config.mode.tooltip":"Grayscale algorithm",)json"
    R"json("config.strength.label":"Strength",)json"
    R"json("config.strength.tooltip":"0 = original image, 100 = fully grayscale",)json"
    R"json("example.capture":"Example: Capture (sample plugin)"})json",
};

}  // namespace

bool load_language_tables(shot::I18n& i18n) {
    return i18n.loadTables(kLangTables, std::size(kLangTables));
}

}  // namespace example_processor

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
//
// label 写命令 id 作为文案 key：托盘渲染时按 display(owner, label) 到 i18n_list
// 解析（document/I18N.md §2.3）。直接写中文会把语言切换锁死，老宿主查不到时
// 原样显示 key 也是协议允许的降级。
const char kListsAddTopic[] = "lists.add";
const char kTrayCommandPayload[] = R"json({
    "list": "tray_command_list",
    "owner": "example.invert",
    "items": [
        {
            "id": "example.capture",
            "label": "example.capture",
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
        shot::Logger(host).warn(
            "lists.add 发布失败（无事件总线或队列满），托盘示例命令不可用");
        return;
    }
    shot::Logger(host).info("发布 lists.add：托盘命令 example.capture（文案 key 由 i18n_list 解析）");
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

    // 语言表要在宿主读 config_list / tray_command_list 之前发布：create() 拿不到
    // host，这里是第一个能拿到服务表的生命周期路径。失败只降级（界面显示 key），
    // 不阻止插件加载；异常也绝不能穿过 C 导出边界。
    if (entry->start != nullptr && instance->state != nullptr) {
        try {
            if (!entry->start(instance->state, host)) {
                shot::Logger(host).warn(std::string(entry->id) + " 语言表加载失败，界面文案回退为 key");
            }
        } catch (...) {
            shot::Logger(host).warn(std::string(entry->id) +
                                    " 初始化多语言时异常，界面文案回退为 key");
        }
    }

    // 运行期贡献只属于 example.invert（同一个 DLL 里的 grayscale 不需要）。
    // 用 entry 指针判断而不是再写一遍 id 字符串。放在语言表之后，让托盘第一次
    // 解析 label 时 i18n_source_list 里已经有对应文本。
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
    // 所以实例销毁时也把标记清掉：宿主禁用时已按 owner 清掉旧贡献，
    // 重新 create 再声明。
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
