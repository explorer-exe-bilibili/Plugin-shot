#pragma once

// ============================================================================
//  region_image.h —— 截图像素的解码 / 裁剪 / 编码（stb，平台无关）
//
//  Linux/Wayland 分支专用：portal 截图是磁盘上的 PNG，解码成 RGBA8 之后既当覆盖层
//  背景，也是最终裁剪的数据源——裁剪直接在这份内存上做，不再触发第二次采集。
//
//  Windows 分支不使用本文件：它在 win32 层用 GDI BitBlt 现场截屏，并且发布 screen
//  region 让宿主流水线裁剪，而不是自己写 PNG。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace region_selector {

/**
 * @brief 截图图像像素矩形：原点 = 整张截图的左上角。
 *
 * 与 ScreenRect 的区别：ScreenRect 是虚拟桌面/显示器的系统坐标，ImageRect 是图像
 * 自身的像素坐标。Wayland 下截图可能包含所有输出（合成图），它的像素坐标与任何
 * 输出布局都没有关系，所以两者不能混用。
 */
struct ImageRect {
    int x = 0;      /**< 左边缘（图像像素，0 = 整张图最左边）。 */
    int y = 0;      /**< 上边缘（图像像素，0 = 整张图最上边）。 */
    int width = 0;  /**< 宽度（> 0 才有内容）。 */
    int height = 0; /**< 高度（> 0 才有内容）。 */
};

/**
 * @brief 用 stb_image 把 PNG 文件解码成 RGBA8 像素（顶行 = 图像顶部）。
 *
 * @param[in] path    PNG 文件路径（UTF-8）。
 * @param[out] pixels 输出像素，长度 = width * height * 4；失败时清空。
 * @param[out] width  图像宽度；失败时为 0。
 * @param[out] height 图像高度；失败时为 0。
 * @return true = 解码成功；false = 文件不存在 / 不是 PNG / 内存不足。
 * @note 固定按 4 通道解码（灰度/调色板 PNG 也补成 RGBA），这样裁剪与写盘只有一种布局。
 */
bool loadPngAsRgba(const std::string& path, std::vector<std::uint8_t>& pixels, int& width,
                   int& height);

/**
 * @brief 从 RGBA8 源图里裁出一块并写成 PNG（stb_image_write）。
 *
 * @param[in] pixels 源像素（长度 = width * height * 4，顶行 = 图像顶部）。
 * @param[in] width  源图宽度。
 * @param[in] height 源图高度。
 * @param[in] crop   裁剪矩形（图像像素）；越界部分会被夹进源图。
 * @param[in] path   输出 PNG 路径（UTF-8）；目录必须已存在。
 * @return true = 已完整写出；false = 参数非法 / 裁剪为空 / 写文件失败。
 * @note 裁剪矩形先夹取再编码；写入失败时 stb 可能留下半个文件，由调用方按失败处理。
 */
bool saveRgbaCropAsPng(const std::uint8_t* pixels, int width, int height, const ImageRect& crop,
                       const std::string& path);

}  // namespace region_selector
