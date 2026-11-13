// region_background_x11 —— Linux/X11 版的"覆盖层背景"：XGetImage 整屏快照 + GL 纹理。
//
// 为什么必须用快照而不是透明窗口：
//   这是 Windows 分支用真机踩出来的结论（AGENTS.md §7 坑 #2）：透明帧缓冲在第一次
//   显示时可用，hide → show 一轮之后合成就会失效，变成整屏黑 + 只剩十字线。现在两个
//   平台的覆盖层都是**不透明窗口 + 触发瞬间的桌面快照**：遮罩/挖空/十字线/折角画在
//   快照上，效果等价于"透出桌面"，但不依赖任何窗口合成特性。
//
// 抓哪一块：
//   这里抓**整个 X screen**（root 窗口 0,0 起，DisplayWidth x DisplayHeight），而不是
//   只抓鼠标所在显示器。原因有两个：
//     1. 绘制层可以按覆盖层窗口的实际位置从整屏图里裁一块（UV 子矩形），窗口被 WM
//        挪了几像素也不会错位；
//     2. 请求的显示器矩形在根坐标里可能是负数/超出（窗口被摆放得比屏幕大），整屏图
//        裁剪时天然夹住，不会读到 root 之外。
//   代价是 4K 双屏时一次约 30MB 的临时内存，上传纹理后立刻释放（不常驻）。
//
// 合成器（Mutter / KWin 等）下 root 窗口不是最终画面：
//   XComposite 打开后，窗口内容被重定向到 offscreen pixmap，由合成器画进
//   **Composite Overlay Window**，root 窗口里常常只剩桌面底色。直接对 root 做
//   XGetImage 就会得到一块纯色（真机现象：遮罩是灰色的、看不到桌面内容）。所以拿到
//   root 快照后先做 32x32 均匀采样的"疑似纯色"检查；纯色就改用
//   XCompositeGetOverlayWindow() 拿最终合成画面再抓一次，用完立即
//   XCompositeReleaseOverlayWindow()。没有该回退能力时（运行期服务器无 XComposite
//   扩展，或构建期没找到 libxcomposite，宏 SHOT_REGION_NO_XCOMPOSITE）保持 root 画面
//   并 WARN，退化为纯色遮罩但不影响框选功能。最终画面仍疑似纯色时再 WARN 一行
//   （带分辨率 / 首像素），方便用户回传日志定位。
//
// 像素格式：XImage 是 ZPixmap，bpp=32 且字节序与客户端一致时按 32 位字直接取
// R/G/B mask；其它少见格式（16/24bpp、字节序相反）退回 XGetPixel（慢但正确）。
// root 与覆盖窗口的 visual 可能不同（覆盖窗口常见 depth 32），所以两者共用
// imageToRgba()：通道一律按 XImage 的 mask 归一化，不假设具体的 RGB 排列。
//
// 失败语义（需求要求）：XGetImage 失败只 WARN，覆盖层退化为**纯色底 + 遮罩**，
// 拖框/窗口吸附/发布区域都照常工作——背景只是观感，不是功能依赖。
//
// 线程：GL 上下文在 UI 线程上当前有效（createWindow 里 makeCurrent），本函数只在
// UI 线程的 startSelection 里调用。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#if !defined(SHOT_REGION_NO_XCOMPOSITE)
#include <X11/extensions/Xcomposite.h>
#endif

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

/** @brief "疑似纯色"检测的采样网格：每轴 32 个点（4K 屏也只取 1024 个像素，开销可忽略）。 */
inline constexpr int kBlankSampleGrid = 32;

/** @brief 采样点每个通道的最大差值小于它就算纯色：留 1 级余量吸收量化/抖动差异。 */
inline constexpr int kBlankChannelDelta = 2;

