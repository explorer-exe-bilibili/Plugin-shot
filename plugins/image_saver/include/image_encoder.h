#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "plugin_api.h"

namespace image_saver {

/**
 * @brief 把 BGRA8 的 ShotImage 编码成 PNG/JPEG 文件。
 *
 * 分两步：先按 stride 逐行把 BGRA 转成紧凑缓冲（PNG 用 RGBA、JPEG 用 RGB），
 * 再交给 stb_image_write 编码。ShotImage 的 stride 可以为负、也可以带行尾填充，
 * 所以只能按行寻址；把 data 当连续块是这类插件最常见的越界来源。
 *
 * @note 写文件走 stbi_write_*_to_func + 自己的 ofstream，而不是 stb 的按文件名
 *       接口：后者吃 char* 路径（Windows 上是 ANSI），中文目录会打不开；先用
 *       std::filesystem::path 的 UTF-8 构造把路径变成宽字符再开流，才能跨平台
 *       支持 UTF-8 路径。这条是契约里"Windows UTF-8 落盘前再转宽字符"的落点。
 */
class ImageEncoder {
public:
    /**
     * @brief 是否是本插件认识的格式名。
     * @param[in] format "png" 或 "jpeg"（大小写敏感，与配置项 ENUM 候选一致）。
     * @return true = 支持编码。
     */
    static bool supportsFormat(std::string_view format) noexcept;

    /**
     * @brief 返回格式对应的文件扩展名（含点）。
     * @param[in] format 格式名；不认识的格式按 PNG 处理，与 save 的默认一致。
     * @return 静态字符串 ".png" 或 ".jpg"。
     */
    static const char* extensionFor(std::string_view format) noexcept;

    /**
     * @brief 校验图像能不能编码：data 非空、宽高为正、channels == 4、|stride| 装得下一行。
     *
     * 老宿主的 ShotImage 里可能没有 pixel_format 字段；那种情况按当年约定用
     * channels == 4 当成 BGRA8（新插件 + 老宿主的降级路径）。
     *
     * @param[in] image 待保存图像。
     * @return 0 = 可以编码；1 = 参数非法（对应 save 的返回码 1）。
     */
    static std::int32_t validate(const ShotImage& image) noexcept;

    /**
     * @brief 把图像编码成文件；失败时删掉写了一半的文件，不留垃圾。
     * @param[in] image   已通过 validate 的图像；只读，不修改。
     * @param[in] format  "png" / "jpeg"。
     * @param[in] quality JPEG 质量 1..100（越界会被夹住；PNG 忽略）。
     * @param[in] path    目标路径；调用方保证父目录已存在。
     * @return true = 已完整写出；false = 打开/编码/写盘任一步失败。
     */
    static bool encode(const ShotImage& image, std::string_view format, int quality,
                       const std::filesystem::path& path);

private:
    /**
     * @brief 逐行把 BGRA 转成紧凑的 RGBA（PNG 用，保留 alpha）。
     * @param[in]  image 已通过 validate 的图像。
     * @param[out] out   输出像素，按 width*4 行宽连续存放。
     * @return true = 成功；false = 参数非法。
     */
    static bool convertToRgba(const ShotImage& image, std::vector<std::uint8_t>& out);

    /**
     * @brief 逐行把 BGRA 转成紧凑的 RGB（JPEG 用，丢弃 alpha）。
     * @param[in]  image 已通过 validate 的图像。
     * @param[out] out   输出像素，按 width*3 行宽连续存放。
     * @return true = 成功；false = 参数非法。
     */
    static bool convertToRgb(const ShotImage& image, std::vector<std::uint8_t>& out);

    /**
     * @brief stb_image_write 的写文件回调：把编码出来的字节写进 ofstream。
     *
     * 函数签名由 stb 的 stbi_write_func 固定，只能是自由函数形态，因此作为
     * private static 成员而不是匿名命名空间函数（STYLE §6.7 的合法例外：
     * 签名由第三方 API 固定，无法改成类成员函数以外的东西）。
     *
     * @param[in,out] context 指向 std::ofstream 的指针（由 encode 传入）。
     * @param[in]     data    编码数据块，只在调用期间有效。
     * @param[in]     size    data 的字节数；stb 可能分多次回调。
     */
    static void writeToStream(void* context, void* data, int size);
};

}  // namespace image_saver
