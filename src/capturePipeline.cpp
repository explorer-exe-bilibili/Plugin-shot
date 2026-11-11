#include "capturePipeline.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "PluginInstances/ImageProcessor.h"
#include "PluginInstances/ImageSaver.h"
#include "PluginInstances/ScreenCapture.h"
#include "hostServices.h"
#include "pluginManager.h"

// ============================================================================
//  capturePipeline.cpp —— 采集流水线的工作线程实现
//
//  数据流（详细线程/锁说明见 capturePipeline.h 的文件头）：
//    capture.request  ──main 分发回调──►  有界队列  ──worker──►  capture → crop
//                                                                 → process → save
//                                                                 → 发布结果
//
//  三条纪律：
//    1. main 线程的回调只做"拷贝负载 + 入队"，绝不跑插件；
//    2. worker 执行插件能力时整段持宿主生命周期锁（插件不会在流水线中途被卸载），
//       但不持任何宿主数据锁；
//    3. worker 发布的 capture.done / capture.failed 走事件总线（只入队、非阻塞），
//       订阅回调仍然在 main 线程执行 —— 所以 worker 永远不需要等 main。
// ============================================================================

namespace {

// 主题约定见 document/CAPTURE.md §3 与 document/EVENT_BUS.md §4。
const char kCaptureRequestTopic[] = "capture.request";
const char kCaptureDoneTopic[] = "capture.done";
const char kCaptureFailedTopic[] = "capture.failed";

// 事件负载约束：单条 64 KiB。capture.done 只有路径与几个数字，远小于上限。
constexpr uint32_t kSavePathSize = 1024;  /**< 保存插件写最终路径的缓冲；契约建议 1024 */

// 单张截图的上限：防止 describe 报出离谱尺寸时把宿主动内存吃光。
// 4K 屏约 33 MB，8K 约 132 MB；1 GiB 足够覆盖当前桌面，也留了误报的余量。
constexpr std::int64_t kMaxCaptureBytes = 1024LL * 1024 * 1024;

// 请求队列上限：worker 串行执行，一张 4K 图要几百毫秒；队列再长只会让用户觉得
// "点了很多次怎么现在才截"。4 条足够吸收连点，超出的丢弃并 warn（验收要求有日志）。
constexpr std::size_t kMaxQueuedRequests = 4;

/**
 * @brief 发布一条主题事件（负载为空或 JSON 文本）。
 * @param[in] topic   主题，不能为空。
 * @param[in] payload JSON 文本；空串表示无负载。
 * @return true = 已入队；false = 参数非法 / 队列满（事件被丢弃）。
 */
bool publishTopic(const char* topic, const std::string& payload) {
    const void* data = payload.empty() ? nullptr : payload.data();
    const auto len = static_cast<uint32_t>(payload.size());
    return Host::HostServices::eventBusPublish(nullptr, topic, data, len) == 0;
}

/**
 * @brief 把跨边界数字收进 int32（越界按边界截断）。
 * @param[in] value JSON 里的整数（int64）。
 * @return int32 范围内的值。
 */
std::int32_t clampToInt32(std::int64_t value) {
    if (value > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (value < std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(value);
}

/**
 * @brief 从 JSON 对象里读一个整数坐标/尺寸字段。
 * @param[in]  object 已解析的 JSON 对象。
 * @param[in]  key    字段名。
 * @param[out] out    成功时写入（int32 截断）。
 * @return true = 字段存在且是整数字面量。
 */
bool readJsonInt(const nlohmann::json& object, const char* key, std::int32_t& out) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) {
        return false;
    }
    out = clampToInt32(it->get<std::int64_t>());
    return true;
}

}  // namespace

namespace Host {

CapturePipeline::CapturePipeline(Plugin::PluginManager& manager, std::mutex& lifecycleMutex)
    : m_manager(manager), m_lifecycleMutex(lifecycleMutex) {}

CapturePipeline::~CapturePipeline() {
    stop();
}

bool CapturePipeline::start() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_workerRunning) {
            m_stopping = false;
            m_worker = std::thread(&CapturePipeline::workerLoop, this);
            m_workerRunning = true;
            spdlog::info("CapturePipeline: 工作线程已启动（请求队列上限 {}）", kMaxQueuedRequests);
        }
    }
    if (!m_subscribed) {
        m_subscribed = HostServices::eventBusSubscribe(nullptr, kCaptureRequestTopic,
                                                       &CapturePipeline::onCaptureRequest, this) == 0;
        if (!m_subscribed) {
            spdlog::warn("CapturePipeline: 订阅 {} 失败，只能由 captureOnce() 直接驱动",
                         kCaptureRequestTopic);
        }
    }
    return m_subscribed;
}

