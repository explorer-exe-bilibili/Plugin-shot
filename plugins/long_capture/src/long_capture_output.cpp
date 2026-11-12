// 长截图的"产出侧"：最终保存、保存目录解析、文案与托盘气泡。
//
// 为什么单独一个 TU：这些逻辑与"一次会话怎么跑"无关，放在 long_capture.cpp 里会让
// 生命周期/事件编排的文件超过 500 行（STYLE.md §6.8）。本文件只在会话线程或宿主
// main 线程上被调用，内部不持有线程与窗口。
//
// 关键约束：capture.done **只在最终保存成功时发布一条**，负载与宿主流水线同格式
// {path,width,height,processors:0}；每帧采集不再经过 image_saver，也不再发事件。

#include "long_capture.h"

#include <string>

#include <nlohmann/json.hpp>

#include "long_image_saver.h"
#include "shot_plugin_helpers.h"
#include "stitch_canvas.h"
#include "tray_hint.h"

namespace long_capture {
namespace {

/** @brief 最终长图的完成事件：与宿主 CapturePipeline 的输出格式一致。 */
constexpr char kTopicCaptureDone[] = "capture.done";

/** @brief image_saver 的插件 id / 保存目录配置键。 */
constexpr char kImageSaverOwner[] = "image_saver.main";
constexpr char kImageSaverDirectoryKey[] = "directory";

}  // namespace

bool LongCapture::saveCanvas(const StitchCanvas& canvas, std::string& path, std::string& error) {
    error.clear();
    path.clear();
    if (canvas.empty()) {
        error = "画布是空的，没有可保存的内容";
        return false;
    }
    const SaveOutcome outcome = LongImageSaver::save(canvas, configuredSaveDirectory());
    if (!outcome.ok) {
        error = outcome.error;
        return false;
    }
    path = outcome.path;

    nlohmann::json payload;
    payload["path"] = path;
    payload["width"] = canvas.width();
    payload["height"] = canvas.height();
    payload["processors"] = 0;
    const std::string text = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (!shot::EventBus(m_host).publish(kTopicCaptureDone, text)) {
        // 文件已经落盘；事件发布失败只影响通知/剪贴板等消费者，不把保存判失败。
        shot::Logger(m_host).warn(
            "[long_capture] 保存成功，但发布 capture.done 失败（事件队列满？）");
    }
    return true;
}

std::string LongCapture::configuredSaveDirectory() const {
    shot::PluginLists lists(m_host);
    if (!lists.available()) {
        return {};
    }
    const std::string text = lists.json("config_list");
    if (text.empty()) {
        return {};
    }
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return {};
    }
    for (const auto& item : parsed) {
        if (!item.is_object()) {
            continue;
        }
        const auto owner = item.find("owner");
        const auto id = item.find("id");
        const auto value = item.find("value");
        if (owner == item.end() || id == item.end() || value == item.end() ||
            !owner->is_string() || !id->is_string() || !value->is_string()) {
            continue;
        }
        // 保存目录以 image_saver 插件的配置为准；读不到时交给 LongImageSaver 退回 Pictures。
        if (owner->get<std::string>() == kImageSaverOwner &&
            id->get<std::string>() == kImageSaverDirectoryKey) {
            return value->get<std::string>();
        }
    }
    return {};
}

std::string LongCapture::localized(const char* key, const char* fallback) const {
    const char* text = (fallback != nullptr) ? fallback : "";
    if (key != nullptr && m_i18n) {
        text = m_i18n->tr(key, text);
    }
    return std::string(text);
}

void LongCapture::showTrayHint(const char* titleKey, const char* titleFallback,
                               const char* textKey, const char* textFallback) const {
    try {
        if (!m_trayHint) {
            m_trayHint = std::make_unique<TrayHint>();
        }
        const std::string title = localized(titleKey, titleFallback);
        const std::string text = (textKey != nullptr) ? localized(textKey, textFallback)
                                                      : std::string(textFallback ? textFallback : "");
        if (!m_trayHint->show(m_host, title, text)) {
            shot::Logger(m_host).debug("[long_capture] 托盘气泡显示失败（explorer 未运行？）");
        }
    } catch (...) {
        shot::Logger(m_host).warn("[long_capture] 托盘气泡创建异常，已忽略");
    }
}

}  // namespace long_capture
