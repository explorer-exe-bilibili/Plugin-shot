// opencv_provider —— 运行库提供者插件
//
// 它不提供任何算法，只负责把 OpenCV 带进本进程，为此实现能力 "shot.lib_provider"：
// 宿主据此把它登记成 PluginType::Lib（LibProvider），而不是一个坏掉的图像处理器。
//
// 想再叠加能力（例如把 OpenCV 的某个接口开放给别的插件）时，做法是新增一个能力 id
// 并在 kIfaces 里加一项 —— 纯新增数据，不需要动 ABI 版本。

#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <string>

#ifdef OPENCV_PROVIDER_HAS_OPENCV
    #include <opencv2/core.hpp>
#endif

#include "plugin_api.h"
#include "shot_plugin_helpers.h"

namespace {

const char kPluginId[] = "opencv.provider";
const char kPluginName[] = "OpenCV Environment Provider";
const char kPluginVersion[] = "1.0.0";
const char kLibraryName[] = "opencv";

// ---- 多语言：插件显示名/描述 ----------------------------------------------
//
// 本插件没有配置项也没有自绘 UI，但设置页按 (owner=插件 id, "plugin.name") 解析
// 插件分区的显示名，所以这里按 shot::I18n 的约定发布一张最小的语言表到
// i18n_source_list（owner 就是下面的 kPluginId，条目不需要 target）。
// key 只有 plugin.name / plugin.description；每语言一张 JSON 对象、@meta 必填，
// 两张表的 key 集必须一致。日志与库名不进表（库名是技术值，日志保持中文）。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"OpenCV 提供者",)json"
    R"json("plugin.description":"把 OpenCV 带进进程的运行库提供者，不提供任何算法"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"OpenCV Environment Provider",)json"
    R"json("plugin.description":"Library provider that loads OpenCV into the process; it offers no algorithms"})json",
};

/**
 * @brief 实例状态：只持有语言表（本插件不缓存别的对象）。
 *
 * I18n 不可拷贝/移动，用 unique_ptr 持有；destroy 里先析构它（会退订
 * lists.changed），再释放状态本身。
 */
struct ProviderState {
    /** @brief 插件自身文案；构造失败时为 nullptr，显示名回退成 key。 */
    std::unique_ptr<shot::I18n> i18n;
};

// 构建期没找到 OpenCV 时，本插件依旧合法，只是报不出库版本。
std::string opencv_version() {
#ifdef OPENCV_PROVIDER_HAS_OPENCV
    return cv::getVersionString();
#else
    return "unknown";
#endif
}

// 模块级初始化：先做 ABI 协商，再说一句自己装好了。
// 返回非 0 = "这个 DLL 无法在本宿主上工作"，宿主会直接卸载它。
std::int32_t opencv_provider_init(const ShotHostServices* host, std::uint32_t flags) {
    (void)flags;
    if (host == nullptr) {
        return 0;  // 老宿主没有服务表，仍然可以工作
    }
    if (SHOT_HAS_FIELD(host, ShotHostServices, abi_major) && host->abi_major != SHOT_ABI_MAJOR) {
        return 1;
    }
    // shot::Logger 内部处理了老宿主没有 log 字段的情况，这里直接写日志即可。
    shot::Logger(host).info("[OpenCVProvider] initialized, opencv " + opencv_version());
    return 0;
}

// 实例不需要真的持有能力对象，但必须是真实分配出来的指针：
// 宿主不会解引用它，可 destroy 会拿它来释放。
// 语言表在这里发布（create 是唯一拿得到宿主服务表的实例路径），异常绝不能
// 穿过 C 导出边界；文案失败只降级成 key，不影响 lib_provider 能力。
void* opencv_provider_create(std::uint32_t index, const ShotHostServices* host) {
    if (index != 0) {
        return nullptr;
    }
    auto state = std::make_unique<ProviderState>();
    try {
        state->i18n = std::make_unique<shot::I18n>(host, kPluginId);
        if (!state->i18n->loadTables(kLangTables, std::size(kLangTables))) {
            shot::Logger(host).warn("[OpenCVProvider] 语言表解析失败，显示名回退为 key");
        }
    } catch (...) {
        state->i18n.reset();
        shot::Logger(host).warn("[OpenCVProvider] 初始化多语言失败，显示名回退为 key");
    }
    return state.release();
}

void opencv_provider_destroy(void* instance) { delete static_cast<ProviderState*>(instance); }

// ---- 能力：shot.lib_provider ----------------------------------------------

const char* lib_get_name(void* /*self*/) { return kLibraryName; }

// 返回插件内的静态字符串（有效期到 DLL 卸载），宿主只读不释放。
const char* lib_get_version(void* /*self*/) {
    static const std::string version = opencv_version();
    return version.c_str();
}

// SHOT_OPS 负责填 struct_size / version；字段顺序必须与结构体声明一致。
const ShotLibProviderV1 kLibOps = SHOT_OPS(ShotLibProviderV1,
                                           .get_library_name    = &lib_get_name,
                                           .get_library_version = &lib_get_version);

// 能力目录：以 { nullptr, 0, nullptr } 结尾，shot_query_interface 靠它停下。
const ShotIfaceEntry kIfaces[] = {
    {SHOT_IFACE_LIB_PROVIDER, 1, &kLibOps},
    {nullptr, 0, nullptr},
};

// 没有的能力一律返回 NULL，宿主安静跳过。
void* opencv_provider_query(void* /*instance*/, const char* iface_id, std::uint32_t version) {
    return shot_query_interface(kIfaces, iface_id, version);
}

std::int32_t opencv_provider_info(std::uint32_t index, ShotPluginInfo* out) {
    if (out == nullptr || index != 0) {
        return 1;
    }
    if (out->struct_size < SHOT_REQUIRES(ShotPluginInfo, version)) {
        return 2;
    }
    out->abi_minor = SHOT_ABI_MINOR;
    out->id = kPluginId;
    out->name = kPluginName;
    out->version = kPluginVersion;
    return 0;
}

const ShotModule kModule = {
    .struct_size     = sizeof(ShotModule),
    .abi_major       = SHOT_ABI_MAJOR,
    .abi_minor       = SHOT_ABI_MINOR,
    .plugin_count    = 1,
    .init            = &opencv_provider_init,
    .shutdown        = nullptr,
    .get_info        = &opencv_provider_info,
    .create          = &opencv_provider_create,
    .destroy         = &opencv_provider_destroy,
    .query_interface = &opencv_provider_query,
};

}  // namespace

extern "C" {

/**
 * @brief 唯一导出符号，返回静态模块表。
 */
SCREENSHOT_API const ShotModule* shot_module_entry(void) { return &kModule; }

}  // extern "C"
