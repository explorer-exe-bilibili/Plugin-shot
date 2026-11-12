// Linux 契约层：校验宿主入参、按会话类型分派到 Wayland 或 X11 后端、映射返回码、写日志。
//
// 为什么运行期分派：宿主在 Linux 只有一份二进制，用户的桌面可能是 Wayland
// （Ubuntu 24.04 默认）也可能是 X11；插件的 .so 不能按会话类型分别编译，只能看
// 环境变量。会话类型在进程生命周期内不会变，所以只在构造 ScreenCapture 时判定一次。
//
// 判定规则（与 wayland_capture.cpp 的 WaylandCapture::isWaylandSession 保持一致）：
//   WAYLAND_DISPLAY 非空，或 XDG_SESSION_TYPE=wayland（大小写不敏感）→ Wayland；
//   其余（包括两个变量都没设置的无头环境）→ X11。两个变量都看，是因为精简环境/
//   登录管理器可能只设置其中一个；无头环境走 X11 分支后由 X11 层报"无法连接
//   X server"，返回码同样是 3，不在契约层伪造成功。
//
// 两条后端的尺寸语义不同：
//   Wayland：门户一次只共享一个源（输出或窗口），origin 恒为 (0,0)，尺寸取
//            PipeWire 实际协商的帧尺寸；
//   X11：整个默认 X screen（DisplayWidth / DisplayHeight），origin 恒为 (0,0)。
// 共同点：describe 报的尺寸与 capture 写的必须是同一套；中间变了就返回 2。

#include "screen_capture.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

#include "shot_plugin_helpers.h"
#include "wayland_capture.h"
#include "x11_capture.h"

namespace screen_capture {
namespace {

// Wayland 的 capture 最多等新帧的时间。200ms 是"允许一次短抖动"与"不让宿主 worker
// 线程卡太久"之间的折中；X11 路径不使用它。
constexpr std::int32_t kFrameWaitMs = 200;

/**
 * @brief 判断当前桌面会话是不是 Wayland。
 *
 * 契约层用它在两个后端之间分派；Wayland 后端自己会再校验一次（防御环境在构造后
 * 被改）。两处规则必须保持同步，否则"选择的后端"和"后端自己的判断"会打架。
 *
 * @return true = Wayland 会话；false = X11 或无显示会话。
 */
bool isWaylandSession() {
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (waylandDisplay != nullptr && waylandDisplay[0] != '\0') {
        return true;
    }
    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType == nullptr) {
        return false;
    }
    std::string type(sessionType);
    for (char& character : type) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return type == "wayland";
}

}  // namespace

ScreenCapture::ScreenCapture(const ShotHostServices* host) noexcept : m_host(host) {
    m_waylandSession = isWaylandSession();
    const shot::Logger log(m_host);
    if (m_waylandSession) {
        // 门户对象与两个后台线程只在 Wayland 会话创建；X11 会话完全不碰 libportal。
        m_wayland.reset(new (std::nothrow) WaylandCapture(host));
        log.info("screen_capture: 检测到 Wayland 会话，使用 xdg-desktop-portal ScreenCast + PipeWire");
    } else {
        log.info("screen_capture: 当前不是 Wayland 会话，使用 X11（XShmGetImage / XGetImage）");
    }
}

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
    return m_waylandSession ? describeWayland(out) : describeX11(out);
}

std::int32_t ScreenCapture::capture(ShotImage* dst, bool includeCursor) const {
    const shot::Logger log(m_host);
    if (dst == nullptr || !isValidTarget(*dst)) {
        log.error("screen_capture: capture 的目标图不合法（空指针 / 格式 / 尺寸 / stride）");
        return 2;
    }
    return m_waylandSession ? captureWayland(dst, includeCursor) : captureX11(dst, includeCursor);
}

std::int32_t ScreenCapture::describeWayland(ShotCaptureInfo* out) const {
    const shot::Logger log(m_host);
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

std::int32_t ScreenCapture::captureWayland(ShotImage* dst, bool includeCursor) const {
    const shot::Logger log(m_host);
    if (m_wayland == nullptr) {
        log.error("screen_capture: Wayland 采集对象未创建（内存不足）");
        return 3;
    }

    // include_cursor 在 Wayland 上没有可执行语义：光标由门户/合成器决定，应用既
    // 不能读光标也不能把光标画进别人的帧。第一次 capture 如实 WARN 一次，不假装
    // 支持；配置值仍然保留，切回 Windows/X11 时照常生效。
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
    // 返回 2，绝不按错的尺寸写像素（与 Windows/X11 版同一语义）。
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

std::int32_t ScreenCapture::describeX11(ShotCaptureInfo* out) const {
    const shot::Logger log(m_host);
    ShotCaptureInfo geometry = {};
    const std::int32_t code = X11Capture::queryGeometry(geometry, log);
    if (code != 0) {
        return code;
    }

    // 区域/单显示器采集：与 Windows 版一样，v1 只描述整个 X screen。DisplayWidth/
    // DisplayHeight 已经包含 XRandR/Xinerama 合并后的多显示器，取景由宿主抓整屏后裁剪。
    out->width = geometry.width;
    out->height = geometry.height;
    out->origin_x = geometry.origin_x;
    out->origin_y = geometry.origin_y;
    out->pixel_format = SHOT_PIXEL_FORMAT_BGRA8;
    return 0;
}

std::int32_t ScreenCapture::captureX11(ShotImage* dst, bool includeCursor) const {
    const shot::Logger log(m_host);
    // 每次 capture 重新读一次几何：describe 与 capture 是两次独立调用，中间可能
    // 改分辨率/拔显示器/旋转屏幕；尺寸对不上返回 2，绝不按错的尺寸写像素。
    ShotCaptureInfo geometry = {};
    const std::int32_t code = X11Capture::queryGeometry(geometry, log);
    if (code != 0) {
        return code;
    }
    if (geometry.width != dst->width || geometry.height != dst->height) {
        log.error("screen_capture: 目标尺寸与当前 X11 screen 不一致，拒绝写入");
        return 2;
    }
    if (!X11Capture::captureTo(*dst, geometry, includeCursor, log)) {
        return 3;
    }
    return 0;
}

void ScreenCapture::setCaptureWindow(bool window) {
    // X11 分支没有创建 Wayland 对象，source 配置对它是空操作；Windows 版本没有这个函数。
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
