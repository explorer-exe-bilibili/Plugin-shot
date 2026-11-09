#pragma once

// ============================================================================
//  include/tray_icon.h —— 托盘图标（shot.ui_provider 的公共接口）
//
//  托盘自己开线程、自己建隐藏消息窗口、自己调 Shell_NotifyIconW；宿主只调
//  start / stop / post，从不接触 HWND，也不知道插件有几个线程。事件总线的
//  订阅也由本对象管理（回调只把事件拷进队列，所有 UI 操作都在托盘线程上做）。
//
//  右键菜单完全数据驱动：内容来自宿主列表服务里的 tray_command_list，点击条目
//  只发布条目声明的事件；本类不认识任何具体命令。菜单的解析在 TrayMenu
//  （tray_menu.h），Win32 渲染在 tray_icon_menu.cpp。
//
//  Win32 类型（HWND / HICON / HMENU）全部关在 Impl 里（定义在内部头
//  tray_icon_impl.h），公共头不包含 <windows.h>。
// ============================================================================

#include <cstdint>
#include <memory>

#include "plugin_api.h"

namespace tray {

class TrayConfig;

/**
 * @brief Windows 托盘图标：一个图标 + 一个隐藏消息窗口 + 一个托盘线程。
 *
 * 对应契约里的 `shot.ui_provider`：宿主只调 init / deinit / post_to_plugin
 * （本类分别包装成 start / stop / post），生命周期与线程模型：
 *
 *   - start() 返回 true 时托盘图标已由 Shell 接受（NIM_ADD 成功），线程在跑；
 *   - 左键单击 = 发布列表里 `"default":true` 条目的事件（没有则发布
 *     capture.request）；右键 = 弹数据驱动菜单（tray_command_list，点击条目
 *     发布该条目声明的 event）；
 *   - 订阅 capture.done / capture.failed / lists.changed：回调在宿主分发线程上，
 *     只把主题与负载深拷贝后 PostMessage 唤醒托盘线程；气泡与菜单重建都在
 *     托盘线程上做；
 *   - stop() 退订事件总线、让消息循环退出、NIM_DELETE、join 线程，返回前
 *     图标与线程都已不存在——宿主热卸载/退出都靠它把资源收干净。
 *
 * @note 一个进程里只应创建一个 TrayIcon：托盘图标是进程级资源。
 */
class TrayIcon {
public:
    /** @brief 构造空的托盘对象；此时还没有线程与图标。 */
    TrayIcon();
    /** @brief 析构：等价于调用 stop()，不留下托盘线程与图标。 */
    ~TrayIcon();

    /** @brief 不可拷贝：持有托盘线程与进程级图标。 */
    TrayIcon(const TrayIcon&) = delete;
    /** @brief 不可赋值。 */
    TrayIcon& operator=(const TrayIcon&) = delete;

    /**
     * @brief 建隐藏消息窗口、添加托盘图标并启动托盘线程；返回时图标已就绪。
     *
     * 幂等：已在运行时直接返回 true（重复 init 是空操作）。启动时订阅事件总线
     * 主题 `capture.done` / `capture.failed` / `lists.changed`（回调只入队 +
     * PostMessage 唤醒），并在托盘线程里先构建一次右键菜单。
     *
     * @param[in] host   宿主服务表；可为 nullptr（老宿主没有服务表，托盘仍会出现，
     *                   只是没法发布/接收事件）。指针必须活到 stop() 返回。
     * @param[in] config 开关配置；可为 nullptr（按默认值"都通知"处理）。
     *                   指针必须活到 stop() 返回。
     * @return true = 托盘图标已添加、线程已就绪；false = 建线程/建窗口/加图标失败。
     * @note 建线程可能抛异常（线程资源耗尽），这里就地兜住，异常绝不越过 DLL 边界。
     */
    bool start(const ShotHostServices* host, TrayConfig* config);

    /**
     * @brief 退订事件总线、让托盘线程退出消息循环并 join，最后删掉托盘图标。
     *
     * 可重复调用；返回时图标与线程一定都已经收干净。
     */
    void stop();

    /**
     * @brief 任意线程可调：把事件排进托盘线程队列并立即唤醒它。
     *
     * 投 `host.shutdown` 会直接转成 stop()（宿主卸载 UI 插件前先发它，见
     * document/SETTINGS.md §3.1），其余主题入队后由托盘线程处理。
     *
     * @param[in] name 事件名；按契约是编译期静态字符串，不能为空。
     * @param[in] data 不透明事件数据，可为 nullptr（按空数据入队）。
     * @param[in] len  data 的字节数；data 非空时按此长度拷走。
     * @return 0 = 已入队/已处理；1 = 托盘还没就绪或正在关闭；2 = 入队/唤醒失败；
     *         -1 = 参数非法。
     * @note 事件名与 len 字节会在返回前拷进队列，投递方可立即释放自己的缓冲区。
     */
    std::int32_t post(const char* name, const char* data, std::uint32_t len);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl; /**< PIMPL：Win32 句柄、线程、事件队列全藏在 Impl 里。 */
};

}  // namespace tray
