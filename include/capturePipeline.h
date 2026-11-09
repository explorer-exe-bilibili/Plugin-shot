#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plugin_api.h"

namespace Plugin {
class PluginManager;
}  // namespace Plugin

// ============================================================================
//  Host::CapturePipeline —— 采集流水线（宿主侧编排，工作线程执行）
//
//  把三件事串起来：屏幕采集插件出像素 → 图像处理插件原地处理 → 保存插件落盘。
//  宿主自己不做任何采集/处理/保存算法，只负责按顺序调用、分配像素缓冲、
//  把结果变成事件发出去。事件主题与负载见 document/CAPTURE.md。
//
//  为什么要工作线程：4K/多显示器抓屏 + 图像处理 + PNG 编码是"几百毫秒级"的活，
//  以前它跑在事件分发线程（也就是宿主 main 线程）上，抓一张图就把设置页/托盘/
//  插件启停全部卡住。现在：
//
//    capture.request（任意线程 publish）
//          │  main 线程分发回调：只拷贝负载 + 入队（非阻塞，队列满则 warn 丢弃）
//          ▼
//    [有界请求队列] ──► worker 线程：串行执行 capture → crop → process → save
//          │                 │ 整段持宿主生命周期锁：这期间插件不会被热卸载
//          ▼                 ▼
//    capture.done / capture.failed（worker 发布；总线只入队，订阅回调仍在 main）
//
//  锁策略：整段流水线持宿主生命周期锁（HostRuntime::m_lifecycleMutex）。选它而不是
//  "逐阶段加锁 + 跨阶段校验"的理由：一次流水线的三个阶段用的是同一批插件实例，
//  拆锁就要在阶段之间重新取快照并处理"插件在间隙被卸载"的所有分支，收益只是让
//  禁用少等几十毫秒；v1 不值得。代价明确：禁用/退出要等当前这张图走完（几百 ms）。
//
//  死锁边界：worker 永不等待 main 线程（发布事件/唤醒同步等待者都是非阻塞的），
//  所以 main 线程等 worker（--capture 的同步等待、退出时的 join）不会环等。
// ============================================================================

namespace Host {

/** @brief capture.request 负载里解析出的区域参数（物理虚拟桌面像素）。 */
struct CaptureRegion {
    std::int32_t x = 0;       /**< 区域左上角 x，原点 = 虚拟桌面左上角；越界会被夹取 */
    std::int32_t y = 0;       /**< 区域左上角 y */
    std::int32_t width = 0;   /**< 区域宽（像素，必须 > 0） */
    std::int32_t height = 0;  /**< 区域高（像素，必须 > 0） */
};

/**
 * @brief 采集流水线：把 capture.request 排进队列，由内部工作线程串行执行。
 *
 * 能力来源与优先级（与 v1 相同）：
 *  - 屏幕采集：取当前加载顺序里的**第一个** shot.screen_capture 插件，抓完整虚拟桌面；
 *  - 区域截图：带合法 region 的请求抓整屏后由宿主按行裁剪（契约不改）；
 *    `{"mode":"region"}`（没有 region 对象）是"请开始框选"的占位请求，由
 *    region_selector 插件消费，**流水线直接忽略、不落盘**，否则会多存一张全屏图；
 *  - 图像处理：按加载顺序对**所有** shot.image_processor 插件逐个 process；
 *    单个处理器失败只记警告，不影响整条流水线；
 *  - 图像保存：取当前加载顺序里的**第一个** shot.image_saver 插件。
 *
 * 线程模型：
 *  - start / stop / captureOnce / requestCapture 只允许在宿主 main 线程调用；
 *  - start 起 worker 线程并订阅 capture.request，stop 退订并 join worker；
 *  - 一次只处理一张图（串行），连续请求排队；队列上限有限，满了 warn 丢弃。
 */
class CapturePipeline {
public:
    /**
     * @brief 绑定插件管理器与宿主生命周期锁；不启动线程、不订阅事件。
     * @param[in] manager         插件管理器，生命周期必须覆盖本对象。
     * @param[in] lifecycleMutex  宿主生命周期锁；worker 执行流水线期间持有它，
     *                            保证抓屏/处理/保存时对应插件不会被热卸载。
     */
    CapturePipeline(Plugin::PluginManager& manager, std::mutex& lifecycleMutex);
    /** @brief 停 worker（join）并退订 capture.request。 */
    ~CapturePipeline();

