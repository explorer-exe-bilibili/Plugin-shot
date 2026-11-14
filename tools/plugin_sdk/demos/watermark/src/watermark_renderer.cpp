// 水印渲染实现：内置点阵字体 → BGRA8 像素，白字 + 1px 黑描边，右下角排版。
//
// 为什么先描边再画白字：截图可能是深色也可能是浅色，单色文字总有一半场景看不清；
// 1px 描边让白字在浅色背景上也有轮廓，代价只是多一遍像素写。
//
// 坐标全部相对图像算（margin 从右/下边缘量起），y 行用 stride 寻址：stride 为负
// （自底向上存储）时 data + y*stride 仍然指向第 y 行，这是契约要求的按行寻址。
//
// 本文件不依赖任何共享状态，采集 worker 线程可直接调用。

#include "watermark_renderer.h"

#include <cstddef>
#include <cstdint>

#include "bitmap_font.h"

namespace watermark_demo {
namespace {

/**
 * @brief 校验图像可写：BGRA8、4 通道、stride 不小于一行像素。
 * @param[in] image 宿主给的图像。
 * @return true = 可以按 BGRA8 写像素。
 */
bool validateImage(const ShotImage* image) {
    if (image == nullptr || image->data == nullptr) {
        return false;
    }
    if (image->width < 1 || image->height < 1) {
        return false;
    }
    if (!SHOT_HAS_FIELD(image, ShotImage, pixel_format) ||
        image->pixel_format != SHOT_PIXEL_FORMAT_BGRA8) {
        return false;
    }
    if (image->channels != 4) {
        return false;
    }
    const std::int32_t rowBytes = image->width * 4;
    const std::int32_t absStride = image->stride < 0 ? -image->stride : image->stride;
    return absStride >= rowBytes;
}

/**
 * @brief 取 (x, y) 处像素的地址；调用方保证坐标在界内。
 * @param[in] image 图像。
 * @param[in] x     列坐标（0 .. width-1）。
 * @param[in] y     行坐标（0 .. height-1）。
 * @return 指向该像素 B 分量的指针（BGRA8，4 字节）。
 */
std::uint8_t* pixelAt(const ShotImage& image, int x, int y) {
    return image.data + static_cast<std::ptrdiff_t>(y) * image.stride +
           static_cast<std::ptrdiff_t>(x) * 4;
}

/**
 * @brief 把 (x, y) 刷成不透明颜色；越界自动忽略（水印允许被图像边缘裁掉）。
 * @param[in] image 图像。
 * @param[in] x     列坐标。
 * @param[in] y     行坐标。
 * @param[in] b     B 分量。
 * @param[in] g     G 分量。
 * @param[in] r     R 分量。
 * @note 写入的 A 恒为 255：水印必须不透明，否则在带透明通道的截图里会看不见。
 */
void paintPixel(const ShotImage& image, int x, int y, std::uint8_t b, std::uint8_t g,
                std::uint8_t r) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
        return;
    }
    std::uint8_t* pixel = pixelAt(image, x, y);
    pixel[0] = b;
    pixel[1] = g;
    pixel[2] = r;
    pixel[3] = 255;
}

/**
 * @brief 画一个字形：先画 1px 黑色描边，再把字身刷成白色。
 * @param[in] image   图像。
 * @param[in] glyph   字形点阵。
 * @param[in] originX 字形左上角在图像里的列坐标。
 * @param[in] originY 字形左上角在图像里的行坐标。
 * @param[in] scale   放大倍数：一个点阵像素占 scale x scale 个图像像素。
 */
void drawGlyph(const ShotImage& image, const Glyph& glyph, int originX, int originY,
               int scale) {
    // 第一遍：描边。对每个"亮"的点阵像素，把它放大方块外面那一圈刷黑；
    // 方块之间会重叠，重复写黑没有副作用，换来实现简单。
    for (int row = 0; row < kGlyphHeight; ++row) {
        const std::uint8_t bits = glyph.rows[row];
        for (int col = 0; col < kGlyphWidth; ++col) {
            // bit4 是最左列，所以从高位往低位读。
            if ((bits & static_cast<std::uint8_t>(1u << (kGlyphWidth - 1 - col))) == 0) {
                continue;
            }
            const int x = originX + col * scale;
            const int y = originY + row * scale;
            for (int dy = -1; dy <= scale; ++dy) {
                for (int dx = -1; dx <= scale; ++dx) {
                    const bool inside = dx >= 0 && dx < scale && dy >= 0 && dy < scale;
                    if (!inside) {
                        paintPixel(image, x + dx, y + dy, 0, 0, 0);
                    }
                }
            }
        }
    }
    // 第二遍：白色字身，覆盖在描边内侧。
    for (int row = 0; row < kGlyphHeight; ++row) {
        const std::uint8_t bits = glyph.rows[row];
        for (int col = 0; col < kGlyphWidth; ++col) {
            if ((bits & static_cast<std::uint8_t>(1u << (kGlyphWidth - 1 - col))) == 0) {
                continue;
            }
            const int x = originX + col * scale;
            const int y = originY + row * scale;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    paintPixel(image, x + dx, y + dy, 255, 255, 255);
                }
            }
        }
    }
}

}  // namespace

bool drawTextBottomRight(ShotImage* image, std::string_view text, int scale, int margin) {
    if (image == nullptr || text.empty() || scale < 1 || margin < 0) {
        return false;
    }
    if (!validateImage(image)) {
        return false;
    }
    const int textWidth = textPixelWidth(text, scale);
    const int textHeight = kGlyphHeight * scale;
    // 右下角对齐；再留 1px 给描边，否则字的最外圈会被图像边缘裁掉。
    const int startX = image->width - margin - textWidth;
    const int startY = image->height - margin - textHeight;
    if (startX < 1 || startY < 1 || startX + textWidth > image->width - 1 ||
        startY + textHeight > image->height - 1) {
        return false;  // 图太小/边距太大：整行不画，避免画出半行字
    }

    // 逐字符绘制；字间距 = scale 像素（与 textPixelWidth 的算法保持一致）。
    const int advance = kGlyphWidth * scale + scale;
    int x = startX;
    for (const char character : text) {
        if (const Glyph* glyph = findGlyph(character)) {
            drawGlyph(*image, *glyph, x, startY, scale);
        }
        x += advance;  // 字体表外的字符只是空出一格，不中断整行
    }
    return true;
}

}  // namespace watermark_demo
