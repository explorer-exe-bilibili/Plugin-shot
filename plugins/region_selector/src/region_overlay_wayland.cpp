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
// 隐藏与提交时序（真机踩过的坑，不要"优化"掉）：
//   glfwHideWindow 在 Wayland 上只是把 unmap 请求写进 wl_display 的发送缓冲，真正
//   发出去靠下一次 glfwPollEvents/WaitEvents 里的 wl_display_flush。确认时必须
//   hideOverlay() + 立刻 poll，否则覆盖层留在屏幕上、UI 线程又进入空闲阻塞，
//   合成器的 ping 无人应答 → GNOME 报"应用没有响应"。
//   裁剪 + 编码 + 写盘放在工作线程（runWaylandCommit），UI 线程只负责隐藏、算布局、
//   释放 GL 纹理与起线程；纹理释放必须在 UI 线程（GL 上下文只在那里有效）。
//
// 窗口定位/全屏：Wayland 客户端不能摆放自己的窗口，也不能查询输出位置。窗口落在
// 哪个输出、是否真的全屏由合成器决定；这里只提出请求，并在日志里记录实际尺寸，
// 供真机验证时对照（README「Linux / Wayland」一节）。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
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

/** @brief 隐藏覆盖层后额外等一小段时间的秒数：留给合成器处理 unmap（下次 poll 之前）。 */
constexpr double kHideSettlePollSeconds = 0.02;

/**
 * @brief 一次 Wayland 提交的输入快照（由工作线程持有，不再碰 UI 线程状态）。
 *
 * 用 shared_ptr 传给工作线程：std::thread 构造失败（线程资源耗尽）时，构造函数的
 * 参数是否已经被移动由实现决定；持有一份拷贝才能保证失败后还能在 UI 线程上补做。
 */
struct WaylandCommitJob {
    const ShotHostServices* host = nullptr;  /**< 日志与事件总线（生命周期由 stop() 的 join 保证）。 */
    std::vector<std::uint8_t> pixels;        /**< 整屏 RGBA 像素（从 UI 线程 move 进来）。 */
    int sourceWidth = 0;                     /**< 源图宽度。 */
    int sourceHeight = 0;                    /**< 源图高度。 */
    ImageRect crop;                          /**< 要裁剪的图像像素矩形。 */
    std::string path;                        /**< 输出 PNG 路径（UI 线程已生成）。 */
    std::function<bool()> shouldAbort;       /**< 宿主退出检查（在 UI 线程上绑定）。 */
};

/**
 * @brief 发布 capture.done（与宿主流水线同格式：processors = 0）。
 *
 * 从工作线程调用是安全的：宿主的事件总线 publish 只做深拷贝入队，分发回到宿主
 * main 线程（见 src/hostServices.cpp 的线程模型注释）。
 *
 * @param[in] host  宿主服务表（可空：没有总线时只记 WARN）。
 * @param[in] path  已写好的 PNG 路径。
 * @param[in] width  产出宽度（图像像素）。
 * @param[in] height 产出高度（图像像素）。
 */
