#pragma once

#include <stdint.h>

// ============================================================================
//  能力：运行库提供者  "shot.lib_provider"
// ============================================================================

/**
 * @brief 运行库提供者能力函数表，对应 SHOT_IFACE_LIB_PROVIDER、版本 1。
 *
 * 有些包不提供宿主可调用的算法，只负责把某个运行库带进进程（典型：OpenCV）。
 * 它用这个能力表明自己是"库/环境提供者"，而不是一个坏掉的图像处理器；
 * 宿主见到它就按 PluginType::Lib 登记，名字与版本用于界面显示和依赖校验。
 *
 * @note 两个 getter 返回的都是插件内的静态字符串，有效期到 DLL 卸载；宿主只读，
 *       不释放。追加能力时在这个结构体末尾加字段即可（abi_minor 按需递增）。
 */
typedef struct ShotLibProviderV1 {
    uint32_t struct_size;                           /**< = sizeof(ShotLibProviderV1)；构造方填 */
    uint32_t version;                               /**< 函数表版本，当前为 1 */
    const char* (*get_library_name)(void* self);    /**< 返回库名，如 "opencv"；不能为 NULL */
    const char* (*get_library_version)(void* self); /**< 返回库版本，如 "4.12.0"；不能为 NULL */
} ShotLibProviderV1;
