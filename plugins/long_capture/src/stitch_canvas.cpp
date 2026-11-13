// 长图画布：定宽、可从头尾两端生长的 RGBA 像素块。
//
// 内存布局（关键，改动前先看）：
//   m_pixels 是一段 capacityRows × width × 4 字节的连续缓冲；
//   第 0 行有效像素在 m_top，共 m_height 行。m_top 上下都可能留空行：
//     · 上方空行（headroom）：向上滚动时直接 m_top -= rows 再 memcpy，不动已有像素；
//     · 下方空行（tailroom）：向下滚动时直接追加在 m_height 之后。
//   余量用尽才整块搬家一次，新容量翻倍且仍然留两侧余量；上限是配置的 max_height。
//
// 这里刻意不用 std::vector<Row> / deque：每行一次分配会让几千行时分配器成为热点，
// 而 memcpy 一整段连续行是 O(n) 且对缓存友好。
//
// 本文件不包含任何平台头（拼接/画布/存盘必须平台无关）。

#include "stitch_canvas.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace long_capture {
namespace {

/** @brief 首次分配/搬家时在两侧各留的最小行数；一帧通常几十到几百行，1024 行够撑很久。 */
constexpr int kMinReserveRows = 1024;

}  // namespace

StitchCanvas::StitchCanvas(int width, int maxHeight)
    : m_width(std::max(1, width)), m_limit(std::max(1, maxHeight)) {}

bool StitchCanvas::append(const CaptureFrame& frame, int srcRow, int rows) {
    if (rows <= 0 || frame.width != m_width || rows > frame.height || srcRow < 0 ||
        srcRow + rows > frame.height) {
        return false;
    }
    if (m_height + rows > m_limit) {
        return false;
    }
    if (!ensureRoom(0, rows)) {
        return false;
    }
    const std::size_t bytes = rowBytes();
    // srcRow 由调用方按"去掉底部固定条"算好；这里只负责把连续 rows 行拷到画布底部。
    const std::uint8_t* src = frame.row(srcRow);
    std::uint8_t* dst = m_pixels.get() + static_cast<std::size_t>(m_top + m_height) * bytes;
    std::memcpy(dst, src, bytes * static_cast<std::size_t>(rows));
    m_height += rows;
    return true;
}

bool StitchCanvas::prepend(const CaptureFrame& frame, int srcRow, int rows) {
    if (rows <= 0 || frame.width != m_width || rows > frame.height || srcRow < 0 ||
        srcRow + rows > frame.height) {
        return false;
    }
    if (m_height + rows > m_limit) {
        return false;
    }
    if (!ensureRoom(rows, 0)) {
        return false;
    }
    const std::size_t bytes = rowBytes();
    // srcRow 由调用方按"去掉顶部固定条"算好（通常是 topStatic）。
    const std::uint8_t* src = frame.row(srcRow);
    m_top -= rows;
    std::uint8_t* dst = m_pixels.get() + static_cast<std::size_t>(m_top) * bytes;
    std::memcpy(dst, src, bytes * static_cast<std::size_t>(rows));
    m_height += rows;
    return true;
}

bool StitchCanvas::trimTop(int rows) {
    if (rows <= 0 || rows >= m_height) {
        return false;
    }
    m_top += rows;
    m_height -= rows;
    return true;
}

bool StitchCanvas::truncate(int rows) {
    if (rows <= 0 || rows >= m_height) {
        return false;
    }
    m_height = rows;
    return true;
}

bool StitchCanvas::trimBottom(int rows) {
    if (rows <= 0 || rows >= m_height) {
        return false;
    }
    m_height -= rows;
    return true;
}

bool StitchCanvas::ensureRoom(int above, int below) {
    const int need = m_height + above + below;
    if (need > m_limit) {
        return false;
    }
    if (m_top >= above && m_top + m_height + below <= m_capacityRows) {
        return true;  // 现有余量就够，不搬家
    }

    // 新容量按"当前内容的两倍 + 两侧需求 + 最小余量"估，再夹到上限；
    // need 一定 <= m_limit，所以 target >= need 一定成立。
    long long target = static_cast<long long>(m_height) * 2 + above + below +
                       static_cast<long long>(kMinReserveRows) * 2;
    if (target > m_limit) {
        target = m_limit;
    }
    const int capacity = static_cast<int>(std::max<long long>(target, need));
    const int extra = capacity - need;
    const int newTop = above + extra / 2;

    const std::size_t bytes = rowBytes();
    auto grown = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(capacity) * bytes);
    if (m_height > 0 && m_pixels != nullptr) {
        std::memcpy(grown.get() + static_cast<std::size_t>(newTop) * bytes,
                    m_pixels.get() + static_cast<std::size_t>(m_top) * bytes,
                    bytes * static_cast<std::size_t>(m_height));
    }
    m_pixels = std::move(grown);
    m_capacityRows = capacity;
    m_top = newTop;
    return true;
}

bool StitchCanvas::buildPreview(int maxWidth, int maxHeight, std::vector<std::uint8_t>& out,
                                int& outWidth, int& outHeight) const {
    if (m_height <= 0 || m_width <= 0 || maxWidth <= 0 || maxHeight <= 0) {
        return false;
    }
    const double scaleX = static_cast<double>(maxWidth) / static_cast<double>(m_width);
    const double scaleY = static_cast<double>(maxHeight) / static_cast<double>(m_height);
    // 不放大：画布比预览框还小时按 1:1 送出去，HUD 端再 StretchBlt 到框内。
    const double scale = std::min({1.0, scaleX, scaleY});
    outWidth = std::max(1, static_cast<int>(static_cast<double>(m_width) * scale));
    outHeight = std::max(1, static_cast<int>(static_cast<double>(m_height) * scale));
    out.resize(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight) * 4);

    for (int y = 0; y < outHeight; ++y) {
        const int srcY = static_cast<int>(static_cast<long long>(y) * m_height / outHeight);
        const std::uint8_t* srcRow = row(srcY);
        std::uint8_t* dstRow =
            out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(outWidth) * 4;
        for (int x = 0; x < outWidth; ++x) {
            const int srcX = static_cast<int>(static_cast<long long>(x) * m_width / outWidth);
            std::memcpy(dstRow + static_cast<std::size_t>(x) * 4,
                        srcRow + static_cast<std::size_t>(srcX) * 4, 4);
        }
    }
    return true;
}

}  // namespace long_capture
