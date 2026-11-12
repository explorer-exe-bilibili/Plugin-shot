#pragma once

// ============================================================================
//  region_portal.h —— xdg-desktop-portal 的 Screenshot 门户（Linux/Wayland 分支）
//
//  Wayland 不允许应用直接读屏幕，唯一的通用途径是桌面门户：
//  org.freedesktop.portal.Screenshot 由合成器/门户后端完成截图，首次会弹系统授权框。
//  本文件只声明"调一次门户、拿回一个临时 PNG 路径"这一件事；解码/裁剪在
//  region_image.h，缓存与绘制在 platform 层（region_background_wayland.cpp）。
//
//  只有 Linux/Wayland 分支编译本文件对应的 .cpp；它不包含在任何公共头里。
// ============================================================================

#include <functional>
#include <string>

#include "plugin_api.h"

namespace region_selector {

/**
 * @brief 一次 Screenshot 门户调用的结果分类。
 *
 * 分类只影响日志级别与"是否继续本次框选"：门户不可用/失败要 WARN，用户取消/拒绝
 * 与超时安静放弃（不弹窗、不发布任何事件）。
 */
enum class PortalScreenshotStatus {
    Ok,          /**< 成功：pngPath 是门户给的临时 PNG 路径。 */
    Cancelled,   /**< 用户取消或拒绝授权：静默放弃，不算错误。 */
    TimedOut,    /**< 等授权等太久：已取消 D-Bus 请求，放弃本次框选。 */
    Unavailable, /**< 门户服务不可用（没装 / 没运行 / 桌面不支持）：WARN。 */
    Failed,      /**< 其它门户错误：WARN，并记录门户给的原因。 */
};

/**
 * @brief 调用一次 xdg-desktop-portal 的 Screenshot 门户，拿回整屏截图的临时 PNG 路径。
 *
 * 实现要点（与需求"方案 B"一致）：
 *  - 使用 libportal 的 `xdp_portal_take_screenshot()` + `XDP_SCREENSHOT_FLAG_NONE`
 *    （非交互模式 = 整屏截图；交互模式会多弹一次选区域/选窗口，这里不要）；
 *  - 门户 API 是异步的，本类在调用线程里临时建一个线程私有的 GMainContext + GMainLoop，
 *    阻塞到回调或超时为止，不碰宿主的主循环；
 *  - 超时时间与取消语义见 region_portal.cpp 的常量注释。
 *
 * 调用线程：插件的 UI 线程（阻塞期间覆盖层还没显示，宿主其它线程不受影响）。
 */
class PortalScreenshot {
public:
    /**
     * @brief 绑定宿主服务表（只用于写日志）。
     * @param[in] host 宿主服务表；可为 nullptr（日志静默丢弃）。
     */
    explicit PortalScreenshot(const ShotHostServices* host);

    /**
     * @brief 请求一次整屏截图。
     *
     * 成功时 `pngPath` 指向门户创建的临时文件：调用方**读完必须删除**它
     * （门户把文件的所有权交给调用方）。
     *
     * @param[out] pngPath 成功时的临时 PNG 路径；失败时清空。
     * @param[in]  shouldAbort 可选的中断检查（每 100ms 调一次）：返回 true 时取消门户
     *             请求并按"取消"返回。用于宿主正在退出时不要卡在授权框上等满超时。
     * @return 见 PortalScreenshotStatus。
     * @note 不抛异常：门户/GLib 的失败都转成返回值；日志为中文（e2e 按中文断言）。
     */
    PortalScreenshotStatus take(std::string& pngPath,
                                const std::function<bool()>& shouldAbort) const;

private:
    const ShotHostServices* m_host = nullptr; /**< 宿主服务表，只读，可为 nullptr。 */
};

/**
 * @brief 判断当前桌面会话是不是 Wayland。
 *
 * 运行时探测 `WAYLAND_DISPLAY`（最直接）与 `XDG_SESSION_TYPE`（display manager 设置，
 * 有些精简环境只有其中一个）：任一命中就按 Wayland 处理。Linux 同一份 .so 同时编进
 * Wayland 与 X11 两套实现：返回 true 走门户定格图（B 方案），false 走 X11 的
 * XGetImage 快照 + 全屏覆盖层（见 README「Linux（X11）」）。
 *
 * @return true = 当前是 Wayland 会话；false = 不是（按 X11 处理；无显示会话时
 *         X11 初始化会失败并记日志）。
 */
bool isWaylandSession();

}  // namespace region_selector
