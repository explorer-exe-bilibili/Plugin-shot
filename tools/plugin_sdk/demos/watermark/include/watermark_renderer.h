#pragma once

// ============================================================================
//  include/watermark_renderer.h —— 把一行文字画到图像右下角（白字 + 黑描边）
//
//  只依赖 ShotImage 契约与内置点阵字体，不依赖任何图像库。绘制规则：
//    · 文字按整数倍放大，白色不透明；
//    · 每个白色像素周围先铺一圈黑色描边，保证浅色/深色背景上都看得清；
//    · 从右下角往左/往上排版，距边缘各留 margin 像素；
//    · 只改像素（B/G/R/A），绝不改 ShotImage 的结构体字段，且严格按 stride 寻址。
//
//  线程模型：纯函数、无共享状态，可在采集 worker 线程上安全调用。
// ============================================================================

#include <string_view>

#include "plugin_api.h"

namespace watermark_demo {

/**
 * @brief 在图像右下角绘制一行水印文字。
 * @param[in,out] image  宿主提供的 BGRA8 图像；只改像素，不改结构体字段。
 * @param[in]     text   要绘制的文本（ASCII；字体表外的字符按空格宽度处理）。
 * @param[in]     scale  整数放大倍数：1 = 5x7 像素，4 = 20x28 像素。
 * @param[in]     margin 距右边缘/下边缘的像素数（>= 0）。
 * @return true = 已绘制；false = 参数非法、文本为空或图像太小放不下（不改任何像素）。
 * @note 图像不满足 BGRA8/stride 约定时也返回 false：调用方按"跳过本次水印"处理即可。
 */
bool drawTextBottomRight(ShotImage* image, std::string_view text, int scale, int margin);

}  // namespace watermark_demo