    /** @brief 禁止拷贝：订阅句柄（this）与 worker 线程必须唯一。 */
    CapturePipeline(const CapturePipeline&) = delete;
    /** @brief 禁止赋值：订阅句柄（this）与 worker 线程必须唯一。 */
    CapturePipeline& operator=(const CapturePipeline&) = delete;

    /**
     * @brief 启动工作线程并订阅 capture.request。
     *
     * 工作线程一定会启动（--capture 也依赖它）；订阅失败只影响"事件触发"这条入口，
     * 返回 false 时仍可以用 captureOnce() 直接驱动。
     *
     * @return true = 已订阅 capture.request（或本来就订阅着）；
     *         false = 事件总线不可用，只能由 captureOnce() 驱动。
     */
    bool start();

    /**
     * @brief 退订 capture.request、通知 worker 退出并 join；重复调用是空操作。
     *
     * 正在执行的那张图会先走完（worker 持生命周期锁，join 等它释放），未执行的排队
     * 请求直接丢弃；同步等待方若还在（正常退出路径不会有）会被唤醒为失败。
     * @note 必须在插件卸载之前调用（HostRuntime 析构第一步）。
     */
    void stop();

    /**
     * @brief 提交一次采集请求并等待完成（供 `--capture` 使用）。
     *
     * 请求排进与 capture.request 相同的队列，由 worker 串行执行；本函数在 main 线程
     * 阻塞等待 worker 完成。worker 执行期间持有生命周期锁，所以这里不会持锁等待，
     * 不存在"main 持锁等 worker、worker 等锁"的死锁。
     *
     * @param[out] savedPath 成功时写入最终文件路径（可为 nullptr）；失败时清空。
     * @return true = 已落盘并发布 capture.done；false = 已发布 capture.failed。
     */
    bool captureOnce(std::string* savedPath = nullptr);

    /**
     * @brief 提交一次异步采集请求（事件回调走这里），立即返回。
     * @param[in] payload capture.request 的负载（JSON 文本，可为空 = 全屏）。
     * @return true = 已入队；false = worker 未启动 / 队列已满（本次请求被丢弃）。
     */
    bool requestCapture(const std::string& payload = {});

private:
    /** @brief 一次流水线的结果（在 worker 线程上产生）。 */
    struct PipelineResult {
        bool ok = false;                  /**< true = 已落盘并应发 capture.done */
        bool skipped = false;             /**< true = 本次请求不是给流水线的（区域占位请求），
                                               不采集、不落盘、也不发 capture.done/failed */
        std::string savedPath;            /**< 最终文件路径；插件没写回时为空串 */
        std::string failStage;            /**< 失败阶段："capture" / "save" */
        std::string failReason;           /**< 失败原因（写进 capture.failed 负载） */
        std::int32_t width = 0;           /**< 最终图像宽（region 裁剪后） */
        std::int32_t height = 0;          /**< 最终图像高（region 裁剪后） */
        std::uint32_t processors = 0;     /**< 成功处理过这张图的处理器个数 */
    };

    /** @brief 同步提交方等待 worker 结果的会合点。 */
    struct Completion {
        std::mutex mutex;            /**< 保护 done/result */
        std::condition_variable cv;  /**< worker 完成时唤醒 */
        bool done = false;           /**< worker 是否已写入 result */
        PipelineResult result;       /**< 流水线结果 */
    };

    /** @brief 队列里的一次请求；completion 为空表示异步（事件总线触发）。 */
    struct PendingRequest {
        std::uint64_t sequence = 0;                    /**< 诊断日志用的自增序号 */
        std::string payload;                           /**< capture.request 负载 */
        std::shared_ptr<Completion> completion;        /**< 同步等待方；异步请求为空 */
    };

    /**
     * @brief 事件总线回调：capture.request → 入队（本函数只跑在 main 线程）。
     * @param[in] user this 指针。
     * @param[in] data 负载字节（可为 nullptr）。
     * @param[in] len  负载字节数。
     */
    static void onCaptureRequest(void* user, const char* topic, const void* data, uint32_t len);

