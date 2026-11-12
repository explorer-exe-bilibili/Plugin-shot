#pragma once

// ============================================================================
//  include/global_keys.h —— 会话内按键的接口（平台接口头）
//
//  Windows 实现见 src/global_keys_windows.cpp：
//    · 一个隐藏消息窗口 + 一条消息线程，负责会话期间全部 RegisterHotKey；
//    · **只注册会话按键**：VK_RETURN（保存）/ VK_ESCAPE（取消），会话结束立即注销；
//    · RegisterHotKey 失败时退回 WH_KEYBOARD_LL 低级钩子（同样只在会话期间安装）；
//    · 长期触发键（如 Ctrl+Alt+L）不在这里注册：它由 hotkey 插件读取
//      long_capture 贡献的 hotkey_list 后统一管理（见 document/LISTS.md）。
//
//  本头文件不包含 <windows.h>：虚拟键码用数值表达，Win32 类型全部关在 Impl 里。
//
//  ⚠ 已知代价：会话期间 VK_RETURN 被 RegisterHotKey 全局吃掉，任何程序里的
//  Enter 都不会再到达它们自己（这是"Enter 保存"这个交互的必然代价，README 里
//  写明了；想避免只能改用带修饰键的组合）。
// ============================================================================

#include <cstdint>
#include <memory>

#include "plugin_api.h"

namespace long_capture {

/**
 * @brief 会话内全局按键：保存（Enter）/ 取消（Esc），只在会话期间注册。
 *
 * 线程模型：start() 起消息线程并等隐藏窗口就绪；beginSession / endSession
 * 可在任意线程调用（只置期望状态 + PostMessage，真正的 RegisterHotKey /
 * UnregisterHotKey / SetWindowsHookEx 全部在消息线程上执行）。回调运行在消息
 * 线程上，实现里只允许置原子标志 + 唤醒会话线程。
 */
class GlobalKeys {
public:
    /** @brief 按键回调类型：user 是注册时传入的上下文。 */
    using Callback = void (*)(void* user);

    /** @brief 构造空的按键对象；此时没有线程与注册。 */
    GlobalKeys();

    /** @brief 析构：等价于 stop()，不留下热键、钩子、窗口与线程。 */
    ~GlobalKeys();

    /** @brief 不可拷贝：持有线程与进程级热键资源。 */
    GlobalKeys(const GlobalKeys&) = delete;
    /** @brief 不可赋值。 */
    GlobalKeys& operator=(const GlobalKeys&) = delete;

    /**
     * @brief 建隐藏消息窗口并启动消息线程。
     * @param[in] host 宿主服务表，只用来写日志；可为 nullptr。
     * @return true = 线程与窗口已就绪；false = 建窗/建线程失败（会话按键不可用）。
     */
    bool start(const ShotHostServices* host);

    /** @brief 注销全部热键/钩子、退出消息循环、注销窗口类并 join；可重复调用。 */
    void stop();

    /**
     * @brief 开始一个会话：注册 Enter（保存）/ Esc（取消），失败时装低级键盘钩子。
     * @param[in] onSave   保存键回调；可为 nullptr。
     * @param[in] onCancel 取消键回调；可为 nullptr。
     * @param[in] user     回传给两个回调的上下文。
     */
    void beginSession(Callback onSave, Callback onCancel, void* user);

    /** @brief 结束会话：立即注销 Enter / Esc，并拆除可能存在的低级键盘钩子。 */
    void endSession();

private:
    struct Impl;                  /**< 窗口/线程/热键 id 全部藏在 Impl 里。 */
    std::unique_ptr<Impl> m_impl; /**< PIMPL：公共头不出现 Win32 类型。 */
};

}  // namespace long_capture
