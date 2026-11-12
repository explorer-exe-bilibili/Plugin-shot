#pragma once

// ============================================================================
//  include/global_keys.h —— 全局按键的接口（平台接口头）
//
//  Windows 实现见 src/global_keys_windows.cpp：
//    · 一个隐藏消息窗口 + 一条消息线程，负责全部 RegisterHotKey；
//    · 触发键（配置项 hotkey）从插件 create 起常驻，可随时按新配置重注册；
//    · 会话期间注册 VK_RETURN（保存）/ VK_ESCAPE（取消），会话结束立即注销；
//    · RegisterHotKey 失败时退回 WH_KEYBOARD_LL 低级钩子（同样只在会话期间安装）。
//
//  本头文件不包含 <windows.h>：组合键解析是纯函数，虚拟键码用数值表达。
//
//  ⚠ 已知代价：会话期间 VK_RETURN 被 RegisterHotKey 全局吃掉，任何程序里的
//  Enter 都不会再到达它们自己（这是"Enter 保存"这个交互的必然代价，README 里
//  写明了；想避免只能改用带修饰键的组合）。
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>

#include "plugin_api.h"

namespace long_capture {

/** @brief 修饰键位掩码：解析结果与 Win32 的 MOD_* 是两套值，由实现负责转换。 */
constexpr std::uint32_t kKeyModCtrl = 1u << 0;  /**< Ctrl（Control 也接受）。 */
constexpr std::uint32_t kKeyModAlt = 1u << 1;   /**< Alt。 */
constexpr std::uint32_t kKeyModShift = 1u << 2; /**< Shift。 */
constexpr std::uint32_t kKeyModWin = 1u << 3;   /**< Win（Windows 也接受）。 */

/**
 * @brief 一个解析后的组合键：修饰键位集合 + 主键虚拟键码。
 *
 * virtualKey 用 Win32 的 VK_* 数值（A-Z / 0-9 就是 ASCII，F1..F12 = 0x70..0x7B）。
 * 解析器本身不包含 <windows.h>，单独测试时可以只靠数值断言。
 */
struct KeyCombo {
    std::uint32_t modifiers = 0;  /**< 上面 kKeyMod* 的按位或；0 = 没有修饰键。 */
    std::uint32_t virtualKey = 0; /**< Win32 VK 数值；0 = 非法（解析失败时保持 0）。 */
    std::string label;            /**< 规范化显示名，如 "Ctrl+Alt+L" / "F9"；日志用。 */
};

/**
 * @brief 把 "Ctrl+Alt+L" 这类文本解析成 KeyCombo（纯函数、无状态、任意线程可调）。
 *
 * 语法（大小写不敏感）：[Ctrl+][Alt+][Shift+][Win+]<A-Z / 0-9 / F1..F12>；
 * 修饰键可省略、顺序任意，主键必须恰好一个。解析只做语法校验，不注册任何系统
 * 热键——注册失败（被别的程序占用）在注册路径上记 WARN 并跳过，不影响其它功能。
 *
 * @param[in]  text  配置值；nullptr / 空串视为非法。
 * @param[out] out   解析成功时写入；失败时被清空。
 * @param[out] error 失败时的中文原因；成功时清空。
 * @return true = 语法合法。
 */
bool parse_key_combo(const char* text, KeyCombo& out, std::string& error);

/**
 * @brief 全局按键：触发键常驻、保存/取消键只在会话期间注册。
 *
 * 线程模型：start() 起消息线程并等隐藏窗口就绪；registerTrigger /
 * beginSession / endSession 可在任意线程调用（只置期望状态 + PostMessage，
 * 真正的 RegisterHotKey / UnregisterHotKey / SetWindowsHookEx 全部在消息线程
 * 上执行）。回调运行在消息线程上，实现里只允许置原子标志 + 唤醒会话线程。
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
     * @return true = 线程与窗口已就绪；false = 建窗/建线程失败（触发键与保存键都不可用）。
     */
    bool start(const ShotHostServices* host);

    /** @brief 注销全部热键/钩子、退出消息循环、注销窗口类并 join；可重复调用。 */
    void stop();

    /**
     * @brief 按组合键文本（重新）注册触发键；空串 = 注销触发键（只用托盘/菜单）。
     * @param[in] comboText 配置项 hotkey 的当前值。
     * @return true = 已把注册请求交给消息线程；false = 消息线程未启动或文本为空。
     * @note 注册结果（被占用/语法错）只记日志，不阻塞调用方；重复调用是幂等的。
     */
    bool registerTrigger(const char* comboText);

    /**
     * @brief 设置触发键回调（在消息线程上调用）；可在 start() 前后调用。
     * @param[in] callback 回调；可为 nullptr（表示不关心）。
     * @param[in] user     回传给 callback 的上下文。
     */
    void setTriggerCallback(Callback callback, void* user);

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
