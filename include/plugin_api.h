#pragma once

#include <cstdint>

#include "plugin_api_export.h"

// ============================================================================
//  宿主与插件之间的 ABI 契约版本。
//
//  契约语义变化（导出符号集、结构布局、调用约定）时必须递增；宿主读到不匹配的
//  版本会立刻拒绝并卸载该插件。当前仍是 1：本项目还没有第三方二进制发行
//  （唯一开发者），所以本轮把工厂从单函数改成了索引工厂但没有递增。
//  **发布第一个公开 SDK 之前如果再动契约，必须递增到 2。**
// ============================================================================
#define PLUGIN_ABI_VERSION 1

// ============================================================================
//  插件元数据尾部标记
//
//  构建期由 cmake/AppendPluginMeta.cmake 把 metadata.json 贴在 DLL/SO 末尾：
//
//      <原始二进制> \n---PLUGIN_META_START---\n {json} \n---PLUGIN_META_END---\n
//
//  PE/ELF 加载器只读文件头的节表，文件尾的这段额外数据被完全忽略，因此追加不影响
//  装载。元数据跟着文件走，用户复制/重命名插件也不会丢。
//  这两个字面量是构建脚本（构建期直接读本头的宏值）与日后宿主读取逻辑的唯一约定。
// ============================================================================
#define PLUGIN_META_START_MARKER "---PLUGIN_META_START---"
#define PLUGIN_META_END_MARKER   "---PLUGIN_META_END---"

// ============================================================================
//  二进制兼容前提（务必遵守，否则是未定义行为）
//  1) 跨边界传递的是 C++ vtable（CPluginHandle 里的函数指针），不是纯 C 结构体。
//  2) 宿主与插件必须由同一个编译器、同一版本 CRT、同一 Debug/Release 配置、同一
//     C++ 标准构建。Windows 上即同为 MSVC + /MD（或同为 /MDd）；GCC/Clang 之间
//     混编会因 vtable 布局与异常模型差异导致崩溃。
//  3) 插件对象由插件自己的堆分配，也只能由插件自己的 plugin_destroy 销毁 ——
//     宿主侧绝不 delete。
//  4) 不要把 STL 容器 / std::string 放进跨边界接口，除非确认两边 CRT 一致。
// ============================================================================

// 8 位像素缓冲。
//  · width/height 是像素尺寸；channels 是每像素通道数（截图场景恒为 4，即 BGRA）。
//  · stride 以字节计：可以为负（自底向上存储），其绝对值也可以大于 width*channels
//    （每行尾部有填充）。因此必须按行寻址，不能假设 data 是连续块。
//  · 可用字节数至少为 (height-1)*|stride| + width*channels。
enum PluginType {
    Window,
    Tray,
    ImageProcessor
};
struct Image {
    std::uint8_t* data;
    int width;
    int height;
    int stride;
    int channels;
};

typedef struct{
    int type;
    const char* name;
    void* internal_data;

    int (*process)(void* internal_data, Image* image);
}CPluginHandle;

// ============================================================================
//  导出契约：一个动态库（插件包）导出下面 4 个 C 函数。
//
//  一个 DLL 可以包含多个插件，宿主只按索引创建，不需要提前知道插件清单：
//  metadata.json 的 plugins[] 顺序必须与 plugin_create(index) 的索引一一对应。
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif
    // 1. 契约版本。宿主第一件事就是拿它和 PLUGIN_ABI_VERSION 比对。
    SCREENSHOT_API int plugin_abi_version(void);

    // 2. 本模块里有几个插件。返回 <= 0 表示这个模块没有可用插件。
    SCREENSHOT_API int plugin_get_count(void);

    // 3. 创建第 index 个插件；越界或分配失败返回 nullptr。
    SCREENSHOT_API CPluginHandle* plugin_create(int index);

    // 4. 销毁句柄。只能由插件自己的堆释放 —— 宿主绝不 delete。
    SCREENSHOT_API void plugin_destroy(CPluginHandle* handle);
#ifdef __cplusplus
}
#endif
