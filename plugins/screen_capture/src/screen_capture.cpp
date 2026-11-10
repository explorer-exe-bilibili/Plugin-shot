// 契约层：校验宿主入参、把平台层结果映射成契约返回码、写日志。
//
// 采集是两段式：describe 报虚拟桌面几何，宿主据此自己分配 BGRA8 缓冲，capture
// 再把像素写进去。本文件不出现 Win32 类型——GDI 细节在 gdi_desktop_capture.cpp，
// 契约与平台实现各归各的文件（STYLE.md §6.6/§6.8）。

#include "screen_capture.h"

#include <cstdint>

#include "screen_capture_impl.h"
#include "shot_plugin_helpers.h"

namespace screen_capture {

ScreenCapture::ScreenCapture(const ShotHostServices* host) noexcept : m_host(host) {}

ScreenCapture::~ScreenCapture() = default;

std::int32_t ScreenCapture::describe(ShotCaptureInfo* out) const {
    const shot::Logger log(m_host);
    if (out == nullptr) {
        log.error("screen_capture: describe 收到空输出结构");
        return 2;
    }
    // 五个字段要一次写全。宿主给的 struct_size 短于完整结构，说明它对契约的理解
    // 比本插件旧；按"对方结构太短"拒绝，绝不写到自己这块内存之外。
    if (out->struct_size < SHOT_REQUIRES(ShotCaptureInfo, pixel_format)) {
        log.error("screen_capture: 宿主给的 ShotCaptureInfo 太短，拒绝写入");
        return 2;
    }

    ShotCaptureInfo geometry = {};
    if (!GdiDesktopCapture::queryGeometry(geometry, log)) {
        // 远程会话断开 / 没有显示器时 GetSystemMetrics 会返回 0。这不是错误，而是
        // "当前没有可采集的屏幕"，契约给了独立的返回码 1。
        log.warn("screen_capture: 当前没有可采集的屏幕");
        return 1;
    }
    // 区域 / 单显示器采集：v1 只描述整个虚拟桌面（ShotCaptureInfo 没有目标矩形字段）。
    // 取景不归插件管：宿主先抓整屏，再按 region 自己做裁剪，所以这里永远报整屏几何。
    // 以后若 ABI 追加目标矩形字段，再在这里换算成矩形；capture 的 BitBlt 参数不用动。

    out->width = geometry.width;
    out->height = geometry.height;
    out->origin_x = geometry.origin_x;
    out->origin_y = geometry.origin_y;
    out->pixel_format = SHOT_PIXEL_FORMAT_BGRA8;
    return 0;
}

std::int32_t ScreenCapture::capture(ShotImage* dst, bool includeCursor) const {
    const shot::Logger log(m_host);
    if (dst == nullptr || !isValidTarget(*dst)) {
        log.error("screen_capture: capture 的目标图不合法（空指针 / 格式 / 尺寸 / stride）");
        return 2;
    }

    ShotCaptureInfo geometry = {};
    if (!GdiDesktopCapture::queryGeometry(geometry, log)) {
        log.warn("screen_capture: 当前没有可采集的屏幕");
        return 1;
    }

    // describe 与 capture 是两次独立调用：中间可能换分辨率、拔显示器。尺寸对不上时
    // 返回 2，绝不按错的尺寸写像素（那会把画面写错行，或者让宿主以为成功了）。
    if (geometry.width != dst->width || geometry.height != dst->height) {
        log.error("screen_capture: 目标尺寸与当前虚拟桌面不一致，拒绝写入");
        return 2;
    }

    if (!GdiDesktopCapture::captureTo(*dst, geometry, includeCursor, log)) {
        return 3;
    }
    return 0;
}

bool ScreenCapture::isValidTarget(const ShotImage& dst) {
    if (dst.data == nullptr || dst.width <= 0 || dst.height <= 0) {
        return false;
    }
    // 本能力只写 BGRA8。宿主可能比本插件老、结构里还没有 pixel_format 字段，那种
    // 情况按当年的约定当成 BGRA8 处理（STYLE.md §6.5 的降级，不是错误）。
    if (SHOT_HAS_FIELD(&dst, ShotImage, pixel_format) &&
        dst.pixel_format != SHOT_PIXEL_FORMAT_BGRA8) {
        return false;
    }
    if (dst.channels != 4) {
        return false;  // BGRA8 的 "8" 是每通道 8 位，4 个通道
    }
    // 按 |stride| >= width*4 验证一行装得下：stride 可以为负（自底向上），也可以
    // 大于 width*4（行尾填充）。用 int64 算，避免极端 width 下 int 乘法溢出。
    const std::int64_t rowBytes = static_cast<std::int64_t>(dst.width) * 4;
    const std::int64_t stride = dst.stride;
    return stride > 0 ? stride >= rowBytes : -stride >= rowBytes;
}

}  // namespace screen_capture
