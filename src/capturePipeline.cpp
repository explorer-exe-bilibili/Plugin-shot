#include "capturePipeline.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "PluginInstances/ImageProcessor.h"
#include "PluginInstances/ImageSaver.h"
#include "PluginInstances/ScreenCapture.h"
#include "hostServices.h"
#include "pluginManager.h"

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

/**
 * @brief 发布一条主题事件（负载为空或 JSON 文本）。
 *
 * 事件总线在宿主内部是静态服务；这里只是把"拼 payload → publish"的样板收起来。
 * @param[in] topic 主题，不能为空。
 * @param[in] payload JSON 文本；空串表示无负载。
 * @return true = 已入队；false = 参数非法 / 队列满（事件被丢弃）。
 */
bool publishTopic(const char* topic, const std::string& payload) {
    const void* data = payload.empty() ? nullptr : payload.data();
    const auto len = static_cast<uint32_t>(payload.size());
    return Host::HostServices::eventBusPublish(nullptr, topic, data, len) == 0;
}

}  // namespace

namespace Host {

CapturePipeline::CapturePipeline(Plugin::PluginManager& manager) : m_manager(manager) {}

CapturePipeline::~CapturePipeline() {
    stop();
}

bool CapturePipeline::start() {
    if (m_started) {
        return true;
    }
    m_started = HostServices::eventBusSubscribe(nullptr, kCaptureRequestTopic,
                                                &CapturePipeline::onCaptureRequest, this) == 0;
    if (!m_started) {
        spdlog::warn("CapturePipeline: 订阅 {} 失败，只能由 captureOnce() 直接驱动",
                     kCaptureRequestTopic);
    }
    return m_started;
}

void CapturePipeline::stop() {
    if (!m_started) {
        return;
    }
    HostServices::eventBusUnsubscribe(nullptr, this);
    m_started = false;
}

void CapturePipeline::onCaptureRequest(void* user, const char* /*topic*/, const void* /*data*/,
                                       uint32_t /*len*/) {
    auto* pipeline = static_cast<CapturePipeline*>(user);
    if (pipeline == nullptr) {
        return;
    }
    // 回调运行在宿主分发线程（main）：这里同步跑完整条流水线是允许的。
    // v1 忽略负载（只支持整屏采集），见 document/CAPTURE.md 的"刻意不做的事"。
    pipeline->captureOnce();
}

bool CapturePipeline::captureOnce(std::string* savedPath) {
    // TODO: 抓屏 + 图像处理 + 编码都同步跑在宿主 main 线程，4K 屏幕或慢编码会卡住
    // 事件分发；等出现可感知卡顿再挪到工作线程，回调只负责把请求投递给工作线程。
    if (savedPath != nullptr) {
        savedPath->clear();
    }

    // ---- 阶段 1：采集。取第一个采集插件，宿主按 describe 的尺寸分配缓冲 ----
    const std::vector<std::shared_ptr<Plugin::ScreenCaptureInstance>> captures =
        m_manager.getPluginsByType<Plugin::ScreenCaptureInstance>(Plugin::PluginType::ScreenCapture);
    if (captures.empty()) {
        return fail("capture", "没有可用的屏幕采集插件");
    }
    Plugin::ScreenCaptureInstance& capture = *captures.front();
    ShotCaptureInfo info{};
    const int32_t describeRc = capture.describe(info);
    if (describeRc != 0) {
        return fail("capture", "describe 失败（返回 " + std::to_string(describeRc) + "）");
    }
    if (info.width <= 0 || info.height <= 0) {
        return fail("capture", "describe 返回的尺寸非法（" + std::to_string(info.width) + "x" +
                                   std::to_string(info.height) + "）");
    }
    if (info.pixel_format != SHOT_PIXEL_FORMAT_BGRA8) {
        return fail("capture", "不支持的像素格式（" + std::to_string(info.pixel_format) + "）");
    }
    // BGRA8 = 4 字节/像素；宿主把 stride 填成 width*4，保持"一行有效像素紧邻"。
    // 用 int64 算尺寸：width/height 来自插件，先防溢出让 vector 分配出个负数大小。
    const std::int64_t rowBytes = static_cast<std::int64_t>(info.width) * 4;
    const std::int64_t byteCount = rowBytes * static_cast<std::int64_t>(info.height);
    if (byteCount <= 0 || byteCount > kMaxCaptureBytes) {
        return fail("capture", "采集尺寸过大（" + std::to_string(byteCount) + " 字节）");
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(byteCount));
    ShotImage image = {
        .struct_size  = sizeof(ShotImage),
        .pixel_format = SHOT_PIXEL_FORMAT_BGRA8,
        .data         = pixels.data(),
        .width        = info.width,
        .height       = info.height,
        .stride       = static_cast<int32_t>(rowBytes),
        .channels     = 4,
    };
    const int32_t captureRc = capture.capture(image);
    if (captureRc != 0) {
        return fail("capture", "capture 失败（返回 " + std::to_string(captureRc) + "）");
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
        return fail("save", "没有可用的图像保存插件");
    }
    char path[kSavePathSize] = {0};
    const int32_t saveRc = savers.front()->save(image, nullptr, nullptr, path, sizeof(path));
    if (saveRc != 0) {
        return fail("save", "save 失败（返回 " + std::to_string(saveRc) + "）");
    }
    const std::string savedFile(path);
    if (savedFile.empty()) {
        // 契约说成功时 path 里是最终路径；空路径信息量不够，但文件可能已经写成了，
        // 所以按"保存成功、路径未知"继续，只警告 —— 不把整次采集判失败。
        spdlog::warn("CapturePipeline: 保存插件返回成功但没有写回路径");
    }
    publishDone(savedFile, image.width, image.height, processed);
    if (savedPath != nullptr) {
        *savedPath = savedFile;
    }
    spdlog::info("CapturePipeline: 已保存 {}（{}x{}，{} 个处理器生效）", savedFile, image.width,
                 image.height, processed);
    return true;
}

bool CapturePipeline::fail(const std::string& stage, const std::string& reason) {
    spdlog::warn("CapturePipeline: {} 阶段失败：{}", stage, reason);
    nlohmann::json payload;
    payload["stage"] = stage;
    payload["reason"] = reason;
    publishTopic(kCaptureFailedTopic, payload.dump());
    return false;
}

void CapturePipeline::publishDone(const std::string& path, int32_t width, int32_t height,
                                  uint32_t processors) {
    nlohmann::json payload;
    payload["path"] = path;
    payload["width"] = width;
    payload["height"] = height;
    payload["processors"] = processors;
    publishTopic(kCaptureDoneTopic, payload.dump());
}

}  // namespace Host