void CapturePipeline::stop() {
    if (m_subscribed) {
        HostServices::eventBusUnsubscribe(nullptr, this);
        m_subscribed = false;
    }
    std::deque<PendingRequest> abandoned;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_workerRunning) {
            return;
        }
        m_stopping = true;
        abandoned.swap(m_queue);  // 宿主退出：没开始的请求不再执行
    }
    m_requestCv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();  // 正在执行的那张图会先走完（worker 释放生命周期锁后退出）
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_workerRunning = false;
        m_stopping = false;
    }
    // 理论上退出路径没有同步等待方；防御性地把它们唤醒为失败，别让 captureOnce 卡死。
    for (PendingRequest& request : abandoned) {
        if (request.completion == nullptr) {
            continue;
        }
        std::lock_guard<std::mutex> lock(request.completion->mutex);
        request.completion->result.ok = false;
        request.completion->result.failStage = "capture";
        request.completion->result.failMessage = I18n::Message("capture.error.host_shutting_down");
        request.completion->done = true;
        request.completion->cv.notify_all();
    }
    spdlog::info("CapturePipeline: 工作线程已停止（丢弃 {} 条未开始的请求）", abandoned.size());
}

bool CapturePipeline::requestCapture(const std::string& payload) {
    return enqueue(payload, nullptr);
}

bool CapturePipeline::captureOnce(std::string* savedPath) {
    if (savedPath != nullptr) {
        savedPath->clear();
    }
    auto completion = std::make_shared<Completion>();
    if (!enqueue(std::string(), completion)) {
        return false;
    }
    // 等的是"worker 写入完成标志"，不是生命周期锁：本函数不持任何锁，
    // 不会出现"main 持锁定等 worker、worker 等锁"的自锁。
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion] { return completion->done; });
    const PipelineResult result = completion->result;
    lock.unlock();
    if (savedPath != nullptr) {
        *savedPath = result.savedPath;
    }
    return result.ok;
}

void CapturePipeline::onCaptureRequest(void* user, const char* /*topic*/, const void* data,
                                       uint32_t len) {
    auto* pipeline = static_cast<CapturePipeline*>(user);
    if (pipeline == nullptr) {
        return;
    }
    // 回调运行在宿主分发线程（main）：这里只做"拷贝负载 + 入队"，抓屏/编码在
    // worker 上跑，事件循环不会因为一次采集而停摆。
    std::string payload;
    if (data != nullptr && len != 0) {
        payload.assign(static_cast<const char*>(data), len);
    }
    pipeline->requestCapture(payload);
}

bool CapturePipeline::enqueue(std::string payload, std::shared_ptr<Completion> completion) {
    std::uint64_t sequence = 0;
    std::size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_workerRunning) {
            spdlog::warn("capture.request 被丢弃：工作线程未启动（宿主还在初始化或已退出）");
            return false;
        }
        if (m_queue.size() >= kMaxQueuedRequests) {
            // 验收要求：队列满必须留下明确日志，用户/开发者能看出"这次点击被丢了"。
            spdlog::warn("capture.request 队列已满（上限 {}），丢弃本次请求", kMaxQueuedRequests);
            return false;
        }
        sequence = ++m_nextSequence;
        m_queue.push_back(PendingRequest{sequence, std::move(payload), std::move(completion)});
        queued = m_queue.size();
    }
    m_requestCv.notify_one();
    spdlog::info("capture.request 入队：序号 #{}（排队 {}）", sequence, queued);
    return true;
}