/**
 * @brief 从一个像素值里取出 8 位通道：按 mask 右移后线性归一化到 0~255。
 *
 * @param[in] pixel XImage 里的一个像素值。
 * @param[in] mask  该通道在像素值里的位掩码（来自 XImage::red_mask 等）。
 * @return 0~255；mask 为 0（该 X visual 没有这个通道）时返回 0。
 */
std::uint8_t channelFromMask(unsigned long pixel, unsigned long mask) {
    if (mask == 0) {
        return 0;
    }
    int shift = 0;
    while (((mask >> shift) & 1ul) == 0ul) {
        ++shift;
    }
    const unsigned long value = (pixel & mask) >> shift;
    const unsigned long maximum = mask >> shift;
    if (maximum == 0) {
        return 0;
    }
    return static_cast<std::uint8_t>((value * 255ul + maximum / 2ul) / maximum);
}

/** @brief 宿主字节序（XImage::byte_order 用它判断"数据能不能直接按 32 位字读"）。 */
bool hostIsLittleEndian() {
    const std::uint16_t probe = 1;
    return *reinterpret_cast<const std::uint8_t*>(&probe) == 1;
}

/** @brief 把颜色拼成 `#RRGGBB`（日志用；不引入 <iomanip>，保持与插件其它 TU 一致）。 */
std::string hexColor(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", static_cast<unsigned int>(red),
                  static_cast<unsigned int>(green), static_cast<unsigned int>(blue));
    return std::string(buffer);
}

/** @brief 一张 XImage 的"是否疑似空白画面"采样结果（同时带首像素，日志回传用）。 */
struct ImageColorProbe {
    bool blank = false;     /**< 所有采样点几乎同色：多半是合成器没把画面画进 root。 */
    std::uint8_t red = 0;   /**< 首像素（左上角）的红色通道。 */
    std::uint8_t green = 0; /**< 首像素（左上角）的绿色通道。 */
    std::uint8_t blue = 0;  /**< 首像素（左上角）的蓝色通道。 */
};

/**
 * @brief 在整张图上均匀采样 32x32 个点，判断画面是否"几乎纯色"。
 *
 * 纯色画面是"合成器把内容画在 offscreen"的最强信号：真实桌面（壁纸、窗口、面板）
 * 不可能在 1024 个均匀采样点上每通道都只差 1。采样用 XGetPixel 而不是转换后的
 * 缓冲，是为了在决定要不要回退之前就能判定，避免先做一次全图转换。
 *
 * @param[in] image ZPixmap 的 XImage；可以为空（返回默认结果：不判为纯色）。
 * @return 判定结果 + 首像素颜色。
 */
ImageColorProbe probeImageColor(XImage* image) {
    ImageColorProbe probe;
    if (image == nullptr || image->width <= 0 || image->height <= 0) {
        return probe;
    }
    int minimum[3] = {255, 255, 255};
    int maximum[3] = {0, 0, 0};
    bool first = true;
    for (int row = 0; row < kBlankSampleGrid; ++row) {
        // 均匀铺满整张图（不是只采左上角）：合成器没画内容时整张图都是同一个底色。
        const int y = (image->height * row) / kBlankSampleGrid;
        for (int column = 0; column < kBlankSampleGrid; ++column) {
            const int x = (image->width * column) / kBlankSampleGrid;
            const unsigned long pixel = XGetPixel(image, x, y);
            const int channels[3] = {
                channelFromMask(pixel, image->red_mask),
                channelFromMask(pixel, image->green_mask),
                channelFromMask(pixel, image->blue_mask),
            };
            if (first) {
                // 第一个采样点就是 (0,0)：日志里的"首像素"与整屏图左上角一致。
                probe.red = static_cast<std::uint8_t>(channels[0]);
                probe.green = static_cast<std::uint8_t>(channels[1]);
                probe.blue = static_cast<std::uint8_t>(channels[2]);
                first = false;
            }
            for (int index = 0; index < 3; ++index) {
                if (channels[index] < minimum[index]) {
                    minimum[index] = channels[index];
                }
                if (channels[index] > maximum[index]) {
                    maximum[index] = channels[index];
                }
            }
        }
    }
    probe.blank = maximum[0] - minimum[0] < kBlankChannelDelta &&
                  maximum[1] - minimum[1] < kBlankChannelDelta &&
                  maximum[2] - minimum[2] < kBlankChannelDelta;
    return probe;
}

