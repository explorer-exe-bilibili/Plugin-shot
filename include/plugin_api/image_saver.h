#pragma once

#include <stdint.h>
#include "plugin_api/image.h"

// ============================================================================
//  能力：图像保存  "shot.image_saver"
// ============================================================================

/**
 * @brief 图像保存能力函数表，对应 SHOT_IFACE_IMAGE_SAVER、版本 1。
 *
 * 宿主把处理完的图像交给它，由它决定文件格式、编码参数、目录布局——这些全部
 * 来自插件自己的 shot.configurable 配置项，宿主不硬编码任何保存策略。因此换一个
 * 保存插件（例如只写 BMP 的调试插件、上传到图床的插件）不需要改宿主。
 *
 * directory / basename 是**本次调用的覆盖参数**：为 NULL 时插件使用自己的配置/默认值；
 * 非 NULL 时以传入值为准（设置页/命令行"另存为"会用到）。basename 不含扩展名。
 *
 * path 由宿主提供：成功时插件把最终文件路径（含扩展名，NUL 结尾）写进去；长度不够
 * 时返回 2，宿主应先给 1024 字节这类够用的缓冲。路径以平台习惯编码（Windows UTF-8
 * 落盘前再转宽字符）写入，宿主只用于日志与事件负载。
 *
 * 线程模型：save 只在宿主调用线程上执行，同一实例不会被并发调用；实现无需考虑重入。
 *
 * 返回码：0 = 已落盘；1 = 参数不合法（image 为空/尺寸非法/格式不认识）；
 * 2 = path 缓冲不足；3 = 保存失败（目录不可写、编码失败等，实现应已写日志）。
 *
 * @note save 必须实现；没有它宿主按"没有保存插件"处理并发布 capture.failed。
 */
typedef struct ShotImageSaverV1 {
    uint32_t struct_size;   /**< = sizeof(ShotImageSaverV1)；构造方填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */
    /**
     * @brief 把一张图像保存成文件。
     * @param[in]  self      插件实例指针。
     * @param[in]  image     待保存图像；只读，插件不得修改像素或结构体字段。
     * @param[in]  directory 覆盖输出目录（UTF-8）；NULL = 用插件配置。
     * @param[in]  basename  覆盖文件名（不含扩展名，UTF-8）；NULL = 用插件配置。
     * @param[out] path      接收最终文件路径的缓冲；不能为 NULL。
     * @param[in]  path_size path 的字节数。
     * @return 0 = 成功；非 0 见上面的返回码约定。
     */
    int32_t (*save)(void* self, const ShotImage* image, const char* directory,
                    const char* basename, char* path, uint32_t path_size);
} ShotImageSaverV1;