void publishCaptureDone(const ShotHostServices* host, const std::string& path, int width,
                        int height) {
    const shot::Logger log(host);
    shot::EventBus bus(host);
    if (!bus.available()) {
        log.warn("region_selector：宿主没有事件总线，框选结果无法通知其它插件（文件已保存）");
        return;
    }
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

/**
 * @brief 工作线程主体：裁剪 + PNG 编码 + 写盘 + 发布 capture.done。
 *
 * 为什么放到工作线程：整屏 4K 级别的 stb PNG 编码要几百毫秒到数秒。留在 UI 线程时
 * 这段时间里完全没人处理 Wayland 事件（合成器 ping 得不到回应，GNOME 会弹
 * "应用没有响应"），而且下一次触发也要等它做完。
 *
 * 这里只碰 job 自己的数据，不访问 Impl 的图形/窗口状态；宿主退出检查用的是 UI 线程
 * 绑定好的回调（stop() 会先 join 本线程，指针一定有效）。
 *
 * @param[in] job 提交任务（shared_ptr：构造线程失败时调用方还能自己执行）。
 */
void runWaylandCommit(std::shared_ptr<WaylandCommitJob> job) {
    if (job == nullptr) {
        return;
    }
    const shot::Logger log(job->host);
    const auto startedAt = std::chrono::steady_clock::now();
    bool saved = false;
    try {
        saved = saveRgbaCropAsPng(job->pixels.data(), job->sourceWidth, job->sourceHeight, job->crop,
                                  job->path);
    } catch (...) {
        // 工作线程里逃出异常会直接 std::terminate：这一层兜住，只记日志。
        log.warn("region_selector：保存框选结果时内存不足，本次放弃");
        saved = false;
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - startedAt)
                               .count();
    if (!saved) {
        log.warn("region_selector：裁剪 PNG 写盘失败：" + job->path + "（耗时 " +
                 std::to_string(static_cast<long long>(elapsedMs)) + " ms）");
        return;
    }
    char savedText[320] = {};
    std::snprintf(savedText, sizeof(savedText),
                  "region_selector：已保存框选 PNG %s %dx%d（编码+写盘 %lld ms，工作线程）",
                  job->path.c_str(), job->crop.width, job->crop.height,
                  static_cast<long long>(elapsedMs));
    log.info(savedText);
    // 宿主正在退出/热卸载：文件已经写好，但不要在 deinit 之后再发事件
    // （与 Windows 分支的 shouldQuit() 检查同理）。
    if (job->shouldAbort && job->shouldAbort()) {
        log.info("region_selector：宿主正在退出，丢弃本次 capture.done（文件已保存：" + job->path +
                 "）");
        return;
    }
    try {
        publishCaptureDone(job->host, job->path, job->crop.width, job->crop.height);
    } catch (...) {
        // 文件已经完整写出：事件没发出去只影响通知/剪贴板，不能再抛。
        log.warn("region_selector：发布 capture.done 时内存不足（文件已保存：" + job->path + "）");
    }
}

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

    // ---- 关键时序：先让"隐藏"真正送达合成器，再做裁剪/编码 ----
    // glfwHideWindow 在本平台只是把 xdg_toplevel 的销毁 + attach(NULL) 写进 wl_display
    // 的发送缓冲，请求要等下一次 glfw*Events() 里的 wl_display_flush 才发得出去。
    // 不 poll 会有两个真机症状（Ubuntu 26 实测）：
    //   ① 覆盖层画面一直留在屏幕上（用户以为"卡住了"）；
    //   ② 隐藏后 UI 线程阻塞在条件变量上，不再处理任何 Wayland 事件，合成器的 ping
    //      得不到回应，GNOME 直接报"应用没有响应"。
    // 这里连 poll 两次（第二次带上 20ms 超时，给合成器处理 unmap 的机会）；两次都
    // 只是把请求发出去并处理已到达的事件，不会长时间阻塞。
    hideOverlay();
    glfwPollEvents();
    glfwPollEvents();
    if (window != nullptr) {
        glfwWaitEventsTimeout(kHideSettlePollSeconds);
    }
    log.info("region_selector：覆盖层已请求隐藏（GLFW_VISIBLE=" +
             std::to_string((window != nullptr)
                                ? glfwGetWindowAttrib(window, GLFW_VISIBLE)
                                : 0) +
             "），开始编码");

    // 没有可裁剪的内容：按取消处理（不落盘、不发事件），与旧行为一致。
    if (crop.width < 1 || crop.height < 1 || sourcePixels.empty() || sourceWidth <= 0 ||
        sourceHeight <= 0) {
        log.warn("region_selector：选区换算到截图图像像素后为空或没有可裁剪的截图，按取消处理");
        releaseBackgroundTexture();
        return;
    }

    // 保存目录与文件名在 UI 线程解析：resolveOutputDirectory 会查宿主的列表服务
    // （线程归属由宿主决定），不要在工作线程里碰。
    const std::string directory = resolveOutputDirectory(host);
    // directory 为空时原因已经在 resolveOutputDirectory 里记过
    // （配置目录不可建 / 找不到图片目录）。
    if (directory.empty()) {
        releaseBackgroundTexture();
        return;
    }
    const std::string path = makeOutputPngPath(directory);
    if (path.empty()) {
        log.warn("region_selector：生成输出文件名失败（本地时间取不到？），本次不保存");
        releaseBackgroundTexture();
        return;
    }

    auto job = std::make_shared<WaylandCommitJob>();
    job->host = host;
    // 整屏像素 move 进任务：UI 线程不再持有它，工作线程独占读写。
    job->pixels = std::move(sourcePixels);
    job->sourceWidth = sourceWidth;
    job->sourceHeight = sourceHeight;
    job->crop = crop;
    job->path = path;
    job->shouldAbort = [this]() { return shouldQuit(); };

    // GL 纹理必须在 UI 线程释放（GL 上下文只在这里有效）；像素已经 move 走，不浪费拷贝。
    releaseBackgroundTexture();

    // 上一次提交可能还没写完：先 join，保证同一时刻只有一个任务在写盘。
    joinCommitThread();
    char startText[256] = {};
    std::snprintf(startText, sizeof(startText),
                  "region_selector：开始编码 PNG（工作线程）：裁剪 %dx%d @ (%d,%d) → %s",
                  crop.width, crop.height, crop.x, crop.y, path.c_str());
    log.info(startText);
    try {
        commitThread = std::thread(&runWaylandCommit, job);
    } catch (...) {
        // 线程资源耗尽等：退回 UI 线程同步保存（丢图比卡住更糟）。job 仍是有效 shared_ptr，
        // 无论线程构造过程中是否已经移动过参数，这里都还能自己执行。
        log.warn("region_selector：创建编码线程失败，改在 UI 线程同步保存");
        runWaylandCommit(job);
    }
}

}  // namespace region_selector