    /**
     * @brief 把一次请求放进队列（main 线程调用；队列满/worker 未启动时丢弃并 warn）。
     * @param[in] payload    负载 JSON 文本。
     * @param[in] completion 同步等待方；异步传 nullptr。
     * @return true = 已入队。
     */
    bool enqueue(std::string payload, std::shared_ptr<Completion> completion);

    /** @brief worker 主循环：取请求 → 持锁跑流水线 → 发布结果 → 唤醒等待方。 */
    void workerLoop();

    /**
     * @brief 执行一次流水线；调用方必须已持有宿主生命周期锁（worker 负责）。
     *
     * 阶段失败返回 ok=false 的结果（不在这里发事件）；成功时带上最终尺寸与路径；
     * 不是给流水线的请求（见 RegionRequest::Ignore）返回 skipped=true，调用方不得
     * 发任何 capture.* 事件、也不落盘。
     * @param[in] payload capture.request 负载（可能为空 = 全屏）。
     */
    PipelineResult runPipelineLocked(const std::string& payload);

    /**
     * @brief 发布 capture.done / capture.failed（worker 线程调用）。
     * @param[in] result 流水线结果（skipped 的结果不应传进来）。
     */
    void publishResult(const PipelineResult& result);

    /**
     * @brief capture.request 负载的分类结果。
     *
     * 区域截屏是"两段式"的：`{"mode":"region"}`（没有 region 对象）是**请求开始框选**，
     * 由 region_selector 插件处理；流水线必须忽略它，否则会多存一张全屏图。
     */
    enum class RegionRequest {
        FullScreen, /**< 空负载 / 非 region 请求：按整屏采集 */
        Region,     /**< 带合法 region 对象：抓整屏后按区域裁剪 */
        Ignore,     /**< mode=region 但没有合法 region：交给 region_selector，流水线不采集 */
    };

    /**
     * @brief 从 capture.request 负载里解析 region（worker 线程调用）。
     *
     * 约定（document/CAPTURE.md）：空负载 / 非 region 负载 = 整屏采集；
     * `{"mode":"region","region":{x,y,width,height}}` 时坐标是物理虚拟桌面像素；
     * `{"mode":"region"}` 且没有合法 region 对象时是"请开始框选"的占位请求，
     * 返回 Ignore 让流水线直接丢弃（region_selector 订阅同一事件负责框选），
     * 绝不回退成整屏采集。
     *
     * @param[in]  payload 负载 JSON 文本。
     * @param[out] region  Region 时写入区域参数。
     * @return 请求分类；Ignore 时调用方不得采集/落盘。
     */
    static RegionRequest parseRegion(const std::string& payload, CaptureRegion& region);

    /**
     * @brief 按 region 从整屏图像里裁出一块新图像（按行拷贝，尊重 stride/channels）。
     *
     * 越界坐标夹取到图像范围内，宽高至少 1；源图像 stride 可以为负（自底向上）。
     * @param[in]  source    整屏图像。
     * @param[in]  region    请求区域（物理像素）。
     * @param[out] outPixels 裁剪结果的像素缓冲（调用方持有，需活到保存结束）。
     * @param[out] outImage  裁剪出的图像描述，data 指向 outPixels。
     * @return true = 裁剪成功；false = 参数非法/裁剪后尺寸小于 1（调用方按全屏继续）。
     */
    static bool cropImage(const ShotImage& source, const CaptureRegion& region,
                          std::vector<std::uint8_t>& outPixels, ShotImage& outImage);

    Plugin::PluginManager& m_manager;  /**< 实例来源；只取快照，不管理生命周期 */
    std::mutex& m_lifecycleMutex;      /**< 宿主生命周期锁；worker 执行期间持有 */
    std::mutex m_mutex;                /**< 保护 m_queue / m_nextSequence / 运行标志 */
    std::condition_variable m_requestCv; /**< worker 等待新请求 */
    std::deque<PendingRequest> m_queue;  /**< 待执行请求队列（有上限，main 入队） */
    std::thread m_worker;                /**< 工作线程 */
    std::uint64_t m_nextSequence = 0;    /**< 入队序号，诊断日志用 */
    bool m_workerRunning = false;        /**< worker 是否已启动且未停止 */
    bool m_stopping = false;             /**< stop() 已通知退出；worker 据此收尾 */
    bool m_subscribed = false;           /**< 是否已订阅 capture.request */
};

}  // namespace Host
