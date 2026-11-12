#pragma once

// ============================================================================
//  include/hud.h —— 长截图 HUD 的接口（平台接口头）
//
//  HUD 只在 Windows 上实现（src/hud_windows.cpp：无边框置顶工具窗 + GDI 自绘），
//  本头文件不包含 <windows.h>：句柄用 std::uintptr_t 传，Win32 类型全部关在
//  Impl 里。非 Windows 平台不构建本插件（见 CMakeLists.txt）。
//
//  HUD 的职责边界：
//    · 摆在目标内容区之外（右侧 / 左侧 / 下方 / 上方，完全落在工作区内、
//      与内容区至少留 12px），四周都放不下时**不建窗口**，由调用方降级成
//      "只靠键盘 + 托盘气泡提示"；
//    · 自绘状态行、5Hz 缩略图与「保存」「取消」按钮；
//    · 鼠标滚轮转发给目标窗口，避免"滚到 HUD 上就不动了"；
//    · 可拖动，但拖动时被钳制在内容区之外。
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>

#include "plugin_api.h"
#include "screen_rect.h"

namespace shot {
class I18n;  // 多语言：定义在 shot_plugin_helpers/shot_i18n.h，只在本类 .cpp 里用完整类型
}  // namespace shot

namespace long_capture {

/**
 * @brief 长截图 HUD：一个独立线程 + 无边框置顶工具窗，全部 GDI 自绘。
 *
 * 线程模型：start() 起 HUD 线程并等窗口就绪；会话线程通过 setStatus /
 * setThumbnail / showSaved 投递更新（内部只拷数据 + PostMessage），stop() 让
 * 消息循环退出并 join。按钮点击回调运行在 HUD 线程上，实现里只允许置原子标志 /
 * 唤醒会话线程，不能做重活。
 *
 * @note 窗口类属于进程、不随 DLL 卸载消失：线程退出前必须 UnregisterClassW
 *       （AGENTS.md §5.6 / §7 第 6 条，历史上热卸载踩过悬空 WndProc）。
 */
class Hud {
public:
    /** @brief 按钮回调类型：user 是 start() 传入的上下文。 */
    using ActionCallback = void (*)(void* user);

    /** @brief 构造空的 HUD 对象；此时没有线程与窗口。 */
    Hud();

    /** @brief 析构：等价于 stop()，不留下窗口、窗口类与线程。 */
    ~Hud();

    /** @brief 不可拷贝：持有线程与窗口资源。 */
    Hud(const Hud&) = delete;
    /** @brief 不可赋值。 */
    Hud& operator=(const Hud&) = delete;

    /**
     * @brief 尝试在内容区之外摆放 HUD 并显示；放不下时不创建任何窗口。
     *
     * @param[in] host        宿主服务表，HUD 只用来写日志；可为 nullptr。
     * @param[in] i18n        语言表；可为 nullptr（按钮文案退回英文 key 的兜底文本）。
     * @param[in] content     目标窗口客户区（物理像素屏幕坐标）；HUD 必须完全在它外面。
     * @param[in] targetHwnd  目标窗口句柄（HWND 的整型形式），滚轮转发用。
     * @param[in] onSave      点「保存」时回调；可为 nullptr。
     * @param[in] onCancel    点「取消」时回调；可为 nullptr。
     * @param[in] user        回传给上面两个回调的上下文。
     * @return true = HUD 已显示；false = 四周都放不下（或建窗失败），调用方应降级。
     * @note 本方法阻塞到 HUD 线程完成建窗与摆放（毫秒级）。
     */
    bool start(const ShotHostServices* host, const shot::I18n* i18n, const ScreenRect& content,
               std::uintptr_t targetHwnd, ActionCallback onSave, ActionCallback onCancel,
               void* user);

    /** @brief 让 HUD 线程退出、销毁窗口并注销窗口类；可重复调用。 */
    void stop();

    /** @brief HUD 是否仍处于显示状态（线程已启动且未 stop）。 */
    bool active() const noexcept;

    /**
     * @brief 更新状态行（任意线程可调，只拷贝字符串 + PostMessage）。
     * @param[in] main    主状态行（如"段数 12 · 高度 4800 px"）。
     * @param[in] warning 告警行（如"缺口 2 处"）；空串表示不显示告警行。
     */
    void setStatus(const std::string& main, const std::string& warning);

    /**
     * @brief 更新缩略图（任意线程可调）。
     * @param[in] rgba   紧凑 RGBA 预览图；必须 width×height×4 字节。
     * @param[in] width  预览宽度，必须 > 0。
     * @param[in] height 预览高度，必须 > 0。
     * @note 内部整块拷贝一份再交给 HUD 线程，调用方可以立刻复用缓冲。
     */
    void setThumbnail(const std::uint8_t* rgba, int width, int height);

    /**
     * @brief 显示"已保存：路径"，2 秒后自动关闭 HUD。
     * @param[in] utf8Path 最终长图的 UTF-8 绝对路径。
     */
    void showSaved(const std::string& utf8Path);

    /**
     * @brief 显示失败原因，2.5 秒后自动关闭 HUD。
     * @param[in] utf8Reason 中文原因（HUD 只负责显示，不翻译）。
     */
    void showFailure(const std::string& utf8Reason);

private:
    struct Impl;                  /**< 窗口/线程/绘制状态全部藏在 Impl 里。 */
    std::unique_ptr<Impl> m_impl; /**< PIMPL：公共头不出现 Win32 类型。 */
};

}  // namespace long_capture