/**
 * @brief 把 ZPixmap 的 XImage 转成自顶向下的 RGBA8。
 *
 * root 窗口与 XComposite 覆盖窗口的像素格式可能不同（覆盖窗口常见 depth 32/ARGB），
 * 两条路径共用这一份转换：通道一律按 XImage 的 red/green/blue mask 归一化。
 *
 * @param[in]  image  源图；调用方保证非空且尺寸 > 0。
 * @param[out] pixels 目标缓冲，按 image 的宽高重新分配（RGBA 交错，行序自顶向下）。
 */
void imageToRgba(XImage* image, std::vector<std::uint8_t>& pixels) {
    const int width = image->width;
    const int height = image->height;
    pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    const bool nativeByteOrder = (image->byte_order == LSBFirst) == hostIsLittleEndian();
    if (image->bits_per_pixel == 32 && image->bytes_per_line >= width * 4 && nativeByteOrder) {
        // 常见路径：32bpp ZPixmap，直接按行按 32 位字取（memcpy 避免未对齐读取）。
        for (int y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const unsigned char*>(image->data) +
                              static_cast<std::size_t>(y) *
                                  static_cast<std::size_t>(image->bytes_per_line);
            std::uint8_t* destination =
                pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
            for (int x = 0; x < width; ++x) {
                std::uint32_t word = 0;
                std::memcpy(&word, row + static_cast<std::size_t>(x) * 4u, sizeof(word));
                destination[x * 4 + 0] = channelFromMask(word, image->red_mask);
                destination[x * 4 + 1] = channelFromMask(word, image->green_mask);
                destination[x * 4 + 2] = channelFromMask(word, image->blue_mask);
                destination[x * 4 + 3] = 0xFFu;
            }
        }
    } else {
        // 少见格式（16/24bpp、大端服务器等）：交给 Xlib 取像素值，慢但一定正确。
        for (int y = 0; y < height; ++y) {
            std::uint8_t* destination =
                pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
            for (int x = 0; x < width; ++x) {
                const unsigned long pixel = XGetPixel(image, x, y);
                destination[x * 4 + 0] = channelFromMask(pixel, image->red_mask);
                destination[x * 4 + 1] = channelFromMask(pixel, image->green_mask);
                destination[x * 4 + 2] = channelFromMask(pixel, image->blue_mask);
                destination[x * 4 + 3] = 0xFFu;
            }
        }
    }
}

#if !defined(SHOT_REGION_NO_XCOMPOSITE)

/** @brief 抓覆盖窗口期间被 Xlib 报告的错误码（0 = 没有错误）。 */
int g_trappedXErrorCode = 0;

/**
 * @brief 短临界区内的 X 错误处理器：只记错误码，不让 Xlib 按默认方式退出进程。
 *
 * Xlib 的默认错误处理器遇到 BadMatch / BadDrawable 会直接 `exit(1)`。覆盖窗口在
 * 合成器没运行 / 抓取途中合成器重启时可能不可用，XGetImage 正好会报这两类错误，
 * 所以必须临时接管——否则一次背景快照失败会把宿主一起带走。
 *
 * @param[in] display 出错连接（不区分）。
 * @param[in] event   错误详情；可能为空。
 * @return 0 = 已处理（Xlib 继续运行）。
 */
int trapXError(Display* /*display*/, XErrorEvent* event) {
    g_trappedXErrorCode = (event != nullptr) ? event->error_code : 0;
    return 0;
}

