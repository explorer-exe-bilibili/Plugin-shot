#pragma once

// ============================================================================
//  include/long_capture.h —— 长截图插件主体（平台无关的编排层）
//
//  职责划分（STYLE.md §6.6 / §6.8）：
//    · 本类：生命周期、配置项、事件订阅、触发键、会话线程与命令标志；
//    · long_capture_session.cpp：一次会话（找窗口 → HUD/气泡 → 连续抓帧 → 拼接
//      → 保存或取消）；
//    · stitch_canvas / scroll_stitcher / frame_loader / long_image_saver：纯算法；
//    · hud / global_keys / tray_hint / window_target 的 *_windows.cpp：平台实现。
//
//  线程模型：
//    · 宿主 main 线程：create/destroy、set_config、事件回调（只置标志 + 唤醒）；
//    · 会话线程（本类持有）：等"开始"命令，跑一次会话；保存/取消/退出都是标志；
//    · HUD 线程、按键线程：各自的消息循环，回调只置原子标志（见对应平台实现）。
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "capture_frame.h"
#include "long_capture_config.h"
#include "plugin_api.h"
#include "screen_rect.h"
#include "window_target.h"

namespace shot {
class I18n;  // 多语言：定义在 shot_plugin_helpers/shot_i18n.h，只在本类 .cpp 里用完整类型
}  // namespace shot

namespace long_capture {

class GlobalKeys;
class StitchCanvas;
class TrayHint;

/**
 * @brief 长截图插件状态：配置、订阅、触发键与唯一的会话线程。
 *
 * 一次只能有一个会话（再按触发键/托盘项在会话期间会被忽略）。会话线程常驻，
 * 空闲时阻塞在条件变量上；保存/取消/退出通过原子语义的标志 + notify 打断，
 * 不做任何忙等。
 *
 * @note 本类的公开方法会在宿主 main 线程（create/destroy/set_config/事件回调）
 *       与会话线程上被调用，内部共享状态用 m_mutex 保护。
 */
class LongCapture {
public:
    /** @brief 构造：配置取默认值，不起线程、不订阅。 */
    LongCapture();

    /** @brief 析构：兜底调用 stop()，保证线程、按键、订阅先于插件状态释放。 */
    ~LongCapture();

    /** @brief 不可拷贝：持有线程、订阅与窗口资源。 */
    LongCapture(const LongCapture&) = delete;
    /** @brief 不可赋值。 */
    LongCapture& operator=(const LongCapture&) = delete;

    /**
     * @brief 挂上宿主服务：加载语言表、订阅事件、发布托盘命令、注册触发键、起会话线程。
     *
     * 本插件没有 ui_provider 的 init/deinit，create(index, host) 是唯一能拿到宿主
     * 服务表的时机，所以由 plugin_entry.cpp 在状态建好后调用一次。
     *
     * @param[in] host 宿主服务表；老宿主可能为 nullptr（此时只剩配置能力，订阅与
     *                 热键都安静降级，不报错）。
     * @return true = 已就绪；false = 会话线程创建失败（实例仍可当纯配置插件用）。
     * @note 异常绝不越过 ABI 边界：内部全部收敛成 false。
     */
    bool start(const ShotHostServices* host);

    /** @brief 停止会话、注销热键、销毁窗口/窗口类、退订事件总线并 join 全部线程；可重复调用。 */
    void stop();

    /**
     * @brief 配置项个数，转调 LongCaptureConfig。
     * @return 配置项个数。
     */
    std::uint32_t configCount() const noexcept;

    /**
     * @brief 声明第 index 个配置项，转调 LongCaptureConfig。
     * @param[in]  index 配置项下标。
     * @param[out] out   宿主提供并填好 struct_size 的输出结构。
     * @return 契约返回码：0 成功 / 1 参数非法 / 2 out 结构太短。
     */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const noexcept;

    /**
     * @brief 读一个配置项，转调 LongCaptureConfig。
     * @return >= 0 = 值长度（不含 NUL）；负值 = key 不认识或参数非法。
     */
    std::int32_t getConfig(const char* key, char* buf, std::uint32_t bufSize) const;

    /**
     * @brief 写一个配置项；hotkey 变化时立即按新值重注册触发键。
     * @return 0 = 已生效；负值 = key 不认识或值非法。
     */
    std::int32_t setConfig(const char* key, const char* value);

private:
    /** @brief 一次 capture.done / capture.failed 的结果（会话线程取走）。 */
    struct CaptureResult {
        bool ready = false;   /**< 已收到本次请求的结果。 */
        bool failed = false;  /**< true = capture.failed；false = capture.done。 */
        std::string path;     /**< capture.done 的 path；失败时为空。 */
        std::string reason;   /**< capture.failed 的 reason（宿主已本地化）。 */
    };

    /** @brief 事件总线回调（宿主 main 线程执行；只解析/拷贝 + notify）。 */
    static void onBusEvent(void* user, const char* topic, const void* data, std::uint32_t len);

    /** @brief 全局触发键回调（按键线程执行；只置标志 + notify）。 */
    static void onTriggerKey(void* user);
    /** @brief 会话内 Enter 回调（按键线程执行；只置标志 + notify）。 */
    static void onSaveKey(void* user);
    /** @brief 会话内 Esc 回调（按键线程执行；只置标志 + notify）。 */
    static void onCancelKey(void* user);
    /** @brief HUD「保存」按钮回调（HUD 线程执行；只置标志 + notify）。 */
    static void onHudSave(void* user);
    /** @brief HUD「取消」按钮回调（HUD 线程执行；只置标志 + notify）。 */
    static void onHudCancel(void* user);

