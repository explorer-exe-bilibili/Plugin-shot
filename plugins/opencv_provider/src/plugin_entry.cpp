// opencv_provider —— 运行库提供者插件
//
// 它不提供任何算法，只负责把 OpenCV 带进本进程，为此实现能力 "shot.lib_provider"：
// 宿主据此把它登记成 PluginType::Lib（LibProvider），而不是一个坏掉的图像处理器。
//
// 想再叠加能力（例如把 OpenCV 的某个接口开放给别的插件）时，做法是新增一个能力 id
// 并在 query_interface 里多回答一个分支 —— 纯新增数据，不需要动 ABI 版本。

#include <cstdint>
#include <new>
#include <string>

#ifdef OPENCV_PROVIDER_HAS_OPENCV
    #include <opencv2/core.hpp>
#endif

#include "plugin_api.h"

namespace {

const char kPluginId[] = "opencv.provider";
const char kPluginName[] = "OpenCV Environment Provider";
const char kPluginVersion[] = "1.0.0";
const char kLibraryName[] = "opencv";

// 构建期没找到 OpenCV 时，本插件依旧合法，只是报不出库版本。
std::string opencv_version() {
#ifdef OPENCV_PROVIDER_HAS_OPENCV
    return cv::getVersionString();
#else
    return "unknown";
#endif
}

std::int32_t opencv_provider_init(const ShotHostServices* host, std::uint32_t flags) {
    (void)flags;
    if (host != nullptr && SHOT_HAS_FIELD(host, ShotHostServices, log) && host->log != nullptr) {
        const std::string text = "[OpenCVProvider] initialized, opencv " + opencv_version();
        host->log(host->host_context, SHOT_LOG_INFO, text.c_str(),
                  static_cast<std::int32_t>(text.size()));
    }
    return 0;
}

// 实例不需要真的持有东西，但必须是真实分配出来的指针：
// 宿主不会解引用它，可 destroy 会拿它来释放。
void* opencv_provider_create(std::uint32_t index, const ShotHostServices* /*host*/) {
    if (index != 0) {
        return nullptr;
    }
    return new (std::nothrow) int(0);
}

void opencv_provider_destroy(void* instance) { delete static_cast<int*>(instance); }

// ---- 能力：shot.lib_provider ----------------------------------------------

const char* lib_get_name(void* /*self*/) { return kLibraryName; }

// 返回插件内的静态字符串（有效期到 DLL 卸载），宿主只读不释放。
const char* lib_get_version(void* /*self*/) {
    static const std::string version = opencv_version();
    return version.c_str();
}

void* opencv_provider_query(void* /*instance*/, const char* iface_id, std::uint32_t version) {
    if (iface_id == nullptr) {
        return nullptr;
    }
    if (std::string(iface_id) == SHOT_IFACE_LIB_PROVIDER && version <= 1) {
        static const ShotLibProviderV1 ops = {
            .struct_size = sizeof(ShotLibProviderV1),
            .version = 1,
            .get_library_name = &lib_get_name,
            .get_library_version = &lib_get_version,
        };
        return const_cast<ShotLibProviderV1*>(&ops);
    }
    return nullptr;  // 其它能力一律没有，宿主安静跳过
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

const ShotModule g_module = {
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

SCREENSHOT_API const ShotModule* shot_module_entry(void) { return &g_module; }

}  // extern "C"
