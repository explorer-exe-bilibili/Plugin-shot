#pragma once

#include <stdint.h>

// ============================================================================
//  图像：处理类插件的数据契约
// ============================================================================

/**
 * @brief 像素格式常量：8 位 4 通道 BGRA，内存顺序为 B、G、R、A。
 *
 * pixel_format 显式声明像素布局，不再靠 channels 猜；当前只定义这一种，
 * 其它值一律视为不支持。
 *
 * @note 用定宽常量而不是 C 枚举：枚举的宽度由编译器决定，不能进 ABI。
 */
#define SHOT_PIXEL_FORMAT_BGRA8 1u

/**
 * @brief 一张待处理图像：8 位像素缓冲 + 显式声明的布局。
 *
 * data 不保证连续：必须按行寻址，用 stride 算行首，不能假设行尾没有填充。
 * stride 为负表示自底向上存储（data 指向最后一行）；其绝对值可以大于
 * width * channels。可用字节数至少为 (height-1)*|stride| + width*channels，
 * 越界写就是踩坏宿主内存。
 *
 * 老宿主的 ShotImage 里可能还没有 pixel_format 字段（那之前只能靠 channels 猜），
 * 读它之前先过 SHOT_HAS_FIELD。
 *
 * @note 原地处理，故意非 const：插件只能改像素，不得改结构体字段。
 * @warning 宿主与插件在同一进程、没有隔离，越界写会直接破坏宿主。
 */
typedef struct ShotImage {
    uint32_t struct_size;   /**< = sizeof(ShotImage)；构造方填，读取方先比对 */
    uint32_t pixel_format;  /**< SHOT_PIXEL_FORMAT_*；老结构里可能没有这个字段 */
    uint8_t* data;          /**< 图像首行第一个像素；行序由 stride 的正负决定，不能为 NULL */
    int32_t  width;         /**< 以像素计；图像有效时 >= 1 */
    int32_t  height;        /**< 以像素计；图像有效时 >= 1 */
    int32_t  stride;        /**< 行间距，字节为单位，可负；每行前 width*channels 字节有效，其余是填充 */
    int32_t  channels;      /**< 每像素通道数；BGRA8 为 4 */
} ShotImage;
