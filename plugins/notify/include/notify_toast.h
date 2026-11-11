#pragma once

#include <cstdint>
#include <memory>

#include "plugin_api.h"

namespace shot {

class I18n;

}  // namespace shot

namespace notify {

class NotifyConfig;

/**
 * @brief 消息提示器：隐藏消息窗口 + 隐藏托盘图标占位 + Shell 通知气泡。
 *
 * 职责只有一件：把 capture.done / capture.failed 变成一次系统通知。
 *  · 本插件没有 ui_provider 的 init/deinit 生命周期，create(index, host) 是唯一能
 *    拿到宿主服务表的时机，所以 start() 在实例创建路径里调用、stop() 在 destroy 路径；
 *  · 事件总线回调（宿主分发线程）只做"拷进队列 + PostMessage 唤醒"；所有 Win32 /
 *    Shell_NotifyIconW 调用都在本插件自己的通知线程上执行（Shell 通知不是线程安全的）；
 *  · stop() 返回前一定已退订、停线程、删掉隐藏图标——宿主之后 FreeLibrary 也不会
 *    留下指向本 DLL 窗口过程的窗口类。
 *
 * Win32 类型（HWND / HICON）全部关在 Impl 里（内部头 notify_toast_impl.h），
 * 这个公共头不包含 <windows.h>。
 */
class NotifyToast {
public:
    /** @brief 构造：还没有线程与隐藏窗口，start() 之后才可用。 */
    NotifyToast();

    /** @brief 析构：等价于 stop()，不留下通知线程与隐藏图标。 */
    ~NotifyToast();

    /** @brief 不可拷贝：持有线程与 Win32 句柄，复制没有意义。 */
    NotifyToast(const NotifyToast&) = delete;
    /** @brief 不可赋值。 */
    NotifyToast& operator=(const NotifyToast&) = delete;

    /**
     * @brief 绑定宿主、订阅 capture.done / capture.failed / app.quit，并起通知线程。
     *
     * 先订阅再起线程：订阅之后到达的事件先入队，窗口就绪后统一处理，不会漏。
     * 宿主没有事件总线（老宿主）时只记 WARN 并返回 false，不把"少一个服务"升级成
     * "插件加载失败"。
     *
     * @param[in] host   宿主服务表；可为 nullptr（老宿主），此时安静降级。
     * @param[in] config 配置项；生命周期必须覆盖本对象（插件状态里两者同生共死）。
     * @param[in] i18n   语言表；生命周期必须覆盖本对象。可为 nullptr（初始化失败），
     *                   此时气泡文案用 tr 的中文 fallback。
     * @return true = 通知线程与隐藏图标已就绪；false = 无事件总线或初始化失败。
     * @note 建线程可能抛异常（线程资源耗尽），这里就地兜住，异常绝不越过 DLL 边界。
     */
    bool start(const ShotHostServices* host, NotifyConfig* config, const shot::I18n* i18n);

    /**
     * @brief 退订、让通知线程退出并 join，最后删掉隐藏托盘图标。
     *
     * 可重复调用；返回时通知线程一定已经退出、隐藏图标一定已经删除。
     */
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  /**< PIMPL：Win32 线程/窗口/图标状态在内部头里。 */
};

}  // namespace notify