/**
 * @brief 抓 XComposite 覆盖窗口（合成器画出来的最终画面）。
 *
 * 没有扩展 / 覆盖窗口不可见 / 抓取失败都只 WARN 并返回 nullptr，调用方保持 root 快照。
 * 覆盖窗口只有合成器在运行时才有内容；不可见时 XGetImage 会触发 BadMatch，所以先
 * 查 map_state，并把这段调用放进错误接管里。
 *
 * @param[in] display     GLFW 的 X11 Display。
 * @param[in] root        默认 screen 的 root 窗口。
 * @param[in] width       抓取宽度（X screen 宽：覆盖窗口与屏幕同尺寸）。
 * @param[in] height      抓取高度。
 * @param[in] log         日志：失败原因要能回传，方便真机排查。
 * @return 新的 XImage（调用方负责 XDestroyImage）；失败返回 nullptr。
 */
XImage* captureCompositeOverlayImage(Display* display, Window root, int width, int height,
                                     const shot::Logger& log) {
    int eventBase = 0;
    int errorBase = 0;
    if (XCompositeQueryExtension(display, &eventBase, &errorBase) == 0) {
        log.warn("region_selector：X 服务器没有 XComposite 扩展，无法抓合成器覆盖窗口；"
                 "保持 root 窗口画面（可能是纯色）");
        return nullptr;
    }

    // 进程级错误处理器只在下面这几行里接管，抓完 XSync 把异步错误消费掉再装回原处理器
    // （绝不能长期替换：GLFW / settings_ui 也走同一个 Xlib 连接）。
    const XErrorHandler previous = XSetErrorHandler(&trapXError);
    g_trappedXErrorCode = 0;
    const Window overlay = XCompositeGetOverlayWindow(display, root);
    XImage* image = nullptr;
    bool viewable = false;
    if (overlay != None) {
        XWindowAttributes attributes{};
        viewable = XGetWindowAttributes(display, overlay, &attributes) != 0 &&
                   attributes.map_state == IsViewable;
        if (viewable) {
            image = XGetImage(display, overlay, 0, 0, static_cast<unsigned int>(width),
                              static_cast<unsigned int>(height), AllPlanes, ZPixmap);
        }
        XCompositeReleaseOverlayWindow(display, root);
    }
    XSync(display, False);
    const int errorCode = g_trappedXErrorCode;
    XSetErrorHandler(previous);

    if (overlay == None) {
        log.warn("region_selector：XCompositeGetOverlayWindow 返回空窗口；保持 root 窗口画面");
        return nullptr;
    }
    if (!viewable) {
        log.warn("region_selector：XComposite 覆盖窗口不可见（合成器没在运行？）；"
                 "保持 root 窗口画面");
        return nullptr;
    }
    if (image == nullptr) {
        log.warn("region_selector：截取 XComposite 覆盖窗口失败（X 错误码 " +
                 std::to_string(errorCode) + "）；保持 root 窗口画面");
        return nullptr;
    }
    return image;
}

#endif  // !SHOT_REGION_NO_XCOMPOSITE

}  // namespace

