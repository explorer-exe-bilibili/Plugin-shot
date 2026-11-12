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
 * @brief 截屏结果通知器：把 capture.done / capture.failed 变成一次系统通知。
 *
 * 两个平台各有一套后端，但公共接口与生命周期相同：
 *  · Windows：隐藏消息窗口 + 隐藏占位托盘图标 + `Shell_NotifyIconW(NIF_INFO)`
 *    气泡，所有 Shell 调用都在插件自己的通知线程上执行；
 *  · Linux：`libnotify`（`notify_notification_new` + `notify_notification_show`）
 *    走桌面通知的 D-Bus 服务，不建窗口、不持线程，回调里同步提交（Wayland
 *    同样可用）。
 *
 * 本插件没有 ui_provider 的 init/deinit 生命周期，create(index, host) 是唯一能
 * 拿到宿主服务表的时机，所以 start() 在实例创建路径里调用、stop() 在 destroy 路径。
 * 事件总线回调（宿主分发线程）在 Windows 上只做"拷进队列 + PostMessage 唤醒"，
 * 在 Linux 上直接提交通知；stop()/destroy 返回后一定已退订、停线程（Windows）、
 * 释放 libnotify（Linux），宿主之后 FreeLibrary 不会留下指向本 DLL 的回调。
 */
class NotifyToast {
public:
    /** @brief 构造：还没有通知后端（Windows 线程/窗口或 Linux libnotify 状态），start() 之后才可用。 */
    NotifyToast();

    /** @brief 析构：等价于 stop()，不留下通知线程、隐藏图标或 libnotify 全局状态。 */
    ~NotifyToast();

    /** @brief 不可拷贝：持有线程 / 窗口 / libnotify 状态，复制没有意义。 */
    NotifyToast(const NotifyToast&) = delete;
    /** @brief 不可赋值。 */
    NotifyToast& operator=(const NotifyToast&) = delete;

    /**
     * @brief 绑定宿主、订阅 capture.done / capture.failed，并初始化平台通知后端。
     *
     * Windows 先订阅再起通知线程：订阅之后到达的事件先入队，窗口就绪后统一处理，
     * 不会漏。Linux 在订阅前 `notify_init`（失败只 WARN 并返回 false，不把"没有
     * 通知服务"升级成"插件加载失败"）。
     *
     * @param[in] host   宿主服务表；可为 nullptr（老宿主），此时安静降级。
     * @param[in] config 配置项；生命周期必须覆盖本对象（插件状态里两者同生共死）。
     * @param[in] i18n   语言表；生命周期必须覆盖本对象。可为 nullptr（初始化失败），
     *                   此时气泡文案用 tr 的中文 fallback。
     * @return true = 通知后端已就绪；false = 无事件总线或初始化失败。
     * @note 建线程可能抛异常（线程资源耗尽），这里就地兜住，异常绝不越过 DLL 边界。
     */
    bool start(const ShotHostServices* host, NotifyConfig* config, const shot::I18n* i18n);

    /**
     * @brief 退订事件总线并释放平台通知后端。
     *
     * Windows 让通知线程退出并 join，最后删掉隐藏托盘图标；Linux 调
     * `notify_uninit` 释放 libnotify 的全局状态。可重复调用；返回时不会再有
     * 回调进入本对象，宿主可以安全地卸载 DLL。
     */
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  /**< PIMPL：平台相关状态（Win32 句柄 / libnotify）。 */
};

}  // namespace notify
