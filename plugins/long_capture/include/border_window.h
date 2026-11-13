#pragma once

// ============================================================================
//  include/border_window.h —— 长截图选区边框（平台接口头）
//
//  会话开始后，用户只看到 HUD，屏幕上的哪块区域正在被截没有视觉边界。本类在
//  目标内容区**外侧**画一圈 2px 的主题色描边，颜色取宿主设置 core/theme_color，
//  让用户一眼看清"正在截哪一块"。
//
//  平台：只有 Windows 实现（src/border_window_windows.cpp：无边框、置顶、点击
//  穿透的工具窗 + SetWindowRgn 挖掉中间，只保留描边）。非 Windows 平台整个插件
//  不构建（见 CMakeLists.txt），所以这里不提供会误导人的伪实现。
//
//  为什么不用半透明遮罩：整窗 WS_EX_LAYERED 半透明会闪、会挡操作；用窗口区域
//  "外矩形 - 内矩形"只保留 2px 描边，既不挡鼠标，也不遮内容区边缘一个像素。
//
//  线程：窗口只能在创建它的线程上销毁。会话线程调用 start()，必须在同一线程上
//  调用 stop()（AGENTS.md §7 第 6 条）；析构函数会兜底 stop()。
// ============================================================================

#include <cstdint>
#include <memory>
#include <string_view>

#include "plugin_api.h"
#include "screen_rect.h"

namespace long_capture {

/** @brief 边框宽度（物理像素）：画在目标内容区**外侧**，不占用内容区。 */
inline constexpr int kBorderWidth = 2;

/** @brief 主题色默认值：宿主配置 `core/theme_color` 缺失/非法时用它（与 region_selector 一致）。 */
inline constexpr char kDefaultThemeColorHex[] = "#4C9AFF";

/** @brief 默认主题色的 0xRRGGBB 形式。 */
inline constexpr std::uint32_t kDefaultThemeColorRgb = 0x4C9AFFu;

/**
 * @brief 解析 `#RRGGBB`（`#` 可省略）成 0xRRGGBB。
 *
 * 与 region_selector 的解析规则保持一致：只接受 6 位十六进制，不扩展 #RGB /
 * #AARRGGBB，让"非法时回退默认色"的边界保持明确。
 *
 * @param[in] text     配置值，如 "#3B82F6"。
 * @param[in] fallback 解析失败（或不是 6 位十六进制）时的返回值。
 * @return 0xRRGGBB，或 fallback。
 */
std::uint32_t parseThemeColorRgb(std::string_view text, std::uint32_t fallback);

/**
 * @brief 选区边框窗口：在目标内容区外侧画一圈 2px 的主题色描边。
 *
 * 窗口矩形 = 内容区外扩 kBorderWidth（物理像素），中间用窗口区域挖空，因此描边
 * 完全落在内容区之外：既不会遮住内容边缘，也不会被 ScreenGrabber 抓进长图。
 * 置顶、不激活、点击穿透，不参与截图，也不做 hide/show 抖动。
 *
 * @note 本类不是线程安全的：start() / stop() / active() 都只在会话线程上调用。
 */
class BorderWindow {
public:
    /**
     * @brief 构造空的边框对象：不建窗、不注册窗口类。
     * @param[in] host 宿主服务表，只用来写日志；可为 nullptr。
     */
    explicit BorderWindow(const ShotHostServices* host);

    /** @brief 析构：等价于 stop()，不留下窗口与窗口类。 */
    ~BorderWindow();

    /** @brief 不可拷贝：持有窗口与进程级窗口类。 */
    BorderWindow(const BorderWindow&) = delete;
    /** @brief 不可赋值。 */
    BorderWindow& operator=(const BorderWindow&) = delete;

    /**
     * @brief 显示内容区外侧的主题色边框；已在显示时会先收掉旧窗口。
     *
     * 建窗前把创建线程的 DPI 上下文兜到 PMv2 并恢复，保证窗口坐标就是物理像素，
     * 与采集/框选坐标完全一致。失败只记日志，不影响会话（HUD/按键照常工作）。
     *
     * @param[in] content  目标内容区（绝对屏幕坐标、物理像素）；宽高必须 > 0。
     * @param[in] colorRgb 描边颜色，0xRRGGBB。
     * @return true = 边框已显示；false = 参数非法或建窗/区域设置失败。
     */
    bool start(const ScreenRect& content, std::uint32_t colorRgb);

    /** @brief 销毁边框窗口并注销窗口类；可重复调用。 */
    void stop();

    /** @brief 边框是否正在显示（窗口已创建且未 stop）。 */
    bool active() const noexcept;

private:
    struct Impl;                  /**< 窗口/窗口类/颜色全部藏在 Impl 里。 */
    std::unique_ptr<Impl> m_impl; /**< PIMPL：公共头不出现 Win32 类型。 */
};

}  // namespace long_capture
