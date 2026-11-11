// 唯一的导出符号 + 索引目录 + ABI 胶水。
//
// 一个 DLL = 一个插件包：
//   · kEntries 的顺序就是 create(index) 的索引，必须与 metadata.json 的
//     plugins[] 顺序一一对应（两者都只有 region_selector.main 一个条目）；
//   · 覆盖层"会什么"由它自己的能力目录回答：只有 shot.ui_provider，
//     宿主用 query_interface 逐个询问，不需要知道有几种插件类型；
//   · 宿主拿到的 instance 是一个不透明指针：插件自己分配、自己释放；
//   · ABI 胶水（状态对象 + 回调 + 能力目录 + 插件身份证）也放在这个文件——它才是
//     "入口"，窗口/框选实现文件不再混 ABI 代码（STYLE.md §6.8）。
//
// 匿名命名空间里的函数都是 ABI 回调 / 模块表回调：签名由契约固定，只能是自由函数
// （STYLE.md §6.7 的合法例外）。

#include <cstdint>
#include <iterator>
#include <memory>
#include <new>

#include "plugin_api.h"
#include "plugin_entry.h"
#include "region_window.h"
#include "shot_plugin_helpers.h"

namespace {

using region_selector::PluginEntry;

const PluginEntry* const kEntries[] = {
    &region_selector::kRegionSelector,
};

constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(std::size(kEntries));

using region_selector::PluginInstance;

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

    // 用 shot::Logger 报一句自己的 id：宿主日志里能看到 region_selector.main 已被创建。
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
        return 0;  // 老宿主没有服务表，覆盖层仍可由 post_to_plugin 驱动（只是收不到总线事件）
    }
    if (SHOT_HAS_FIELD(host, ShotHostServices, abi_major) && host->abi_major != SHOT_ABI_MAJOR) {
        return 1;
    }
    // 事件总线（ABI 2.0 宿主提供）在旧宿主上可能没有：照常加载，覆盖层只是收不到
    // capture.request；由 UI 线程记 WARN 并安静降级，不在这里拒绝加载。
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
// 插件对象由插件 create / destroy，宿主只拿到一个不透明指针。
// ---------------------------------------------------------------------------
namespace region_selector {
namespace {

// ---------------------------------------------------------------------------
// 用户可见文案（覆盖层提示 / 尺寸单位 / 窗口标题）
//
// 日志一律保持中文、不进语言表：e2e 脚本按中文日志断言（"已截取覆盖层背景"等），
// 翻译了反而会让验证失败。语言表只放画给用户看的字。
//
// 两张表的 key 集必须完全一致、每个 key 两种语言都齐全：宿主按 key 查当前语言，
// 缺 key 时会走 tr() 的回退链。单张表一行会超过 120 列，所以拆成相邻字符串字面量，
// 编译期拼成一条 JSON：JSON 文本里没有裸换行（值里换行会把提示拆成多行）。
// ---------------------------------------------------------------------------
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("region.hint":"单击截取窗口 · 拖拽选择区域 · Esc / 右键取消",)json"
    R"json("overlay.size.unit":"像素",)json"
    R"json("overlay.window_title":"Plugin-shot 区域选择"})json",
    R"json({"@meta":"en-US",)json"
    R"json("region.hint":"Click a window to capture · Drag to select · Esc / right-click to cancel",)json"
    R"json("overlay.size.unit":"px",)json"
    R"json("overlay.window_title":"Plugin-shot Region Select"})json",
};

/** @brief 插件状态：一个覆盖层窗口 + 它用的多语言表。 */
struct RegionSelectorState {
    RegionWindow window;
    // 语言表放插件状态里：只有 init 拿得到宿主服务表，而覆盖层渲染线程要读文案。
    // deinit/destroy 先停窗口线程再 reset，保证渲染帧里不会用到已释放的对象。
    std::unique_ptr<shot::I18n> i18n;
};

void* create_state() {
    return new (std::nothrow) RegionSelectorState();
}

void destroy_state(void* self) {
    auto* state = static_cast<RegionSelectorState*>(self);
    if (state != nullptr) {
        try {
            state->window.stop();  // 先收窗口线程，再释放状态
            state->i18n.reset();   // 与 deinit 对称：确保没有渲染线程还在读文案
        } catch (...) {
            // 析构路径也不能让异常越过 ABI 边界；继续释放内存。
        }
        delete state;
    }
}

std::int32_t ui_init(void* self, const ShotHostServices* host) {
    // 异常绝不能越过 extern "C" 边界：start() 内部已兜住建线程，这里再兜一层
    if (auto* state = static_cast<RegionSelectorState*>(state_of(self))) {
        try {
            // 多语言表只在 init 里构造一次：此时才拿得到宿主服务表。tr() 之后只查
            // 本地表，不碰宿主，所以覆盖层可以每帧直接调用。指针交给窗口线程时
            // 必须先于 start()，否则第一帧就可能读到空指针（触发事件不保证延后）。
            if (!state->i18n) {
                state->i18n = std::make_unique<shot::I18n>(host, kRegionSelector.id);
                state->i18n->loadTables(kLangTables, std::size(kLangTables));
            }
            return state->window.start(host, state->i18n.get()) ? 0 : 1;
        } catch (...) {
            return 1;
        }
    }
    return 1;
}

std::int32_t ui_deinit(void* self) {
    // 返回前必须已退订事件总线、停线程、销毁窗口（SETTINGS.md §3.1 的硬性要求）。
    if (auto* state = static_cast<RegionSelectorState*>(state_of(self))) {
        try {
            state->window.stop();
            // 窗口线程已 join，此时再释放语言表：顺序反了会让渲染中的 tr() 悬空。
            state->i18n.reset();
        } catch (...) {
            return 1;
        }
    }
    return 0;
}

std::int32_t ui_post(void* self, const ShotEvent* ev) {
    auto* state = static_cast<RegionSelectorState*>(state_of(self));
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

const PluginEntry kRegionSelector = {
    "region_selector.main", "Region Selector", "0.1.0", &create_state, &destroy_state, kIfaces,
};

}  // namespace region_selector
