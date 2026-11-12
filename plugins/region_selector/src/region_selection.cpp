// region_selection —— region.select.request / region.selected 选择握手。
//
// 为什么需要这个握手：长截图插件自带 GDI 采集层，不需要宿主流水线。它只想知道
// "用户选了哪块区域/哪个窗口"，由本插件复用现有的悬停吸附/拖框交互完成选择，
// 但确认时**不能**发 capture.request（那会让宿主再采集一次、再落一次盘）。
//
// 事件约定（document/EVENT_BUS.md）：
//   region.select.request  {"purpose":"long_capture"}
//   region.selected        {"purpose":"long_capture","region":{x,y,width,height}}
//   region.selected        {"purpose":"long_capture","cancelled":true}   （Esc/右键）
//
// 坐标：region 是物理虚拟桌面像素（与 capture.request 的 region 完全同一套坐标），
// 发起方按需加回虚拟桌面原点。普通框选（capture.request 触发）完全不受影响。

#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

/**
 * @brief 从 region.select.request 负载里取 purpose。
 *
 * @param[in] payload 事件负载原文。
 * @return purpose 文本；负载不是 JSON 对象 / 没有非空字符串 purpose 时返回空串。
 * @note 不抛异常：事件总线是共享的，坏负载安静忽略。
 */
std::string parsePurpose(const std::string& payload) {
    if (payload.empty()) {
        return {};
    }
    const nlohmann::json data = nlohmann::json::parse(payload, nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
        return {};
    }
    const auto purpose = data.find("purpose");
    if (purpose == data.end() || !purpose->is_string()) {
        return {};
    }
    return purpose->get<std::string>();
}

}  // namespace

bool RegionWindow::Impl::beginPurposeSelection(const std::string& payload) {
    const std::string purpose = parsePurpose(payload);
    if (purpose.empty()) {
        shot::Logger(host).warn("region_selector：region.select.request 缺少 purpose，已忽略");
        return false;
    }
    if (overlayVisible) {
        // 已经在框选：可能是普通区域截屏，也可能是另一次选择请求。不重开覆盖层
        // （避免打断用户正在拖的选区），但给本次发起方回一条取消，别让它等到超时。
        shot::Logger(host).debug("region_selector：正在框选，忽略新的选择请求（purpose=" + purpose +
                                 "），已回取消");
        publishPurposeCancelled(purpose);
        return false;
    }
    if (!startSelection(purpose)) {
        shot::Logger(host).warn("region_selector：选择请求启动失败（purpose=" + purpose +
                                "），回一条取消");
        publishPurposeCancelled(purpose);
        return false;
    }
    return true;
}

void RegionWindow::Impl::publishPurposeSelection(const std::string& purpose,
                                                 const ScreenRect& region) const {
    shot::EventBus bus(host);
    if (!bus.available()) {
        shot::Logger(host).warn("region_selector：宿主没有事件总线，选择结果无法交给发起方");
        return;
    }
    const nlohmann::json payload = {
        {"purpose", purpose},
        {"region",
         {{"x", region.x}, {"y", region.y}, {"width", region.width}, {"height", region.height}}},
    };
    if (!bus.publish(kTopicRegionSelected, payload.dump())) {
        shot::Logger(host).warn("region_selector：发布 region.selected 失败（队列满或参数非法）");
        return;
    }
    char message[256] = {};
    std::snprintf(message, sizeof(message),
                  "region_selector：已回选择结果 purpose=%s region={x:%d,y:%d,width:%d,height:%d}"
                  "（物理虚拟桌面像素）",
                  purpose.c_str(), region.x, region.y, region.width, region.height);
    shot::Logger(host).info(message);
}

void RegionWindow::Impl::publishPurposeCancelled(const std::string& purpose) const {
    shot::EventBus bus(host);
    if (!bus.available()) {
        return;
    }
    const nlohmann::json payload = {
        {"purpose", purpose},
        {"cancelled", true},
    };
    if (!bus.publish(kTopicRegionSelected, payload.dump())) {
        shot::Logger(host).warn("region_selector：发布 region.selected(cancelled) 失败");
        return;
    }
    shot::Logger(host).info("region_selector：已回取消结果 purpose=" + purpose);
}

}  // namespace region_selector
