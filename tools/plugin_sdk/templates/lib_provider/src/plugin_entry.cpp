// 运行库提供者模板：最小可编译骨架（唯一导出符号 + shot.lib_provider + 模块表）。
//
// 这个包不提供宿主可调用的算法，只负责把某个运行库带进进程，并回答"库名 / 版本"。
// 宿主见到 shot.lib_provider 就按 PluginType::Lib 登记，不会把它当成坏掉的处理器；
// 其它插件在 metadata.requires 里写 provides 的名字即可依赖本包（提供者先加载）。
//
// 真正带库的插件通常比这个模板多两件事：
//   · CMakeLists 的 shot_add_plugin() 里加 LINK_LIBRARIES（见该文件注释）；
//   · 创建实例时初始化库（例如 OpenCV 不需要初始化，但某些库需要）。
// 库的运行时 DLL 属于宿主的环境，不要塞进 plugins/ 目录（那里只放插件自己的 .dll）。
//
// 本文件不包含 shot_plugin_helpers.h 汇总头，只取两个不需要 nlohmann-json 的
// 子头：lib_provider 插件连 JSON 都用不到，保持最小依赖。

#include <cstdint>
#include <iterator>
#include <new>
#include <string>

#include "lib_provider.h"
#include "plugin_api.h"
#include "shot_plugin_helpers/shot_iface_entry.h"
#include "shot_plugin_helpers/shot_logger.h"

namespace {

/** @brief 插件实例状态；本模板没有状态，留一个空结构让 create/destroy 对称。 */
struct ProviderState {
    int reserved = 0; /**< 预留：将来放真实库的句柄/初始化标记。 */
};

/** @brief 分配插件状态。 */
void* create_state() {
    return new (std::nothrow) ProviderState();
}

/** @brief 释放插件状态。 */
void destroy_state(void* self) {
    delete static_cast<ProviderState*>(self);
}

// ---- shot.lib_provider：回答库名与运行期版本 ----

/** @brief 返回库名；字符串必须是静态存储、活到 DLL 卸载。 */
const char* get_library_name(void* /*self*/) {
    return lib_provider_template::kLibraryName;
}

/** @brief 返回库版本；真实插件应取自库的版本头/宏，不要手写。 */
const char* get_library_version(void* /*self*/) {
    return lib_provider_template::kLibraryVersion;
}

const ShotLibProviderV1 kProviderOps = SHOT_OPS(ShotLibProviderV1,
                                                .get_library_name    = &get_library_name,
                                                .get_library_version = &get_library_version);

// 能力目录：宿主按 id + version 查询；必须 {nullptr, 0, nullptr} 结尾。
const ShotIfaceEntry kIfaces[] = {
    {SHOT_IFACE_LIB_PROVIDER, 1, &kProviderOps},
    {nullptr, 0, nullptr},
};

/**
 * @brief 创建第 index 个插件实例；宿主拿到非空指针后才会调 query_interface。
 * @param[in] index 0..plugin_count-1（本包只有一个插件，只能是 0）。
 * @param[in] host  宿主服务表（create 是唯一能拿到它的时机）。
 * @return 不透明实例指针；失败返回 nullptr。
 */
void* create_instance(std::uint32_t index, const ShotHostServices* host) {
    if (index != 0) {
        return nullptr;
    }
    void* state = create_state();
    if (state == nullptr) {
        return nullptr;
    }
    shot::Logger(host).info(std::string("{{PLUGIN_ID}}：提供 ") + lib_provider_template::kLibraryName +
                            " " + lib_provider_template::kLibraryVersion);
    return state;
}

/** @brief 按能力 id 查询函数表；没有这个能力返回 nullptr（宿主安静跳过）。 */
void* query_instance(void* self, const char* iface_id, std::uint32_t version) {
    if (self == nullptr) {
        return nullptr;
    }
    return shot_query_interface(kIfaces, iface_id, version);
}

/** @brief 填插件身份证；宿主不创建实例也能列清单。 */
std::int32_t get_info(std::uint32_t index, ShotPluginInfo* out) {
    if (out == nullptr || index != 0) {
        return 1;
    }
    if (out->struct_size < SHOT_REQUIRES(ShotPluginInfo, version)) {
        return 2;
    }
    out->abi_minor = SHOT_ABI_MINOR;
    out->id = "{{PLUGIN_ID}}";
    out->name = "{{PLUGIN_NAME}}";
    out->version = "{{VERSION}}";
    return 0;
}

/** @brief 模块级初始化：拿宿主 ABI 版本做一次协商。 */
std::int32_t module_init(const ShotHostServices* host, std::uint32_t flags) {
    (void)flags;
    if (host == nullptr) {
        return 0;
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
    .plugin_count    = 1,  // 本 DLL 只有 lib_provider 一个插件
    .init            = &module_init,
    .shutdown        = nullptr,  // 没有模块级资源要收
    .get_info        = &get_info,
    .create          = &create_instance,
    .destroy         = &destroy_state,
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
