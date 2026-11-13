#pragma once

// ============================================================================
//  include/tray_icon.h —— 托盘图标（shot.ui_provider 的公共接口）
//
//  托盘自己开线程、自己管理平台对象；宿主只调 start / stop / post，从不接触
//  HWND / GDBus 对象，也不知道插件有几个线程。事件总线的订阅也由本对象管理
//  （回调只把事件拷进队列并唤醒插件线程，所有 UI/协议操作都在插件线程上做）。
//
//  托盘只管"入口"：图标 + 菜单。成功/失败的提示气泡不归托盘管——它们由独立
//  的消息提示插件 plugins/notify 订阅 capture.done / capture.failed 后弹，
//  避免同一件事被两个插件各弹一次。本插件不订阅任何 capture.* 结果事件。
//
//  菜单完全数据驱动：内容来自宿主列表服务里的 tray_command_list，点击条目只
//  发布条目声明的事件；本类不认识任何具体命令。菜单的解析在 TrayMenu
//  （tray_menu.h），两个平台共用。
//
//  平台实现（同一个公共接口，按平台互斥编译）：
//    · Windows：Win32 托盘图标（Shell_NotifyIconW + 隐藏消息窗口）——
//      src/tray_icon.cpp / tray_icon_window.cpp / tray_icon_icon.cpp /
//      tray_icon_menu.cpp / tray_icon_events.cpp，默认用宿主 exe 的命名资源
//      IDI_ICON1，`icon` 配置可换成 .ico 文件；左键执行 default 命令
//      （没有则 capture.request），右键弹数据驱动菜单。
//    · Linux：StatusNotifierItem（org.kde.StatusNotifierItem）+ Canonical
//      dbusmenu，走 GIO/GDBus —— src/tray_icon_linux.cpp / tray_sni_linux.cpp /
//      tray_dbusmenu_linux.cpp，图标用主题图标名 camera-photo-symbolic
//      （`icon` 配置在 Linux 上被忽略），左键 Activate 执行 default 命令
//      （没有则发布 settings.open），菜单由 SNI 宿主直接读 dbusmenu。
//
//  平台类型全部关在 Impl 里（Windows 见内部头 tray_icon_impl.h，Linux 见
//  tray_icon_linux_impl.h），公共头不包含 <windows.h> 或 <gio/gio.h>。
// ============================================================================

#include <cstdint>
#include <memory>

#include "plugin_api.h"

namespace shot {
class I18n;
}  // namespace shot

namespace tray {

class TrayConfig;

/**
 * @brief 托盘图标：一个图标 + 一个数据驱动菜单 + 一条插件自己的线程。
 *
 * 对应契约里的 `shot.ui_provider`：宿主只调 init / deinit / post_to_plugin
 * （本类分别包装成 start / stop / post），生命周期与线程模型：
 *
 *   - start() 返回 true 时插件线程已拉起（Windows 还会同步等到图标被 Shell
 *     接受；Linux 的 SNI 注册是异步的，start 不阻塞宿主 init）；
 *   - Windows：左键 = 发布列表里 `"default":true` 条目的事件（没有则发布
 *     capture.request）；右键 = 弹数据驱动菜单（tray_command_list）；
 *   - Linux：SNI 的 Activate = default 条目；没有 default（core 预置项都没有）
 *     则发布 settings.open。菜单由 SNI 宿主调用 dbusmenu 的 GetLayout 渲染，
 *     点击条目发布条目声明的事件；
 *   - 订阅 lists.changed：回调在宿主分发线程上，只把主题与负载深拷贝后唤醒
 *     插件线程；菜单重建都在插件线程上做；
 *   - 不弹任何气泡：成功/失败提示由独立的消息提示插件负责；
 *   - stop() 退订事件总线、让插件线程退出并 join，返回前图标/菜单/线程都已
 *     不存在——宿主热卸载/退出都靠它把资源收干净。
 *
 * @note 一个进程里只应创建一个 TrayIcon：托盘图标是进程级资源。
 */
class TrayIcon {
public:
    /** @brief 构造空的托盘对象；此时还没有线程与图标。 */
    TrayIcon();
    /** @brief 析构：等价于调用 stop()，不留下插件线程与图标。 */
    ~TrayIcon();

    /** @brief 不可拷贝：持有插件线程与进程级图标。 */
    TrayIcon(const TrayIcon&) = delete;
    /** @brief 不可赋值。 */
    TrayIcon& operator=(const TrayIcon&) = delete;

    /**
     * @brief 启动托盘后端：订阅事件总线、拉起插件线程并创建/添加图标。
     *
     * 幂等：已在运行时直接返回 true（重复 init 是空操作）。启动时订阅事件总线
     * 主题 `lists.changed`（回调只入队 + 唤醒），并在插件线程里先构建一次菜单。
     *
     * @param[in] host   宿主服务表；可为 nullptr（老宿主没有服务表，托盘仍会出现，
     *                   只是没法发布/接收事件）。指针必须活到 stop() 返回。
     * @param[in] config 托盘配置；可为 nullptr（按默认值）。指针必须活到 stop() 返回。
     *                   Windows 用它选 .ico 图标；Linux 后端忽略（用主题图标）。
     * @param[in] i18n   多语言解析器；可为 nullptr（按兜底中文）。指针必须活到 stop() 返回。
     * @return true = 插件线程已就绪；false = 建线程/建上下文失败。
     * @note 建线程可能抛异常（线程资源耗尽），这里就地兜住，异常绝不越过 DLL 边界。
     */
    bool start(const ShotHostServices* host, TrayConfig* config, shot::I18n* i18n);

    /**
     * @brief 退订事件总线、让插件线程退出并 join，最后删掉图标/注销 SNI。
     *
     * 可重复调用；返回时图标与线程一定都已经收干净（Linux 还会注销 SNI 对象、
     * 释放公共名字并销毁私有 D-Bus 连接）。
     */
    void stop();

    /**
     * @brief 任意线程可调：请求按最新配置重设图标。
     *
     * Windows：配置可能由宿主的 apply_stored 在启动阶段直接推送（那时还没有
     * config.applied 事件），也可能来自设置页应用；两种路径都走这里。
     * Linux：图标来自桌面图标主题（IconName），没有"换图标文件"路径，本调用
     * 是空操作（`icon` 配置在 Linux 上被忽略，见 README §5）。
     */
    void requestIconRefresh();

    /**
     * @brief 任意线程可调：把事件排进插件线程队列并立即唤醒它。
     *
     * 投 `host.shutdown` 会直接转成 stop()（宿主卸载 UI 插件前先发它，见
     * document/SETTINGS.md §3.1）；`lists.changed` 与事件总线走同一条队列，
     * 其余主题在 Linux 上是不需要处理的空操作。
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
    std::unique_ptr<Impl> m_impl; /**< PIMPL：平台句柄、线程、事件队列全藏在 Impl 里。 */
};

}  // namespace tray
