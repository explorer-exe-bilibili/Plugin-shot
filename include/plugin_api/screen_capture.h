#pragma once

#include <stdint.h>
#include "plugin_api/image.h"

// ============================================================================
//  能力：屏幕采集  "shot.screen_capture"
// ============================================================================

/**
 * @brief 采集目标的几何与像素格式：宿主据此分配目标缓冲。
 *
 * v1 只描述"整个虚拟桌面"：多显示器时 width/height 是合成后的像素总尺寸，
 * origin_x/origin_y 是左上角在系统坐标系里的位置（主屏左上角不一定是 (0,0)）。
 * 区域采集、单显示器采集留给后续版本（新增字段或新结构体，老字段不动）。
 *
 * @note 字段只在 describe 成功后有效；宿主不应对失败时的内容做任何假设。
 */
typedef struct ShotCaptureInfo {
    uint32_t struct_size;   /**< = sizeof(ShotCaptureInfo)；构造方填，读取方先比对 */
    int32_t  width;         /**< 采集结果像素宽；有效时 >= 1 */
    int32_t  height;        /**< 采集结果像素高；有效时 >= 1 */
    int32_t  origin_x;      /**< 内容左上角在系统坐标里的 x；多显示器时可能为负 */
    int32_t  origin_y;      /**< 内容左上角在系统坐标里的 y；多显示器时可能为负 */
    uint32_t pixel_format;  /**< 结果的像素格式，目前必须是 SHOT_PIXEL_FORMAT_BGRA8 */
} ShotCaptureInfo;

/**
 * @brief 屏幕采集能力函数表，对应 SHOT_IFACE_SCREEN_CAPTURE、版本 1。
 *
 * 两段式：宿主先 describe 拿到尺寸，自己分配一块 BGRA8 缓冲并填好 ShotImage，
 * 再调 capture 让插件往里写像素。插件因此**从不分配图像内存**，也就没有
 * "谁分配谁释放"的跨 DLL 所有权问题；capture 返回后插件不得保存 dst 或其
 * data 指针——下一张图宿主可能换一块完全不同的内存。
 *
 * 线程模型：两个函数都只在宿主调用线程上执行，实现内部若用系统 API 需要自己
 * 保证该线程可用（Windows 上 GDI 抓屏不要求 UI 线程）。
 *
 * 返回码：0 = 成功；1 = 当前没有可采集的屏幕（无显示器/远程会话断开）；
 * 2 = 参数不合法（out/dst 为空、pixel_format 不认识、缓冲尺寸不足）；
 * 3 = 系统调用失败（实现应已写日志）。非 0 时宿主不会使用 dst 的内容。
 *
 * @note 两个函数指针都必须实现；缺一个宿主按"不支持屏幕采集"处理，跳过该插件。
 */
typedef struct ShotScreenCaptureV1 {
    uint32_t struct_size;   /**< = sizeof(ShotScreenCaptureV1)；构造方填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */
    /**
     * @brief 报告采集目标的几何与像素格式。
     * @param[in]  self 插件实例指针。
     * @param[out] out  由调用方提供并填好 struct_size；插件据此判断能写哪些字段。
     * @return 0 = 成功；非 0 见上面的返回码约定。
     */
    int32_t (*describe)(void* self, ShotCaptureInfo* out);
    /**
     * @brief 把屏幕像素写进宿主提供的图像。
     * @param[in] self 插件实例指针。
     * @param[in,out] dst 宿主填好 data/width/height/stride/channels/pixel_format 的图像；
     *                 插件只能写像素，不得改结构体字段。必须按 stride 逐行写，
     *                 stride 为负表示自底向上存储。
     * @return 0 = 成功；非 0 见上面的返回码约定。
     */
    int32_t (*capture)(void* self, ShotImage* dst);
} ShotScreenCaptureV1;
