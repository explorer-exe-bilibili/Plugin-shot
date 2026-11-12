// region_overlay_x11 —— Linux/X11 覆盖层的定位、实际几何日志与确认提交。
//
// 与 Windows 分支的区别只在"坐标从哪来"：
//   · Windows 用 glfwGetWindowPos + 虚拟桌面原点（SM_XVIRTUALSCREEN）换算；
//   · X11 只有一块 X screen，root 原点就是 (0,0)，窗口内容坐标 + 窗口在 root 里的
//     位置就是最终 region。X 像素即物理像素，content scale 不参与坐标（只放大字号）。
//   提交之后的流程完全一致：普通框选发布带 region 的 capture.request，由宿主流水线
//   裁剪落盘；region.select.request 发起的选择只回 region.selected（purpose 透传）。
//
// 提交时序与 Windows 相同，不能省：先 glfwHideWindow + glfwPollEvents，再等
// kHideSettleDelayMs。X11 合成器（尤其开 compositing 的桌面）同样是异步合成的，
// 隐藏函数返回不代表覆盖层已经从画面上消失；宿主 worker 收到事件就抓屏，中间没有
// 同步点，等太短会把遮罩截进图里。
//
// 窗口定位：X11 客户端可以自己摆窗口，所以不像 Wayland 那样请求全屏，而是直接
// setSize + setPos 铺满目标显示器（含任务栏区域）。不使用 glfwSetWindowMonitor：
// 那会走 WM 的全屏状态切换，多一次合成/模式变化。
//
// 为什么不做 Windows 的 +1 像素：那是 Windows 把"矩形刚好等于显示器"的窗口判成
// 全屏窗口（fullscreen optimization）的专有坑，X11 没有这条规则；铺满即精确覆盖。

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <GLFW/glfw3.h>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {

void RegionWindow::Impl::x11PlaceOverlay(const ScreenRect& monitor) {
    if (window == nullptr || monitor.width <= 0 || monitor.height <= 0) {
        return;
    }
    // 位置/尺寸都是 X screen 坐标（= 像素）。窗口是隐藏状态，这里只改属性；真正
    // 显示在 startSelection 的"先渲染两帧再 show"之后。
    glfwSetWindowSize(window, monitor.width, monitor.height);
    glfwSetWindowPos(window, monitor.x, monitor.y);
}

void RegionWindow::Impl::x11LogOverlayGeometry() const {
    if (window == nullptr) {
        return;
    }
    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    char text[320] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：X11 覆盖层窗口 pos=(%d,%d) size=%dx%d 目标显示器=(%d,%d) "
                  "%dx%d（WM 可能再次调整位置，坐标一律按实际窗口位置换算）",
                  windowX, windowY, windowWidth, windowHeight, monitorRect.x, monitorRect.y,
                  monitorRect.width, monitorRect.height);
    shot::Logger(host).info(text);
}

void RegionWindow::Impl::x11CommitSelection(int localX, int localY, int localWidth,
                                            int localHeight) {
    const shot::Logger log(host);
    // hideOverlay() 会清空 selectionPurpose，先拷出来决定发 capture.request 还是
    // region.selected。普通框选（空串）= 交给宿主流水线；非空 = 选择握手。
    const std::string purpose = selectionPurpose;
    int windowX = 0;
    int windowY = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    ScreenRect region;
    region.x = windowX + localX;
    region.y = windowY + localY;
    region.width = localWidth;
    region.height = localHeight;
    char text[256] = {};
    std::snprintf(text, sizeof(text),
                  "region_selector：X11 确认选区 region={x:%d,y:%d,width:%d,height:%d}"
                  "（X screen 坐标 = 物理像素）",
                  region.x, region.y, region.width, region.height);
    log.info(text);

    // ---- 关键时序：先隐藏覆盖层，等它真的从画面上消失，再发布 ----
    hideOverlay();
    glfwPollEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(kHideSettleDelayMs));
    // 等待期间宿主可能已经开始卸载本插件：不要在 deinit 之后再发事件。
    if (shouldQuit()) {
        log.info("region_selector：宿主正在退出，丢弃本次已确认的区域");
        return;
    }
    if (!purpose.empty()) {
        // 选择握手（长截图目标选择等）：只回 region.selected，不发 capture.request，
        // 由发起方自己采集。
        publishPurposeSelection(purpose, region);
    } else {
        publishRegion(region);
    }
}

}  // namespace region_selector
