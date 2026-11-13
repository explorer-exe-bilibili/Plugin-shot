#pragma once

// ============================================================================
//  include/notify_toast_linux_impl.h —— 内部头：Linux（libnotify）版 NotifyToast::Impl
//
//  **不是插件对外接口**：只有 src/notify_toast_linux.cpp 包含它。公共头
//  notify_toast.h 不包含 <libnotify/notify.h>；本插件里也没有 Windows 的隐藏
//  窗口/占位图标，Linux 的通知完全交给桌面通知守护进程（D-Bus）。
//
//  线程模型（两段）：
//    · 显示通知：在宿主事件分发线程上同步提交（show 是一次短 D-Bus 调用，不依赖
//      线程亲和性），少一次跨线程排队；
//    · 点击动作：libnotify 的 ActionInvoked 信号由 **GLib 主上下文**分发，而宿主的
//      主循环不是 GLib 的（事件总线驱动），所以本插件必须自己跑一个最小 GMainLoop
//      才能收到"用户点了通知"。这个循环放在本插件唯一的后台线程里，stop() 时先
//      quit + join，再释放 libnotify 与通知对象——DLL 卸载后不会留下回调。
// ============================================================================

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <glib.h>
#include <libnotify/notify.h>

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
 * @brief Linux 版 NotifyToast::Impl：宿主/配置/语言表、订阅状态、GLib 主循环与待回收通知。
 *
 * 宿主/配置/语言表都只在宿主 main 线程访问（create/capture.* 分发/destroy 串行发生），
 * 因此这些字段不需要互斥量；配置值本身仍是 atomic（set_config 可能来自设置页线程）。
 * 只有"GLib 主循环线程"与"待回收通知列表"是跨线程的，各自带锁。
 */
struct NotifyToast::Impl {
    const ShotHostServices* host = nullptr;  /**< 宿主服务表；生命周期覆盖插件实例。 */
    NotifyConfig* config = nullptr;          /**< 开关配置；与本对象同生共死。 */
    const shot::I18n* i18n = nullptr;        /**< 语言表；可为 nullptr（回退中文）。 */
    bool initialized = false;                /**< notify_init 是否成功。 */
    bool acceptingEvents = false;            /**< stop 开始后拒绝已排队的事件。 */

    // ---- 点击动作用的 GLib 主循环（本插件唯一的后台线程） ----
    std::thread loopThread;   /**< 只跑 GLib 主循环，用来分发 ActionInvoked / closed 信号。 */
    std::mutex loopMutex;     /**< 保护 mainLoop 指针（stop 与循环线程的握手）。 */
    GMainLoop* mainLoop = nullptr; /**< 循环线程里的 GMainLoop；没有线程时为 nullptr。 */

    // ---- 仍可能被点击的通知（各持创建时的那一个引用，关闭时自释放） ----
    std::mutex pendingMutex;                    /**< 保护 pending。 */
    std::vector<NotifyNotification*> pending;   /**< 已提交且带动作的通知；stop() 兜底释放。 */

    /** @brief 事件总线回调：只做深拷贝与分发，异常绝不越过 DLL 边界。 */
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);

    /** @brief 按主题分发一条事件（在宿主 main 线程上执行）。 */
    void handleEvent(const char* topic, const char* data, std::uint32_t len);

    /** @brief capture.done：按配置显示「截屏成功 / 已保存到 <path>」，并挂上点击动作。 */
    void onCaptureDone(const std::string& payload);

    /** @brief capture.failed：按配置显示「截屏失败 / 截图失败：<reason>」（不带动作）。 */
    void onCaptureFailed(const std::string& payload);

    /**
     * @brief 用 libnotify 提交一条桌面通知。
     * @param[in] title     标题（UTF-8，已由配置模板或默认标题解析好）。
     * @param[in] body      正文（UTF-8）。
     * @param[in] failure   true = 失败通知（CRITICAL + warning 图标），false = 成功通知。
     * @param[in] sound     true = 附 sound-name hint；false = 附 suppress-sound hint。
     *                      两者都只是给桌面通知服务的建议，不保证真的有声/静音。
     * @param[in] imagePath 成功通知的图片路径（UTF-8）；空串 = 不挂点击动作（失败通知）。
     * @param[in] openFolder true = 点击后打开所在文件夹；false = 打开图片。
     */
    void showNotification(const std::string& title, const std::string& body, bool failure,
                          bool sound, const std::string& imagePath, bool openFolder);

    /** @brief 按当前语言取文案；i18n 不可用时返回中文 fallback。 */
    const char* localized(const char* key, const char* fallback) const;

    // ---- GLib 主循环与通知回收（只有 Linux 后端有这一层） ----
    void startActionLoop();
    void stopActionLoop();
    void runActionLoop();
    void trackPending(NotifyNotification* notification);
    bool removePending(NotifyNotification* notification);
    void releasePending();

    /**
     * @brief libnotify 动作回调：在 GLib 主循环线程上打开图片或其所在目录。
     * @param[in] notification 触发动作的通知（本插件不用）。
     * @param[in] action       动作 id（本插件只用 "default"，不用）。
     * @param[in] userData     showNotification 里申请的点击目标（ClickTarget*）。
     */
    static void onActionInvoked(NotifyNotification* notification, char* action, gpointer userData);

    /**
     * @brief 通知被关闭（超时/用户关掉/动作已触发）：释放本插件持有的那一个引用。
     * @param[in] notification 已被关闭的通知。
     * @param[in] userData     Impl*（GLib 主循环线程上执行）。
     */
    static void onNotificationClosed(NotifyNotification* notification, gpointer userData);
};

}  // namespace notify
