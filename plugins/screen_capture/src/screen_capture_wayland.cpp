// Linux/Wayland 契约层：校验宿主入参、把 Wayland 采集结果映射成契约返回码、写日志。
//
// 与 Windows 的 screen_capture.cpp 是同一职责的平台版本：CMake 按平台只编译其中
// 一个，所以两个 .cpp 各自定义同一个 ScreenCapture 类，公共头与 ABI 胶水完全不变；
// GDI 实现一行未动。
//
// 尺寸语义与 Windows 不同：Wayland 门户一次只共享一个源（整个输出或单个窗口），
// 没有"虚拟桌面"坐标系，所以 origin 恒为 (0,0)，describe 的 width/height 是
// PipeWire 实际协商的帧尺寸。

#include "screen_capture.h"

#include <cstdint>
#include <memory>
#include <new>
#include <string>

#include "shot_plugin_helpers.h"
#include "wayland_capture.h"

namespace screen_capture {
namespace {

// capture 最多等新帧的时间。门户的 PipeWire 流通常 1 帧内就有数据；200ms 是"允许
// 一次短抖动"与"不让宿主 worker 线程卡太久"之间的折中（需求给的上限）。
constexpr std::int32_t kFrameWaitMs = 200;

}  // namespace

ScreenCapture::ScreenCapture(const ShotHostServices* host) noexcept
    : m_host(host), m_wayland(new (std::nothrow) WaylandCapture(host)) {}

ScreenCapture::~ScreenCapture() = default;

std::int32_t ScreenCapture::describe(ShotCaptureInfo* out) const {
    const shot::Logger log(m_host);
    if (out == nullptr) {
        log.error("screen_capture: describe 收到空输出结构");
        return 2;
    }
    // 五个字段要一次写全：宿主给的结构短于完整结构时按"对方结构太短"拒绝，
    // 绝不写到自己这块内存之外（与 Windows 版同一套校验）。
    if (out->struct_size < SHOT_REQUIRES(ShotCaptureInfo, pixel_format)) {
        log.error("screen_capture: 宿主给的 ShotCaptureInfo 太短，拒绝写入");
        return 2;
    }
    if (m_wayland == nullptr) {
        log.error("screen_capture: Wayland 采集对象未创建（内存不足）");
        return 3;
    }

    std::int32_t width = 0;
    std::int32_t height = 0;
    std::string error;
    const std::int32_t code = m_wayland->ensureReady(width, height, error);
    if (code != 0) {
        log.error("screen_capture: " + error);
        return code;
    }

    out->width = width;
    out->height = height;
    // 门户一次只共享一个源（输出或窗口），画面原点就是该源左上角；Wayland 没有
    // Windows 的虚拟桌面合成坐标系，所以 origin 恒为 (0,0)。
    out->origin_x = 0;
    out->origin_y = 0;
    out->pixel_format = SHOT_PIXEL_FORMAT_BGRA8;
    return 0;
}

std::int32_t ScreenCapture::capture(ShotImage* dst, bool includeCursor) const {
    const shot::Logger log(m_host);
    if (dst == nullptr || !isValidTarget(*dst)) {
        log.error("screen_capture: capture 的目标图不合法（空指针 / 格式 / 尺寸 / stride）");
        return 2;
    }
    if (m_wayland == nullptr) {
        log.error("screen_capture: Wayland 采集对象未创建（内存不足）");
        return 3;
    }

    // include_cursor 在 Wayland 上没有可执行语义：光标由门户/合成器决定，应用既
    // 不能读光标也不能把光标画进别人的帧。第一次 capture 如实 WARN 一次，不假装
    // 支持；配置值仍然保留，切回 Windows 时照常生效。
    (void)includeCursor;
    m_wayland->warnCursorLimitation();

    std::int32_t width = 0;
    std::int32_t height = 0;
    std::string error;
    std::int32_t code = m_wayland->ensureReady(width, height, error);
    if (code != 0) {
        log.error("screen_capture: " + error);
        return code;
    }
    // describe 与 capture 是两次独立调用，中间可能改分辨率/换输出：尺寸对不上
    // 返回 2，绝不按错的尺寸写像素（与 Windows 版同一语义）。
    if (width != dst->width || height != dst->height) {
        log.error("screen_capture: 目标尺寸与当前 Wayland 流不一致，拒绝写入");
        return 2;
    }

    code = m_wayland->fetchFrame(*dst, kFrameWaitMs, error);
    if (code != 0) {
        log.error("screen_capture: " + error);
        return code;
    }
    return 0;
}

void ScreenCapture::setCaptureWindow(bool window) {
    if (m_wayland != nullptr) {
        m_wayland->setCaptureWindow(window);
    }
}

bool ScreenCapture::isValidTarget(const ShotImage& dst) {
    if (dst.data == nullptr || dst.width <= 0 || dst.height <= 0) {
        return false;
    }
    // 本能力只写 BGRA8。宿主可能比本插件老、结构里还没有 pixel_format 字段，
    // 那种情况按当年的约定当成 BGRA8 处理（STYLE.md §6.5 的降级）。
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
