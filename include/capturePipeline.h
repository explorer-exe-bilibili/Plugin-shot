#pragma once

#include <cstdint>
#include <string>

namespace Plugin {
class PluginManager;
}  // namespace Plugin

// ============================================================================
//  Host::CapturePipeline —— 采集流水线（宿主侧编排）
//
//  把三件事串起来：屏幕采集插件出像素 → 图像处理插件原地处理 → 保存插件落盘。
//  宿主自己不做任何采集/处理/保存算法，只负责按顺序调用、分配一次像素缓冲、
//  把结果变成事件发出去。事件主题与负载见 document/CAPTURE.md。
// ============================================================================

namespace Host {

/**
 * @brief 采集流水线：一次 captureOnce() 走完"采集 → 处理 → 保存"。
 *
 * 能力来源与优先级：
 *  - 屏幕采集：取当前加载顺序里的**第一个** shot.screen_capture 插件；
 *  - 图像处理：按加载顺序对**所有** shot.image_processor 插件逐个 process；
 *    单个处理器失败只记警告，不影响整条流水线；
 *  - 图像保存：取当前加载顺序里的**第一个** shot.image_saver 插件。
 *
 * 线程模型：start / stop / captureOnce 都只在宿主 main 线程调用。
 * capture.request 的回调由事件分发线程执行，而分发线程就是 main 线程，
 * 所以回调里可以直接跑同步流水线（v1 的已知限制：抓屏+编码会拖住本轮事件）。
 */
class CapturePipeline {
public:
    /**
     * @brief 绑定插件管理器；不订阅任何事件。
     * @param[in] manager 插件管理器，生命周期必须覆盖本对象。
     */
    explicit CapturePipeline(Plugin::PluginManager& manager);
    /** @brief 退订 capture.request（如果还没退订）。 */
    ~CapturePipeline();

    /** @brief 禁止拷贝：订阅句柄（this）必须唯一。 */
    CapturePipeline(const CapturePipeline&) = delete;
    /** @brief 禁止赋值：订阅句柄（this）必须唯一。 */
    CapturePipeline& operator=(const CapturePipeline&) = delete;

    /**
     * @brief 订阅 capture.request：收到后同步跑一次流水线。
     *
     * 任何线程都能 publish capture.request，但回调只在宿主分发线程（main）执行，
     * 所以流水线里调插件是安全的；不会在插件线程上并发采集。
     *
     * @return true = 已订阅（或本来就订阅着）；false = 事件总线不可用，流水线只能
     *         由 captureOnce() 直接驱动（--capture 走这条路）。
     */
    bool start();

    /**
     * @brief 退订 capture.request；重复调用是空操作。
     * @note 必须在事件总线停止分发之后、插件卸载之前调用。
     */
    void stop();

    /**
     * @brief 同步执行一次采集流水线。
     *
     * 阶段失败（没有采集/保存插件、describe/capture/save 返回非 0、尺寸非法）时
     * 发布 capture.failed 并返回 false；成功时发布 capture.done 并返回 true。
     * 处理阶段某个处理器失败只打警告，不改变返回值。
     *
     * @param[out] savedPath 成功时写入最终文件路径（可为 nullptr）；失败时清空。
     * @return true = 已落盘并发布 capture.done；false = 已发布 capture.failed。
     * @note 只允许在宿主 main 线程调用；返回时插件调用全部结束。
     */
    bool captureOnce(std::string* savedPath = nullptr);

private:
    /**
     * @brief 事件总线回调：capture.request → captureOnce()。
     * @param[in] user this 指针。
     * @note 回调在宿主分发线程（main）执行；忽略负载，v1 只支持整屏采集。
     */
    static void onCaptureRequest(void* user, const char* topic, const void* data, uint32_t len);

    /**
     * @brief 记日志并发布 capture.failed，方便调用方 return false。
     * @param[in] stage  失败阶段："capture" / "save"。
     * @param[in] reason 中文原因，写进事件负载。
     * @return 恒为 false。
     */
    bool fail(const std::string& stage, const std::string& reason);

    /**
     * @brief 发布 capture.done（JSON：path / width / height / processors）。
     * @param[in] path       最终文件路径。
     * @param[in] width      图像宽。
     * @param[in] height     图像高。
     * @param[in] processors 成功处理过这张图的处理器个数。
     */
    void publishDone(const std::string& path, int32_t width, int32_t height,
                     uint32_t processors);

    Plugin::PluginManager& m_manager;  /**< 实例来源；只取快照，不管理生命周期 */
    bool m_started = false;            /**< 是否已订阅 capture.request；只由 main 线程读写 */
};

}  // namespace Host
