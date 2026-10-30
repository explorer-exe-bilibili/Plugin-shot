#pragma once

#include <cstdint>

#include "plugin_api_export.h"

// 宿主与插件之间的 ABI 契约版本。任何对 Image / IImageProcessor / 三个导出函数
// 的布局或语义改动都必须递增；宿主读到不匹配的版本会立刻卸载该插件。
//   1 -> Image 无 channels 字段，plugin_abi_version 返回 uint32_t
//   2 -> Image 追加 channels（BGRA 语义显式化），plugin_abi_version 改为返回 int
#define PLUGIN_ABI_VERSION 2

// ============================================================================
//  二进制兼容前提（务必遵守，否则是未定义行为）
//  1) 跨边界传递的是 C++ vtable（IImageProcessor 的虚函数表），不是纯 C 结构体。
//  2) 宿主与插件必须由同一个编译器、同一版本 CRT、同一 Debug/Release 配置、同一
//     C++ 标准构建。Windows 上即同为 MSVC + /MD（或同为 /MDd）；GCC/Clang 之间
//     混编会因 vtable 布局与异常模型差异导致崩溃。
//  3) 插件对象由插件自己的堆分配，也只能由插件自己的 plugin_destroy_processor
//     销毁 —— 宿主侧绝不 delete。
//  4) 不要把 STL 容器 / std::string 放进跨边界接口，除非确认两边 CRT 一致。
// ============================================================================

// 8 位像素缓冲。
//  · width/height 是像素尺寸；channels 是每像素通道数（截图场景恒为 4，即 BGRA）。
//  · stride 以字节计：可以为负（自底向上存储），其绝对值也可以大于 width*channels
//    （每行尾部有填充）。因此必须按行寻址，不能假设 data 是连续块。
//  · 可用字节数至少为 (height-1)*|stride| + width*channels。
struct Image {
    std::uint8_t* data;
    int width;
    int height;
    int stride;
    int channels;   // 追加在末尾：不移动既有字段偏移，渐进升级更安全
};

class IImageProcessor {
public:
    virtual ~IImageProcessor() = default;

    // 返回静态存储期的名字，宿主不负责释放。
    virtual const char* name() const = 0;

    // 原地处理。允许抛异常，宿主会逐插件隔离。
    virtual void process(Image& image) = 0;
};

extern "C" {
SCREENSHOT_API int plugin_abi_version(void);
SCREENSHOT_API IImageProcessor* plugin_create_processor(void);
SCREENSHOT_API void plugin_destroy_processor(IImageProcessor* processor);
}
