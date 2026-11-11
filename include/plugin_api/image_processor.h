#pragma once

#include <stdint.h>
#include "plugin_api/image.h"

// ============================================================================
//  能力：图像处理器  "shot.image_processor"
// ============================================================================

/**
 * @brief 图像处理器能力函数表，对应 SHOT_IFACE_IMAGE_PROCESSOR、版本 1。
 *
 * process 返回 0 才算处理成功；非 0 由宿主当成"这个插件没处理成功"，
 * 不要用它表达"部分成功"。
 */
typedef struct ShotImageProcessorV1 {
    uint32_t struct_size;   /**< = sizeof(ShotImageProcessorV1)；构造方填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */
    /**
     * @brief 就地处理一张图。
     * @param[in,out] image 由宿主提供；像素可改，结构体字段不可改，不能为 NULL。
     * @return 0 = 成功；非 0 = 处理失败（宿主当成"这个插件没处理成功"）。
     * @note 必须按行寻址，并尊重 pixel_format / channels。
     */
    int32_t  (*process)(void* self, ShotImage* image);
} ShotImageProcessorV1;
