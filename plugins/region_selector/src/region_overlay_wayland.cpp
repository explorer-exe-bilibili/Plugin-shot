// region_overlay_wayland —— Wayland 覆盖层的几何映射与结果提交。
//
// 两件事：
//  1. **窗口内容坐标 ↔ 截图图像像素**：覆盖层把整张 portal 截图等比缩放后居中显示
//     （两侧/上下留黑边），用户拖出来的框是窗口坐标，裁剪要换算回截图图像像素。
//     映射每帧按实际窗口尺寸重算（合成器可能改变窗口大小，不能假设"窗口 = 显示器"）。
//  2. **确认后的产出**：直接从内存里的整屏像素裁剪 → 写 PNG → 发布 capture.done
//     {path,width,height,processors:0}。Wayland 分支不发布 capture.request：
//     宿主流水线在 Linux 上也没有 screen_capture 可用，而且重新采集会再弹一次门户
//     授权框（需求明确只要一次授权、一次采集）。
//
// 窗口定位/全屏：Wayland 客户端不能摆放自己的窗口，也不能查询输出位置。窗口落在
// 哪个输出、是否真的全屏由合成器决定；这里只提出请求，并在日志里记录实际尺寸，
// 供真机验证时对照（README「Linux / Wayland」一节）。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>

#include <nlohmann/json.hpp>

#include "region_image.h"
#include "region_output.h"
#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

/** @brief 产出的通知主题：与宿主流水线的 capture.done 同一格式（document/CAPTURE.md §3）。 */
constexpr char kTopicCaptureDone[] = "capture.done";

}  // namespace

RegionWindow::Impl::WaylandLayout RegionWindow::Impl::overlayLayout() const {
    WaylandLayout layout;
    if (window == nullptr || sourceWidth <= 0 || sourceHeight <= 0) {
        return layout;
    }
    int windowWidth = 0;
    int windowHeight = 0;
    // 用窗口内容尺寸（不是 framebuffer）：ImGui 的坐标空间、glfwGetCursorPos 的返回值
    // 都是窗口内容坐标，绘制也按这套坐标走；framebuffer 缩放由 ImGui 后端在投影里处理。
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0) {
        return layout;
    }
    // 等比缩放适配：取两个方向的较小比例，保证整张图都看得见（不裁切、不拉伸）。
    const float scaleX = static_cast<float>(windowWidth) / static_cast<float>(sourceWidth);
    const float scaleY = static_cast<float>(windowHeight) / static_cast<float>(sourceHeight);
    layout.scale = std::min(scaleX, scaleY);
    layout.width = static_cast<float>(sourceWidth) * layout.scale;
    layout.height = static_cast<float>(sourceHeight) * layout.scale;
    // 居中：图比窗口窄/矮时两侧留黑边，拖框落在黑边上会被裁剪时夹掉。
    layout.offsetX = (static_cast<float>(windowWidth) - layout.width) * 0.5f;
    layout.offsetY = (static_cast<float>(windowHeight) - layout.height) * 0.5f;
    return layout;
}

ImageRect RegionWindow::Impl::selectionToImagePixels(int x, int y, int width, int height) const {
    ImageRect pixelRect;
    const WaylandLayout layout = overlayLayout();
    if (layout.scale <= 0.0f || sourceWidth <= 0 || sourceHeight <= 0) {
        return pixelRect;
    }
    // 四舍五入到最近的图像像素，再把两条边夹进图像范围：选区可能有一部分落在黑边上。
    const auto toImageX = [&layout](float value) {
        return static_cast<int>(std::lround((value - layout.offsetX) / layout.scale));
    };
    const auto toImageY = [&layout](float value) {
        return static_cast<int>(std::lround((value - layout.offsetY) / layout.scale));
    };
    const int left = std::clamp(toImageX(static_cast<float>(x)), 0, sourceWidth);
    const int right = std::clamp(toImageX(static_cast<float>(x + width)), 0, sourceWidth);
    const int top = std::clamp(toImageY(static_cast<float>(y)), 0, sourceHeight);
    const int bottom = std::clamp(toImageY(static_cast<float>(y + height)), 0, sourceHeight);
    pixelRect.x = std::min(left, right);
    pixelRect.y = std::min(top, bottom);
    pixelRect.width = std::abs(right - left);
    pixelRect.height = std::abs(bottom - top);
    return pixelRect;
}

void RegionWindow::Impl::prepareWaylandOverlay(const ScreenRect& monitor) {
    if (window == nullptr || monitor.width <= 0 || monitor.height <= 0) {
        return;
    }
    // Wayland 不允许客户端定位窗口：位置由合成器决定。这里先把尺寸对齐输出，
    // 让隐藏态渲染的第一帧比例就接近最终画面；真正的全屏在窗口显示后再请求。
    glfwSetWindowSize(window, monitor.width, monitor.height);
}

