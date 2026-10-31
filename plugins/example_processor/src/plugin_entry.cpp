// 导出契约的四个 C 函数（见 include/plugin_api.h）。
//
// 一个 DLL = 一个插件包：kEntries 的顺序就是 plugin_create(index) 的索引，
// 必须与 metadata.json 的 plugins[] 顺序一一对应。

#include <iterator>
#include <new>

#include "example_processor/processors.hpp"
#include "plugin_api.h"

namespace {

struct Entry {
    PluginType type;
    const char* name;
    int (*process)(void* internal_data, Image* image);
};

constexpr Entry kEntries[] = {
    {::ImageProcessor, "example.invert", &example_processor::invert_bgra},
    {::ImageProcessor, "example.grayscale", &example_processor::grayscale_bgra},
};

constexpr int kEntryCount = static_cast<int>(std::size(kEntries));

}  // namespace

extern "C" {

SCREENSHOT_API int plugin_abi_version(void) { return PLUGIN_ABI_VERSION; }

SCREENSHOT_API int plugin_get_count(void) { return kEntryCount; }

SCREENSHOT_API CPluginHandle* plugin_create(int index) {
    if (index < 0 || index >= kEntryCount) {
        return nullptr;
    }

    auto* handle = new (std::nothrow) CPluginHandle{};
    if (handle == nullptr) {
        return nullptr;
    }

    const Entry& entry = kEntries[index];
    handle->type = entry.type;
    handle->name = entry.name;
    handle->internal_data = nullptr;
    handle->process = entry.process;
    return handle;
}

SCREENSHOT_API void plugin_destroy(CPluginHandle* handle) { delete handle; }

}  // extern "C"
