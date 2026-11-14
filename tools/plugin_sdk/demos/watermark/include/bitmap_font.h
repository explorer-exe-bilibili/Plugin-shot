#pragma once

// ============================================================================
//  include/bitmap_font.h —— 内置 5x7 点阵字体
//
//  为什么要自带字体：watermark demo 的验收标准是"只依赖核心 SDK、不依赖
//  OpenCV / FreeType / 系统字体"，所以字形数据直接内嵌在 DLL 里。当前覆盖：
//    数字 0-9、字母 A-Z（小写自动映射成大写）、以及 : - . / _ 和空格。
//  其它字符（包括中文）按"不绘制 + 占一个字符宽度"处理；需要中文水印时替换成
//  点阵表或接 FreeType，不在本 demo 的范围内。
//
//  字形布局：每个字符 5 列 × 7 行，每行用低 5 位表示该行 5 个像素（最高位是
//  最左列）。绘制方按整数倍放大即可得到清晰的大字。
// ============================================================================

#include <cstdint>
#include <string_view>

namespace watermark_demo {

/** @brief 字形宽度（5 列）；实际绘制宽度 = kGlyphWidth * scale。 */
inline constexpr int kGlyphWidth = 5;

/** @brief 字形高度（7 行）；实际绘制高度 = kGlyphHeight * scale。 */
inline constexpr int kGlyphHeight = 7;

/** @brief 一个字形的 7 行点阵（每行低 5 位有效）。 */
struct Glyph {
    char character;         /**< 该字形对应的可打印 ASCII 字符。 */
    std::uint8_t rows[kGlyphHeight]; /**< 第 0 行（上）到第 6 行（下）的点阵。 */
};

/**
 * @brief 查一个字符的字形。
 * @param[in] character 要查的字符；小写字母会先映射成大写。
 * @return 字形指针；字符不在字体表里（含中文/控制字符）时返回 nullptr。
 * @note 返回的指针指向静态数据，整个 DLL 生命周期内有效。
 */
const Glyph* findGlyph(char character);

/**
 * @brief 计算一行文本的绘制宽度（放大 scale 倍、字间距 scale 像素）。
 * @param[in] text  文本；按字节遍历（ASCII 之外按一个字符宽度算）。
 * @param[in] scale 整数放大倍数（>= 1）。
 * @return 像素宽度；文本为空或 scale < 1 时返回 0。
 */
int textPixelWidth(std::string_view text, int scale);

}  // namespace watermark_demo