void RegionWindow::Impl::requestWaylandFullscreen() {
    if (window == nullptr) {
        return;
    }
    // 窗口显示之后再请求全屏：隐藏窗口上的 setWindowMonitor 在部分合成器上会被忽略。
    // 选输出与 monitorUnderCursor 同一套规则（当前输出 → 主显示器）。
    GLFWmonitor* monitor = glfwGetWindowMonitor(window);
    if (monitor == nullptr) {
        monitor = glfwGetPrimaryMonitor();
    }
    if (monitor == nullptr) {
        shot::Logger(host).warn("region_selector：找不到显示器，覆盖层保持普通窗口");
        return;
    }
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    if (mode == nullptr) {
        shot::Logger(host).warn("region_selector：显示器没有视频模式，覆盖层保持普通窗口");
        return;
    }
    // 位置参数在全屏请求里不起作用；refreshRate 用 GLFW_DONT_CARE，避免强制切刷新率。
    glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, GLFW_DONT_CARE);
    // 合成器可能接受，也可能把窗口留成普通窗口/带标题栏的样子（GNOME 不支持
    // xdg-decoration，无边框请求会被忽略）。记下实际尺寸，真机排查坐标问题时先看这行。
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    char text[256] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：已请求 Wayland 全屏，实际窗口尺寸 %dx%d（由合成器决定）",
                  windowWidth, windowHeight);
    shot::Logger(host).info(text);
}

void RegionWindow::Impl::commitWaylandSelection(int localX, int localY, int localWidth,
                                                int localHeight) {
    const shot::Logger log(host);
    // 先把选区换算到截图图像像素（此时窗口还在，布局用的是刚才那一帧的尺寸）。
    const ImageRect crop = selectionToImagePixels(localX, localY, localWidth, localHeight);
    // 覆盖层立刻收掉：接下来只剩内存运算与写盘，用户应该马上看到桌面恢复。
    // Wayland 分支不需要 Windows 版那 100ms 的"等遮罩从合成器消失"——没有第二次抓屏。
    hideOverlay();

    std::string savedPath;
    // 下面的字符串/向量分配与 PNG 编码都可能抛 bad_alloc；异常如果逃出 std::thread 的
    // 入口会直接 std::terminate，所以在这一层兜住：失败只记日志、不发布事件。
    try {
        if (crop.width < 1 || crop.height < 1 || sourcePixels.empty() || sourceWidth <= 0 ||
            sourceHeight <= 0) {
            log.warn("region_selector：选区换算到截图图像像素后为空或没有可裁剪的截图，按取消处理");
        } else {
            const std::string directory = resolveOutputDirectory(host);
            // directory 为空时原因已经在 resolveOutputDirectory 里记过
            // （配置目录不可建 / 找不到图片目录）。
            if (!directory.empty()) {
                const std::string path = makeOutputPngPath(directory);
                if (path.empty()) {
                    log.warn("region_selector：生成输出文件名失败（本地时间取不到？），本次不保存");
                } else if (!saveRgbaCropAsPng(sourcePixels.data(), sourceWidth, sourceHeight, crop,
                                              path)) {
                    log.warn("region_selector：裁剪 PNG 写盘失败：" + path);
                } else {
                    savedPath = path;
                }
            }
        }
    } catch (...) {
        log.warn("region_selector：保存框选结果时内存不足，本次放弃");
        savedPath.clear();
    }
    // 无论成功失败都释放整屏像素与纹理：下一次框选重新走门户截图。
    releaseBackgroundTexture();
    if (savedPath.empty()) {
        return;
    }
    // 宿主正在退出/热卸载：文件已经写好，但不要在 deinit 之后再发事件
    // （与 Windows 分支的 shouldQuit() 检查同理）。
    if (shouldQuit()) {
        log.info("region_selector：宿主正在退出，丢弃本次 capture.done（文件已保存：" + savedPath +
                 "）");
        return;
    }
    try {
        publishCaptureDone(savedPath, crop.width, crop.height);
    } catch (...) {
        // 文件已经完整写出：事件没发出去只影响通知/剪贴板，不能再抛。
        log.warn("region_selector：发布 capture.done 时内存不足（文件已保存：" + savedPath + "）");
    }
}

void RegionWindow::Impl::publishCaptureDone(const std::string& path, int width, int height) const {
    const shot::Logger log(host);
    shot::EventBus bus(host);
    if (!bus.available()) {
        log.warn("region_selector：宿主没有事件总线，框选结果无法通知其它插件（文件已保存）");
        return;
    }
    // 与宿主流水线的 capture.done 完全同格式：processors = 0（没有经过 image_processor 链）。
    const nlohmann::json payload = {
        {"path", path},
        {"width", width},
        {"height", height},
        {"processors", 0},
    };
    if (!bus.publish(kTopicCaptureDone, payload.dump())) {
        log.warn("region_selector：发布 capture.done 失败（队列满或参数非法），文件已保存");
        return;
    }
    char text[320] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：框选结果已保存 %s（%dx%d 图像像素），已发布 capture.done",
                  path.c_str(), width, height);
    log.info(text);
}

}  // namespace region_selector