void CapturePipeline::workerLoop() {
    for (;;) {
        PendingRequest request;
        std::size_t remaining = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_requestCv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) {
                return;  // m_stopping 且队列已清空：退出（stop 负责 join）
            }
            request = std::move(m_queue.front());
            m_queue.pop_front();
            remaining = m_queue.size();
        }

        spdlog::info("capture worker: 开始 #{}（排队 {}）", request.sequence, remaining);
        const auto startedAt = std::chrono::steady_clock::now();
        PipelineResult result;
        {
            // 整段流水线持生命周期锁：抓屏/处理/保存期间对应插件不会被热卸载。
            // 反向没有风险：worker 不持这把锁等待 main（发布/唤醒都是非阻塞的）。
            std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
            result = runPipelineLocked(request.payload);
        }
        if (result.skipped) {
            // 区域框选占位请求：不采集、不落盘、不发任何 capture.* 事件。
            // debug 级别是刻意的：用户正常工作路径，不该刷 info 日志。
            spdlog::debug("capture worker: 完成 #{}：区域框选请求交给 region_selector，流水线不采集",
                          request.sequence);
        } else {
            publishResult(result);
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - startedAt)
                                       .count();
            spdlog::info("capture worker: 完成 #{}：{}，用时 {} ms", request.sequence,
                         result.ok ? "成功" : "失败", elapsedMs);
        }
        if (request.completion != nullptr) {
            std::lock_guard<std::mutex> lock(request.completion->mutex);
            request.completion->result = std::move(result);
            request.completion->done = true;
            request.completion->cv.notify_all();
        }
    }
}

CapturePipeline::PipelineResult CapturePipeline::runPipelineLocked(const std::string& payload) {
    PipelineResult result;

    // ---- 阶段 0：解析 region（在插件调用之前；坏负载只影响取景范围，不算失败） ----
    // 注意 Ignore：{"mode":"region"} 是"请开始框选"的占位请求，由 region_selector
    // 消费；流水线必须直接返回，绝不能把它当成整屏采集（否则每次区域截屏都会多存
    // 一张全屏图）。空负载 / 非 region 负载仍然按整屏采集。
    CaptureRegion region;
    const RegionRequest regionRequest = parseRegion(payload, region);
    if (regionRequest == RegionRequest::Ignore) {
        result.skipped = true;
        return result;
    }
    const bool wantRegion = regionRequest == RegionRequest::Region;

    // ---- 阶段 1：采集。取第一个采集插件，宿主按 describe 的尺寸分配整屏缓冲 ----
    const std::vector<std::shared_ptr<Plugin::ScreenCaptureInstance>> captures =
        m_manager.getPluginsByType<Plugin::ScreenCaptureInstance>(Plugin::PluginType::ScreenCapture);
    if (captures.empty()) {
        result.failStage = "capture";
        result.failMessage = I18n::Message("capture.error.no_capture_plugin");
        return result;
    }
    Plugin::ScreenCaptureInstance& capture = *captures.front();
    ShotCaptureInfo info{};
    const int32_t describeRc = capture.describe(info);
    if (describeRc != 0) {
        result.failStage = "capture";
        result.failMessage = I18n::Message("capture.error.describe_failed", std::to_string(describeRc));
        return result;
    }
    if (info.width <= 0 || info.height <= 0) {
        result.failStage = "capture";
        result.failMessage = I18n::Message("capture.error.invalid_size",
                                              std::to_string(info.width), std::to_string(info.height));
        return result;
    }
    if (info.pixel_format != SHOT_PIXEL_FORMAT_BGRA8) {
        result.failStage = "capture";
        result.failMessage = I18n::Message("capture.error.unsupported_pixel_format",
                                         std::to_string(info.pixel_format));
        return result;
    }
    // BGRA8 = 4 字节/像素；宿主把 stride 填成 width*4，保持"一行有效像素紧邻"。
    // 用 int64 算尺寸：width/height 来自插件，先防溢出让 vector 分配出个负数大小。
    const std::int64_t rowBytes = static_cast<std::int64_t>(info.width) * 4;
    const std::int64_t byteCount = rowBytes * static_cast<std::int64_t>(info.height);
    if (byteCount <= 0 || byteCount > kMaxCaptureBytes) {
        result.failStage = "capture";
        result.failMessage = I18n::Message("capture.error.too_large", std::to_string(byteCount));
        return result;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(byteCount));
    ShotImage fullImage = {
        .struct_size  = sizeof(ShotImage),
        .pixel_format = SHOT_PIXEL_FORMAT_BGRA8,
        .data         = pixels.data(),
        .width        = info.width,
        .height       = info.height,
        .stride       = static_cast<int32_t>(rowBytes),
        .channels     = 4,
    };
    const int32_t captureRc = capture.capture(fullImage);
    if (captureRc != 0) {
        result.failStage = "capture";
        result.failMessage = I18n::Message("capture.error.capture_failed", std::to_string(captureRc));
        return result;
    }

    // ---- 阶段 1.5：区域裁剪。裁剪失败（越界到没有像素/参数非法）退回整屏 ----
    ShotImage image = fullImage;                 // 后续处理/保存都针对这张图
    std::vector<std::uint8_t> croppedPixels;     // 裁剪结果缓冲，必须活到保存结束
    if (wantRegion) {
        ShotImage cropped{};
        if (cropImage(fullImage, region, croppedPixels, cropped)) {
            image = cropped;
        } else {
            spdlog::warn("CapturePipeline: region 裁剪后没有有效像素，按全屏继续");
        }
    }

    // ---- 阶段 2：处理。单个处理器失败只警告：整条流水线不因一个插件而报废 ----
    std::uint32_t processed = 0;
    const std::vector<std::shared_ptr<Plugin::ImageProcessorInstance>> processors =
        m_manager.getPluginsByType<Plugin::ImageProcessorInstance>(
            Plugin::PluginType::ImageProcessor);
    for (const auto& processor : processors) {
        if (processor->process(image)) {
            ++processed;
        } else {
            spdlog::warn("CapturePipeline: 处理器 {} 失败，继续用当前图像保存", processor->getId());
        }
    }

    // ---- 阶段 3：保存。directory/basename 传 NULL = 用保存插件自己的配置 ----
    const std::vector<std::shared_ptr<Plugin::ImageSaverInstance>> savers =
        m_manager.getPluginsByType<Plugin::ImageSaverInstance>(Plugin::PluginType::ImageSaver);
    if (savers.empty()) {
        result.failStage = "save";
        result.failMessage = I18n::Message("capture.error.no_saver_plugin");
        return result;
    }
    char path[kSavePathSize] = {0};
    const int32_t saveRc = savers.front()->save(image, nullptr, nullptr, path, sizeof(path));
    if (saveRc != 0) {
        result.failStage = "save";
        result.failMessage = I18n::Message("capture.error.save_failed", std::to_string(saveRc));
        return result;
    }
    const std::string savedFile(path);
    if (savedFile.empty()) {
        // 契约说成功时 path 里是最终路径；空路径信息量不够，但文件可能已经写成了，
        // 所以按"保存成功、路径未知"继续，只警告 —— 不把整次采集判失败。
        spdlog::warn("CapturePipeline: 保存插件返回成功但没有写回路径");
    }

    result.ok = true;
    result.savedPath = savedFile;
    result.width = image.width;
    result.height = image.height;
    result.processors = processed;
    return result;
}

