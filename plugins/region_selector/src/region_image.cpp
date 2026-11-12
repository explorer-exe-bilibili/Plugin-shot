// region_image —— stb 的 PNG 解码 / 裁剪 / 编码（Linux/Wayland 分支）。
//
// 整个插件只有本文件定义 stb 的实现宏：stb 的实现在头文件里，多个 TU 同时定义会
// 重复符号。图片都不会太大（最大 8K 级别），一次整图解码 + 一次裁剪拷贝就够了，
// 不做行式流式处理。
//
// 为什么用 stb 而不是 GdkPixbuf / libpng：vcpkg 清单里已有 stb（image_saver 与
// clipboard 也在用），不引入新的系统依赖；门户返回的就是 PNG，stb 直接吃。

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "region_image.h"

namespace region_selector {
namespace {

/** @brief 把裁剪矩形夹进图像范围；夹完为空时宽高都是 0，调用方按失败处理。 */
ImageRect clampToImage(const ImageRect& crop, int width, int height) {
    ImageRect clamped;
    if (width <= 0 || height <= 0) {
        return clamped;
    }
    const int left = std::clamp(crop.x, 0, width);
    const int top = std::clamp(crop.y, 0, height);
    const int right = std::clamp(crop.x + crop.width, 0, width);
    const int bottom = std::clamp(crop.y + crop.height, 0, height);
    clamped.x = std::min(left, right);
    clamped.y = std::min(top, bottom);
    clamped.width = std::abs(right - left);
    clamped.height = std::abs(bottom - top);
    return clamped;
}

}  // namespace

bool loadPngAsRgba(const std::string& path, std::vector<std::uint8_t>& pixels, int& width,
                   int& height) {
    pixels.clear();
    width = 0;
    height = 0;
    if (path.empty()) {
        return false;
    }
    int decodedWidth = 0;
    int decodedHeight = 0;
    int channels = 0;
    // desired_channels = 4：灰度/调色板/带 alpha 的 PNG 统一补成 RGBA，后续只有一种布局。
    unsigned char* decoded =
        stbi_load(path.c_str(), &decodedWidth, &decodedHeight, &channels, 4);
    if (decoded == nullptr || decodedWidth <= 0 || decodedHeight <= 0) {
        if (decoded != nullptr) {
            stbi_image_free(decoded);
        }
        return false;
    }
    const std::size_t byteCount = static_cast<std::size_t>(decodedWidth) *
                                  static_cast<std::size_t>(decodedHeight) * 4u;
    try {
        pixels.assign(decoded, decoded + byteCount);
    } catch (...) {
        // 像素缓冲分配失败（超大图 / 内存紧张）：按解码失败处理，异常不越过 DLL 边界。
        stbi_image_free(decoded);
        pixels.clear();
        return false;
    }
    stbi_image_free(decoded);
    width = decodedWidth;
    height = decodedHeight;
    return true;
}

bool saveRgbaCropAsPng(const std::uint8_t* pixels, int width, int height, const ImageRect& crop,
                       const std::string& path) {
    if (pixels == nullptr || path.empty()) {
        return false;
    }
    const ImageRect clamped = clampToImage(crop, width, height);
    if (clamped.width <= 0 || clamped.height <= 0) {
        return false;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(clamped.width) * 4u;
    std::vector<std::uint8_t> cropped;
    try {
        cropped.resize(rowBytes * static_cast<std::size_t>(clamped.height));
    } catch (...) {
        return false;
    }
    // 逐行拷贝：源图是整张截图（行宽 = 图宽），输出只保留选中列。
    for (int row = 0; row < clamped.height; ++row) {
        const std::size_t sourceOffset =
            (static_cast<std::size_t>(clamped.y + row) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(clamped.x)) *
            4u;
        std::memcpy(cropped.data() + static_cast<std::size_t>(row) * rowBytes,
                    pixels + sourceOffset, rowBytes);
    }
    // stride 用行字节数；stb 在 stride <= 0 时会按 width * comp 处理，这里显式传正值。
    return stbi_write_png(path.c_str(), clamped.width, clamped.height, 4, cropped.data(),
                          static_cast<int>(rowBytes)) != 0;
}

}  // namespace region_selector
