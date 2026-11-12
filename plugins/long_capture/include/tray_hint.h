#pragma once

// ============================================================================
//  include/tray_hint.h —— 托盘气泡提示的接口（平台接口头）
//
//  为什么需要它：HUD 在"内容区四周都放不下"时不显示（内容区几乎占满屏幕的
//  场景），此时用户需要知道"现在可以滚动、Enter 保存 / Esc 取消"。托盘气泡是
//  唯一不占内容区、也不会被截进长图的提示方式。
//
//  Windows 实现见 src/tray_hint_windows.cpp：一个隐藏消息窗口 + 一个 NIS_HIDDEN
//  的占位托盘图标（Shell_NotifyIconW 的气泡必须挂在已添加的图标上），显示几秒后
//  自动删除图标并退出；窗口类在线程退出前 UnregisterClassW。
//
//  本头文件不包含 <windows.h>；非 Windows 平台不构建本插件。
// ============================================================================

#include <memory>
#include <string>

#include "plugin_api.h"

namespace long_capture {

/**
 * @brief 一次性的托盘气泡：show() 起一个短生命周期线程，dismiss()/定时器收尾。
 *
 * @note 只负责"提示"，不订阅任何事件；失败（explorer 没起来、Shell 拒绝）只写日志，
 *       调用方不因此中断长截图会话。
 */
class TrayHint {
public:
    /** @brief 构造空的提示对象；此时没有线程与图标。 */
    TrayHint();

    /** @brief 析构：等价于 dismiss()，不留下托盘图标、窗口与线程。 */
    ~TrayHint();

    /** @brief 不可拷贝：持有线程与托盘图标。 */
    TrayHint(const TrayHint&) = delete;
    /** @brief 不可赋值。 */
    TrayHint& operator=(const TrayHint&) = delete;

    /**
     * @brief 显示一条托盘气泡（阻塞到图标添加完成或失败，毫秒级）。
     *
     * @param[in] host  宿主服务表，只用来写日志；可为 nullptr。
     * @param[in] title 气泡标题（UTF-8）。
     * @param[in] text  气泡正文（UTF-8）。
     * @return true = 隐藏图标已添加、气泡已提交给 Shell；false = 初始化失败。
     * @note 气泡默认保留 4 秒后自动收起；重复调用会先 dismiss() 再重新显示。
     */
    bool show(const ShotHostServices* host, const std::string& title, const std::string& text);

    /** @brief 立即收起气泡、删除隐藏图标并 join 线程；可重复调用。 */
    void dismiss();

    /** @brief 是否仍有存活的提示线程。 */
    bool active() const noexcept;

private:
    struct Impl;                  /**< 窗口/线程/图标状态全部藏在 Impl 里。 */
    std::unique_ptr<Impl> m_impl; /**< PIMPL：公共头不出现 Win32 类型。 */
};

}  // namespace long_capture