    /** @brief 会话线程主体：等开始命令 → runSession() → 回到等待。 */
    void run();

    /** @brief 处理一条事件（宿主 main 线程）。 */
    void handleBusEvent(const char* topic, const char* data, std::uint32_t len);

    /** @brief 请求开始一次会话（会话进行中则忽略）。 */
    void requestStart();
    /** @brief 请求保存当前会话画布。 */
    void requestSave();
    /** @brief 请求取消当前会话（丢弃画布，不落盘）。 */
    void requestCancel();

    /** @brief 唤醒会话线程（所有命令都走这一条；不持业务锁）。 */
    void wake();

    /**
     * @brief 等本次抓帧的结果，或等保存/取消/退出命令。
     * @param[out] result  成功等到 capture.* 时填入。
     * @param[in]  timeoutMs 超时毫秒数；超时按失败处理。
     * @return true = 收到了 capture.* 结果；false = 超时或被命令打断。
     */
    bool waitCapture(CaptureResult& result, int timeoutMs);

    /** @brief 把抓帧结果清空（发请求前调用，避免上一帧的结果被当成这一帧）。 */
    void clearCaptureResult();

    /**
     * @brief 等一段时间，但保存/取消/退出会提前唤醒。
     * @param[in] timeoutMs 最长等待毫秒数。
     * @return true = 有命令到达（调用方应尽快退出循环/缩短等待）。
     */
    bool waitCommand(int timeoutMs);

    /**
     * @brief 跑一次完整会话（找窗口 → HUD/气泡 → 抓帧拼接 → 保存/取消）。
     *
     * 声明在本类里是因为它读写本类的命令标志与抓帧结果；实现拆在
     * long_capture_session.cpp，按"一次会话"这个关注点单独成文件。
     */
    void runSession();

    /**
     * @brief 取一次要截图的窗口（客户区物理像素）。
     * @param[out] target 目标窗口。
     * @param[out] reason 失败时的中文原因。
     * @return true = 成功。
     */
    bool resolveTarget(TargetWindow& target, std::string& reason) const;

    /**
     * @brief 发布一条带 region 的 capture.request。
     * @param[in] client 目标窗口客户区（系统屏幕坐标）。
     * @return true = 已入队；false = 宿主没有事件总线或队列满。
     */
    bool publishCaptureRequest(const ScreenRect& client) const;

    /**
     * @brief 保存画布为长图并发布 capture.done。
     * @param[in]  canvas 已定稿的画布。
     * @param[out] path   成功时的 UTF-8 绝对路径。
     * @param[out] error  失败时的中文原因。
     * @return true = 文件已落盘（capture.done 发布失败只记 WARN，不把保存判失败）。
     */
    bool saveCanvas(const StitchCanvas& canvas, std::string& path, std::string& error);

    /**
     * @brief 从 config_list 快照里读 image_saver.main/directory（读不到返回空串）。
     * @return UTF-8 目录；空串 = 交给 LongImageSaver 退回系统图片目录。
     */
    std::string configuredSaveDirectory() const;

    /**
     * @brief 查一条本插件的文案。
     * @param[in] key      语言表 key（long.*）。
     * @param[in] fallback 语言表不可用时的中文兜底。
     * @return UTF-8 文本；返回的是内部缓冲的拷贝，可安全跨线程持有。
     */
    std::string localized(const char* key, const char* fallback) const;

    /**
     * @brief 用托盘气泡提示一句话（HUD 不可用/会话开始提示/失败提示都走这里）。
     * @param[in] titleKey    标题语言 key。
     * @param[in] titleFallback 标题兜底。
     * @param[in] textKey     正文语言 key。
     * @param[in] textFallback 正文兜底。
     */
    void showTrayHint(const char* titleKey, const char* titleFallback, const char* textKey,
                      const char* textFallback) const;

    const ShotHostServices* m_host = nullptr; /**< 宿主服务表；生命周期覆盖插件实例。 */
    LongCaptureConfig m_config;               /**< 两个配置项（线程安全读写）。 */
    std::unique_ptr<shot::I18n> m_i18n;       /**< 多语言：start() 构造，stop() 释放。 */
    std::unique_ptr<GlobalKeys> m_keys;       /**< 全局按键：start() 起线程，stop() 收干净。 */
    mutable std::unique_ptr<TrayHint> m_trayHint; /**< HUD 不可用时的托盘气泡；可重复 show()。 */
    bool m_busSubscribed = false;             /**< 是否已订阅事件总线（stop 时按它退订）。 */

    std::thread m_worker;         /**< 会话线程；joinable 表示 start 成功过。 */
    mutable std::mutex m_mutex;   /**< 保护下面全部命令/结果状态。 */
    std::condition_variable m_cv; /**< 命令与抓帧结果共用的唤醒点。 */
    bool m_running = false;       /**< 会话线程是否继续等待新会话。 */
    bool m_startPending = false;  /**< 有一次"开始"还没被会话线程取走。 */
    bool m_sessionActive = false; /**< 当前是否正在会话中。 */
    bool m_saveRequested = false; /**< 会话内用户按了保存。 */
    bool m_cancelRequested = false; /**< 会话内用户按了取消/退出。 */
    bool m_captureReady = false;  /**< 本次抓帧的结果已到达。 */
    CaptureResult m_capture;      /**< 最近一次抓帧结果；只在 m_mutex 下读写。 */
};

}  // namespace long_capture
