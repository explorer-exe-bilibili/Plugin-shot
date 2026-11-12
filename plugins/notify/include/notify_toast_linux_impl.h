#pragma once

// ============================================================================
//  include/notify_toast_linux_impl.h —— 内部头：Linux（libnotify）版 NotifyToast::Impl
//
//  **不是插件对外接口**：只有 src/notify_toast_linux.cpp 包含它。公共头
//  notify_toast.h 不包含 <libnotify/notify.h>；本插件里也没有 Windows 的隐藏
//  窗口/占位图标，Linux 的通知完全交给桌面通知守护进程（D-Bus）。
//
//  为什么没有通知线程：libnotify 的 show 是一次短 D-Bus 调用，不依赖窗口类或
//  线程亲和性；在宿主事件分发线程上同步提交即可。少一个线程就少一类"destroy
//  之后还有回调引用已卸载代码"的生命周期问题。
// ============================================================================

#include <cstdint>
#include <string>

#include "notify_toast.h"
#include "plugin_api.h"

namespace shot {
class I18n;  // 语言表：定义在 shot_plugin_helpers/shot_i18n.h，只在本类的 .cpp 里用完整类型
}  // namespace shot

namespace notify {

class NotifyConfig;

/** @brief 订阅：截屏落盘成功（JSON {"path","width","height","processors"}）。 */
inline constexpr char kTopicCaptureDone[] = "capture.done";
/** @brief 订阅：截屏某个阶段失败（JSON {"stage","reason"}）。 */
inline constexpr char kTopicCaptureFailed[] = "capture.failed";

/**
 * @brief Linux 版 NotifyToast::Impl：只持有宿主/配置/语言表与订阅状态。
 *
 * 所有字段都只在宿主 main 线程访问（create/capture.* 分发/destroy 串行发生），
 * 因此不需要互斥量；配置值本身仍是 atomic（set_config 可能来自设置页线程）。
 */
struct NotifyToast::Impl {
    const ShotHostServices* host = nullptr;  /**< 宿主服务表；生命周期覆盖插件实例。 */
    NotifyConfig* config = nullptr;          /**< 开关配置；与本对象同生共死。 */
    const shot::I18n* i18n = nullptr;        /**< 语言表；可为 nullptr（回退中文）。 */
    bool initialized = false;                /**< notify_init 是否成功。 */
    bool acceptingEvents = false;            /**< stop 开始后拒绝已排队的事件。 */

    /** @brief 事件总线回调：只做深拷贝与分发，异常绝不越过 DLL 边界。 */
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);

    /** @brief 按主题分发一条事件（在宿主 main 线程上执行）。 */
    void handleEvent(const char* topic, const char* data, std::uint32_t len);

    /** @brief capture.done：按配置显示「截屏成功 / 已保存到 <path>」。 */
    void onCaptureDone(const std::string& payload);

    /** @brief capture.failed：按配置显示「截屏失败 / 截图失败：<reason>」。 */
    void onCaptureFailed(const std::string& payload);

    /**
     * @brief 用 libnotify 提交一条桌面通知并立即释放通知对象。
     * @param[in] title   标题（UTF-8）。
     * @param[in] body    正文（UTF-8）。
     * @param[in] failure true = 失败通知（CRITICAL + warning 图标），false = 成功通知。
     * @param[in] sound   true = 附 sound-name hint；false = 附 suppress-sound hint。
     *                    两者都只是给桌面通知服务的建议，不保证真的有声/静音。
     */
    void showNotification(const std::string& title, const std::string& body, bool failure,
                          bool sound);

    /** @brief 按当前语言取文案；i18n 不可用时返回中文 fallback。 */
    const char* localized(const char* key, const char* fallback) const;
};

}  // namespace notify
