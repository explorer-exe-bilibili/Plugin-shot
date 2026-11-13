#pragma once

// ============================================================================
//  include/stitch_canvas.h —— 长图画布（平台无关核心）
//
//  把"一串 4 字节/像素的 RGBA 行"放在一段连续内存里，支持从底部追加、从顶部
//  前插。为什么要在中间留上下余量（而不是每帧重新分配整块）：
//    · 用户向下滚动时新行追加在底部，向上滚动时前插在顶部，两个方向会交替出现；
//    · 每次前插都 memmove 整幅图的话，几千像素的画布会被搬来搬去；
//    · 预留 headroom/tailroom 后，常见的一帧（几十到几百行）只是 memcpy 一小段，
//      只有余量用尽才搬家一次，而且新容量按需翻倍（上限是配置的 max_height）。
//
//  本文件与实现只用标准库，不包含任何平台头：画布算法在 Windows / macOS /
//  Linux 上语义完全一致（拼接/画布/存盘必须平台无关）。
// ============================================================================

#include <cstdint>
#include <memory>
#include <vector>

#include "capture_frame.h"

namespace long_capture {

/**
 * @brief 长图画布：定宽、可变高、可从头尾两端生长的 RGBA 像素块。
 *
 * 线程模型：不属于线程安全类型；只由会话线程在"拼一帧"时独占使用，HUD 预览
 * 走 buildPreview() 复制出来的小图，不直接读本对象的内部缓冲。
 */
class StitchCanvas {
public:
    /**
     * @brief 创建定宽画布。
     * @param[in] width     每行的像素宽度，必须 > 0（目标窗口客户区宽度）。
     * @param[in] maxHeight 高度上限（行数），必须 > 0；达到后 append/prepend 返回 false。
     */
    StitchCanvas(int width, int maxHeight);

    /** @brief 不可拷贝：内部持有一整幅像素缓冲。 */
    StitchCanvas(const StitchCanvas&) = delete;
    /** @brief 不可赋值。 */
    StitchCanvas& operator=(const StitchCanvas&) = delete;

    /** @brief 画布宽度（像素）。 */
    int width() const noexcept { return m_width; }
    /** @brief 当前高度（行数）。 */
    int height() const noexcept { return m_height; }
    /** @brief 是否还没有任何像素。 */
    bool empty() const noexcept { return m_height <= 0; }
    /** @brief 是否已经达到配置的高度上限。 */
    bool full() const noexcept { return m_height >= m_limit; }

    /** @brief 整幅有效像素的起始地址（height × width × 4 字节连续）；空画布返回 nullptr。 */
    const std::uint8_t* data() const noexcept {
        return (m_height <= 0 || m_pixels == nullptr)
                   ? nullptr
                   : (m_pixels.get() + static_cast<std::size_t>(m_top) * rowBytes());
    }

    /** @brief 第 y 行的首地址；调用方保证 0 <= y < height()。 */
    const std::uint8_t* row(int y) const noexcept {
        return m_pixels.get() + static_cast<std::size_t>(m_top + y) * rowBytes();
    }

    /**
     * @brief 把一帧底部的 rows 行追加到画布底部。
     *
     * @param[in] frame 已经解码的一帧；宽度必须等于画布宽度，rows 不能超过帧高。
     * @param[in] rows  要追加的行数（正位移 d）。
     * @return true = 已拷贝；false = 参数非法或会超过高度上限。
     */
    bool append(const CaptureFrame& frame, int srcRow, int rows);

    /**
     * @brief 把一帧顶部的 rows 行前插到画布顶部。
     *
     * @param[in] frame 已经解码的一帧；宽度必须等于画布宽度，rows 不能超过帧高。
     * @param[in] rows  要前插的行数（负位移的绝对值）。
     * @return true = 已拷贝；false = 参数非法或会超过高度上限。
     */
    bool prepend(const CaptureFrame& frame, int srcRow, int rows);

    /**
     * @brief 裁掉画布顶部 rows 行（识别出标题栏/固定条后把首帧里的它们去掉）。
     * @param[in] rows 行数；<=0 或 >= 当前高度时返回 false，画布不变。
     * @return true = 已裁剪。
     */
    bool trimTop(int rows);

    /**
     * @brief 把画布截断到前 rows 行（丢掉底部多余内容）。
     *
     * 用在哪：缺口期间画布按"接在末尾"的估计坐标追加过内容，用户回滚并恢复坐标后，
     * 这些行的位置不可信；截断回缺口前的可信高度，等用户重新滚下来时再按正确坐标
     * 追加，才能得到"回滚后重新填上正确内容"的效果。
     *
     * @param[in] rows 保留的行数；<=0 或 >= 当前高度时返回 false，画布不变。
     * @return true = 已截断。
     */
    bool truncate(int rows);

    /**
     * @brief 裁掉画布底部 rows 行（识别出状态栏/固定条后把首帧里的它们去掉）。
     * @param[in] rows 行数；<=0 或 >= 当前高度时返回 false，画布不变。
     * @return true = 已裁剪。
     */
    bool trimBottom(int rows);

    /**
     * @brief 生成一张等比缩小的预览（给 HUD 缩略图用）。
     *
     * 采样方式是最近邻：预览只用于"看个大概"，HUD 端还会用 HALFTONE 再缩一次，
     * 双线性在这里做没有收益，反而拖慢会话线程。
     *
     * @param[in]  maxWidth  预览最大宽度（像素），必须 > 0。
     * @param[in]  maxHeight 预览最大高度（像素），必须 > 0。
     * @param[out] out       紧凑 RGBA 预览缓冲（outWidth × outHeight × 4）。
     * @param[out] outWidth  预览宽度。
     * @param[out] outHeight 预览高度。
     * @return true = 已生成；false = 画布为空或参数非法。
     */
    bool buildPreview(int maxWidth, int maxHeight, std::vector<std::uint8_t>& out,
                      int& outWidth, int& outHeight) const;

private:
    /** @brief 每行的字节数：width × 4。 */
    std::size_t rowBytes() const noexcept { return static_cast<std::size_t>(m_width) * 4U; }

    /**
     * @brief 保证画布上方有 above 行、下方有 below 行的空余空间。
     *
     * 余量不够时整块搬家：新容量 = 已有内容 + 上下需求 + 两侧余量（上限 m_limit），
     * 内容搬到新缓冲的中部，之后一段时间内的追加/前插都不再搬家。
     *
     * @return true = 空间已就绪；false = 需求会超过高度上限。
     */
    bool ensureRoom(int above, int below);

    int m_width = 0;                 /**< 每行像素宽度。 */
    int m_limit = 0;                 /**< 高度上限（行数）。 */
    int m_capacityRows = 0;          /**< 已分配的缓冲行数（含上下余量）。 */
    int m_top = 0;                   /**< 第 0 行有效像素在缓冲里的行下标。 */
    int m_height = 0;                /**< 当前有效行数。 */
    // 用 unique_ptr<uint8_t[]> 而不是 vector：vector::resize 会把新容量清零，
    // 几百 MB 的画布搬家时平白多一次 memset；这里只需要一块未初始化的裸缓冲。
    std::unique_ptr<std::uint8_t[]> m_pixels; /**< capacityRows × width × 4 字节。 */
};

}  // namespace long_capture
