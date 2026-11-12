#pragma once

// ============================================================================
//  include/hotkey_window.h —— 全局热键窗口（shot.ui_provider 的公共接口）
//
//  插件自己开线程、自己建隐藏消息窗口、自己调 RegisterHotKey；宿主只调
//  start / stop / post，从不接触 HWND，也不知道插件开了线程。事件总线的订阅
//  （config.applied / hotkey.suspend / hotkey.resume）由本对象管理：回调运行在
//  宿主分发线程上，只置"需要重注册"标记并 PostMessage 唤醒热键线程，注册/解析
//  全部在热键线程上做。hotkey_list 变化的订阅在 HotkeyCatalog 里（create 阶段
//  建立），变化后通过 change handler 让本对象重注册。
//
//  热键只发事件，不直接调用任何插件：具体主题与负载来自 hotkey_list 条目，
//  内置三条是 capture.request（空）/ capture.request + {"mode":"region"} /
//  settings.open（空）。
//
//  Win32 类型（HWND / HINSTANCE）全部关在 Impl 里（定义在内部头
//  hotkey_window_impl.h），公共头不包含 <windows.h>。平台实现各有内部头：
//  Linux/X11 是 hotkey_window_x11_impl.h（XGrabKey），macOS 等没有全局热键能力
//  的平台用 hotkey_window_stub.cpp（只 WARN，不注册）。X11 实现在运行期拿不到
//  Display（纯 Wayland 会话）时也走同样的降级行为，不让插件加载失败。
// ============================================================================

#include <cstdint>
#include <memory>

#include "plugin_api.h"

namespace hotkey {

class HotkeyCatalog;
class HotkeyConfig;

/**
 * @brief 全局热键：一个隐藏消息窗口 + 一个热键线程 + 任意条数的注册。
 *
 * 对应契约里的 `shot.ui_provider`：宿主只调 init / deinit / post_to_plugin
 * （本类分别包装成 start / stop / post），生命周期与线程模型：
 *
 *   - start() 返回 true 时热键线程已就绪、列表里的热键已按当前配置注册（非法/
 *     被占用的项只 warn 跳过，不影响 start 成功）；
 *   - 订阅 config.applied：设置页应用后重新按目录快照注册；订阅
 *     hotkey.suspend / hotkey.resume：设置页录入组合键时暂停（撤销全部注册）/
 *     恢复（按当前配置重新注册）；
 *   - 收到 WM_HOTKEY 只发布对应事件，不直接调用任何插件；
 *   - stop() 退订事件总线、撤掉全部热键、让消息循环退出、join 线程；返回前
 *     窗口、热键与线程都已不存在——宿主热卸载/退出都靠它把资源收干净。
 *
 * @note 全局热键是进程级资源：同一个组合键在一个进程里只能注册一次。本插件
 *       按 hotkey_list 顺序注册，冲突时先到者生效、后者 warn 跳过。
 */
class HotkeyWindow {
public:
    /** @brief 构造空的热键对象；此时还没有线程与窗口。 */
    HotkeyWindow();
    /** @brief 析构：等价于调用 stop()，不留下热键线程与注册。 */
    ~HotkeyWindow();

    /** @brief 不可拷贝：持有线程与进程级热键注册。 */
    HotkeyWindow(const HotkeyWindow&) = delete;
    /** @brief 不可赋值。 */
    HotkeyWindow& operator=(const HotkeyWindow&) = delete;

    /**
     * @brief 建隐藏消息窗口、按目录快照注册热键并启动热键线程。
     *
     * 幂等：已在运行时直接返回 true（重复 init 是空操作）。启动时先订阅
     * config.applied / hotkey.suspend / hotkey.resume 再起线程；线程创建窗口后
     * 按当前配置注册热键，随后握手返回。
     *
     * @param[in] host    宿主服务表；可为 nullptr（老宿主没有服务表，热键仍可注册，
     *                    只是发不了事件）。指针必须活到 stop() 返回。
     * @param[in] config  热键配置；可为 nullptr（按空配置处理）。指针必须活到
     *                    stop() 返回。
     * @param[in] catalog 热键目录（hotkey_list 快照）；可为 nullptr（不注册任何热键）。
     *                    指针必须活到 stop() 返回。
     * @return true = 线程已就绪；false = 建线程/建窗口失败。
     * @note 建线程可能抛异常（线程资源耗尽），这里就地兜住，异常绝不越过 DLL 边界。
     */
    bool start(const ShotHostServices* host, HotkeyConfig* config, HotkeyCatalog* catalog);

    /**
     * @brief 退订事件总线、撤掉全部热键、让热键线程退出消息循环并 join。
     *
     * 可重复调用；返回时窗口、热键与线程一定都已经收干净。
     */
    void stop();

    /**
     * @brief 任意线程可调：请求热键线程按当前目录快照重新解析并重注册。
     *
     * 三条路径都走这里：启动时由线程自己注册一次、设置页应用后宿主发布
     * config.applied、hotkey_list 变化后 HotkeyCatalog 的 change handler。
     * 重复调用是幂等的：热键线程整批撤掉再注册，不会叠加。
     */
    void requestReapply();

    /**
     * @brief 任意线程可调：向热键线程投递宿主事件。
     *
     * 投 `host.shutdown` 会直接转成 stop()（宿主卸载 UI 插件前先发它，
     * 见 document/SETTINGS.md §3.1）；其它主题当前没有处理者，只记 DEBUG 后
     * 忽略（热键插件只依赖事件总线与配置，不需要 post 通道做业务）。
     *
     * @param[in] name 事件名；按契约是编译期静态字符串，不能为空。
     * @param[in] data 不透明事件数据，可为 nullptr。
     * @param[in] len  data 的字节数。
     * @return 0 = 已处理；-1 = 参数非法；1 = 热键线程还没就绪或正在关闭。
     * @note 事件名与 len 字节会在返回前拷走，投递方可立即释放自己的缓冲区。
     */
    std::int32_t post(const char* name, const char* data, std::uint32_t len);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl; /**< PIMPL：Win32 句柄、线程、注册状态全藏在 Impl 里。 */
};

}  // namespace hotkey