bool RegionWindow::Impl::x11CaptureBackground() {
    const shot::Logger log(host);
    // 上一次会话的纹理可能还挂在 Impl 上（窗口与纹理跨会话复用）：先释放，这样本次
    // 快照失败时绘制层看到的是"没有背景"，而不是把上一次的旧桌面画出来。
    releaseBackgroundTexture();
    Display* display = glfwGetX11Display();
    if (display == nullptr) {
        log.warn("region_selector：X11 Display 不可用，无法截取覆盖层背景（退化为纯色遮罩）");
        return false;
    }
    const int screen = DefaultScreen(display);
    const Window root = RootWindow(display, screen);
    const int screenWidth = DisplayWidth(display, screen);
    const int screenHeight = DisplayHeight(display, screen);
    if (screenWidth <= 0 || screenHeight <= 0) {
        log.warn("region_selector：X11 root 窗口尺寸非法，无法截取覆盖层背景（退化为纯色遮罩）");
        return false;
    }

    // 覆盖层此刻还是隐藏（未 map）的，所以这张图就是用户看到的桌面。
    XImage* image = XGetImage(display, root, 0, 0, static_cast<unsigned int>(screenWidth),
                              static_cast<unsigned int>(screenHeight), AllPlanes, ZPixmap);
    if (image == nullptr) {
        log.warn("region_selector：XGetImage 截取 X11 桌面失败（退化为纯色遮罩），"
                 "框选与发布区域不受影响");
        return false;
    }

    // 合成器下 root 里常常只剩桌面底色：先判"疑似纯色"，纯色就改抓覆盖窗口。
    ImageColorProbe probe = probeImageColor(image);
    if (probe.blank) {
        log.warn("region_selector：XGetImage 得到疑似空白画面（纯色），尝试 XComposite 覆盖窗口");
#if defined(SHOT_REGION_NO_XCOMPOSITE)
        log.warn("region_selector：构建时没有 XComposite 支持（SHOT_REGION_NO_XCOMPOSITE），"
                 "无法尝试覆盖窗口，保持 root 窗口画面；安装 libxcomposite-dev 后重新构建可启用");
#else
        XImage* overlayImage =
            captureCompositeOverlayImage(display, root, screenWidth, screenHeight, log);
        if (overlayImage != nullptr) {
            XDestroyImage(image);
            image = overlayImage;
            probe = probeImageColor(image);
            log.info("region_selector：已改从 XComposite 覆盖窗口截取覆盖层背景"
                     "（root 窗口疑似纯色）");
        }
#endif
    }

    std::vector<std::uint8_t> pixels;
    imageToRgba(image, pixels);
    const int imageWidth = image->width;
    const int imageHeight = image->height;
    XDestroyImage(image);

    // 再保险：最终画面仍是纯色（扩展缺失、覆盖窗口不可用、服务器确实只画了底色）就
    // 明确 WARN 一行并带分辨率/首像素，用户回传日志即可判断是哪种情况。
    if (probe.blank) {
        log.warn("region_selector：X11 快照仍疑似纯色（" + std::to_string(imageWidth) + "x" +
                 std::to_string(imageHeight) + "，首像素 " +
                 hexColor(probe.red, probe.green, probe.blue) +
                 "），覆盖层可能看不到桌面内容；请回传本行日志");
    }

    // 上传成 GL 纹理。GL 上下文在 UI 线程上当前有效；行序自顶向下，与纹理 v=0 在
    // 顶部的约定一致，不需要翻转。
    // 先清掉之前调用可能留下的 GL 错误（例如 ImGui 初始化阶段的试探）：否则下面的
    // glGetError 会把旧错误当成"这次纹理上传失败"，误删一张本来可用的纹理。
    while (glGetError() != GL_NO_ERROR) {
    }
    if (backgroundTexture == 0) {
        glGenTextures(1, &backgroundTexture);
    }
    glBindTexture(GL_TEXTURE_2D, backgroundTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, imageWidth, imageHeight, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        // 超过 GPU 纹理上限（多屏 8K）等：删掉半成品，按"没有背景"处理，绘制层会用纯色底。
        glDeleteTextures(1, &backgroundTexture);
        backgroundTexture = 0;
        log.warn("region_selector：X11 快照纹理上传失败（超过 GPU 纹理上限？），"
                 "覆盖层退化为纯色遮罩");
        return false;
    }
    backgroundWidth = imageWidth;
    backgroundHeight = imageHeight;
    log.info("region_selector：已截取覆盖层背景（X11 整屏快照 " +
             std::to_string(backgroundWidth) + "x" + std::to_string(backgroundHeight) +
             "，按覆盖层窗口位置裁剪显示）");
    return true;
}

}  // namespace region_selector