CapturePipeline::RegionRequest CapturePipeline::parseRegion(const std::string& payload,
                                                           CaptureRegion& region) {
    if (payload.empty()) {
        return RegionRequest::FullScreen;  // 空负载 = 整屏采集（托盘「截屏」/ --capture）
    }
    nlohmann::json data = nlohmann::json::parse(payload, nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
        spdlog::warn("capture.request 负载不是合法 JSON 对象，按全屏采集（payload={}）", payload);
        return RegionRequest::FullScreen;
    }
    const auto modeIt = data.find("mode");
    const std::string mode =
        (modeIt != data.end() && modeIt->is_string()) ? modeIt->get<std::string>() : std::string();
    if (mode != "region") {
        if (!mode.empty()) {
            spdlog::info("capture.request 的 mode={} 不认识，按全屏采集", mode);
        }
        return RegionRequest::FullScreen;
    }

    // mode=region 是"区域截屏"的入口，走两段式约定（见 document/CAPTURE.md）：
    //   · 带合法 region 对象 → 归流水线：抓整屏后裁剪；
    //   · 没有合法 region   → 是"请开始框选"的占位请求，由 region_selector 消费。
    // 流水线必须把后者完全忽略：以前回退成整屏采集，用户每次区域截屏都会额外多存
    // 一张全屏图（用户可感知的 bug）。
    const auto regionIt = data.find("region");
    if (regionIt == data.end() || !regionIt->is_object()) {
        spdlog::debug("capture.request 是区域框选请求（无 region 对象），交给 region_selector，"
                      "流水线不采集");
        return RegionRequest::Ignore;
    }
    CaptureRegion parsed;
    const bool fieldsOk = readJsonInt(*regionIt, "x", parsed.x) &&
                          readJsonInt(*regionIt, "y", parsed.y) &&
                          readJsonInt(*regionIt, "width", parsed.width) &&
                          readJsonInt(*regionIt, "height", parsed.height);
    if (!fieldsOk) {
        spdlog::warn("capture.request 的 region 字段非法（要求整数 x/y/width/height），"
                     "本次区域请求交给 region_selector，流水线不采集（payload={}）",
                     payload);
        return RegionRequest::Ignore;
    }
    if (parsed.width <= 0 || parsed.height <= 0) {
        spdlog::warn("capture.request 的 region 宽高必须 >= 1（实际 {}x{}），"
                     "本次区域请求交给 region_selector，流水线不采集",
                     parsed.width, parsed.height);
        return RegionRequest::Ignore;
    }
    region = parsed;
    spdlog::info("capture.request 指定区域：{}x{}+{},{}（物理虚拟桌面像素，越界将夹取）",
                 region.width, region.height, region.x, region.y);
    return RegionRequest::Region;
}

bool CapturePipeline::cropImage(const ShotImage& source, const CaptureRegion& region,
                                std::vector<std::uint8_t>& outPixels, ShotImage& outImage) {
    if (source.data == nullptr || source.width <= 0 || source.height <= 0 ||
        source.channels <= 0) {
        return false;
    }
    // 越界夹取：左上角夹进图像内，宽高不超过剩余范围（"宽高至少 1"由下面的 <=0 检查兜底）。
    const std::int32_t x0 = std::clamp(region.x, 0, source.width - 1);
    const std::int32_t y0 = std::clamp(region.y, 0, source.height - 1);
    const std::int32_t cropWidth = std::min(region.width, source.width - x0);
    const std::int32_t cropHeight = std::min(region.height, source.height - y0);
    if (cropWidth < 1 || cropHeight < 1) {
        return false;
    }
    const std::int64_t rowBytes = static_cast<std::int64_t>(cropWidth) * source.channels;
    const std::int64_t byteCount = rowBytes * static_cast<std::int64_t>(cropHeight);
    if (byteCount <= 0 || byteCount > kMaxCaptureBytes) {
        return false;
    }
    const std::size_t pixelBytes =
        static_cast<std::size_t>(cropWidth) * static_cast<std::size_t>(source.channels);
    outPixels.assign(static_cast<std::size_t>(byteCount), 0);
    for (std::int32_t row = 0; row < cropHeight; ++row) {
        // 行地址 = data + row*stride：stride 为负时 data 指向最后一行，公式依然成立。
        const std::int64_t srcOffset =
            static_cast<std::int64_t>(y0 + row) * source.stride +
            static_cast<std::int64_t>(x0) * source.channels;
        std::memcpy(outPixels.data() + static_cast<std::size_t>(row) * rowBytes,
                    source.data + srcOffset, pixelBytes);
    }
    outImage = ShotImage{
        .struct_size  = sizeof(ShotImage),
        .pixel_format = source.pixel_format,
        .data         = outPixels.data(),
        .width        = cropWidth,
        .height       = cropHeight,
        .stride       = static_cast<int32_t>(rowBytes),
        .channels     = source.channels,
    };
    spdlog::info("CapturePipeline: 区域裁剪 {}x{}+{},{} → 实际 {}x{}（源 {}x{}）", region.width,
                 region.height, region.x, region.y, cropWidth, cropHeight, source.width,
                 source.height);
    return true;
}

void CapturePipeline::publishResult(const PipelineResult& result) {
    if (!result.ok) {
        nlohmann::json payload;
        payload["stage"] = result.failStage;
        payload["reason"] = result.failMessage.localized();
        spdlog::warn("CapturePipeline: {} 阶段失败：{}", result.failStage,
                     result.failMessage.chinese());
        publishTopic(kCaptureFailedTopic, payload.dump());
        return;
    }
    nlohmann::json payload;
    payload["path"] = result.savedPath;
    payload["width"] = result.width;
    payload["height"] = result.height;
    payload["processors"] = result.processors;
    publishTopic(kCaptureDoneTopic, payload.dump());
    spdlog::info("CapturePipeline: 已保存 {}（{}x{}，{} 个处理器生效）", result.savedPath,
                 result.width, result.height, result.processors);
}

}  // namespace Host
