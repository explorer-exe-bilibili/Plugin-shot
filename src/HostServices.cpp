#include "hostServices.h"

#include <iostream>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>

using namespace Host;

// ---------------------------------------------------------------------------
// 宿主服务表
//
// 静态实例：它必须在插件实例的整个生命周期内有效，所以插件可以放心把指针存起来。
// 将来新增服务就往后追加字段，插件的 SHOT_HAS_FIELD 检查会自动兼容。
// ---------------------------------------------------------------------------

static std::mutex logMutex;
static std::mutex eventMutex;

void HostServices::hostLog(void* /*ctx*/, int32_t level, const char* msg, int32_t len) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (msg == nullptr) {
        return;
    }
    std::ostream& os = (level >= SHOT_LOG_WARN) ? std::cerr : std::cout;
    os << "    [plugin] ";
    if (len < 0) {
        os << msg;
    } else {
        os.write(msg, static_cast<std::streamsize>(len));
    }
    os << '\n';
}

void* HostServices::hostQueryInterface(void* /*ctx*/, const char* /*ifaceId*/, uint32_t /*version*/) {
    return nullptr;  // 宿主暂时不对外提供服务
}

// 可能在插件的任意线程上被调用：先拷一份再入队，投递方返回后就与这份数据无关了。
int32_t HostServices::postToHost(void* /*ctx*/, const ShotEvent* ev) {
    if (ev == nullptr || ev->name == nullptr) {
        return -1;
    }
    if (!SHOT_HAS_FIELD(ev, ShotEvent, data_len)) {
        return -1;  // 老插件的事件里没有 data_len，不猜
    }

    HostEvent event;
    event.name = ev->name;
    event.data = ev->data;
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        g_eventQueue.push(std::move(event));
    }

    const std::string line =
        "[host] received event " + std::string(ev->name) + " (" + std::to_string(ev->data_len) + " bytes)";
    hostLog(nullptr, SHOT_LOG_INFO, line.c_str(), -1);
    return 0;
}

int32_t HostServices::broadcastEvent(const ShotEvent* ev) {
    if (ev == nullptr || ev->name == nullptr) {
        return -1;
    }
    if (!SHOT_HAS_FIELD(ev, ShotEvent, data_len)) {
        return -1;  // 老插件的事件里没有 data_len，不猜
    }

    HostEvent event;
    event.name = ev->name;
    event.data = ev->data;
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        g_eventQueue.push(std::move(event));
    }

    const std::string line =
        "[host] broadcast event " + std::string(ev->name) + " (" + std::to_string(ev->data_len) + " bytes)";
    hostLog(nullptr, SHOT_LOG_INFO, line.c_str(), -1);
    return 0;
}

std::queue<HostEvent> Host::g_broadcastQueue;
std::queue<HostEvent> Host::g_eventQueue;

const ShotHostServices Host::g_hostServices = {
    .struct_size     = sizeof(ShotHostServices),
    .abi_major       = SHOT_ABI_MAJOR,
    .abi_minor       = SHOT_ABI_MINOR,
    .reserved0       = 0,
    .host_context    = nullptr,
    .log             = &Host::HostServices::hostLog,
    .query_interface = &Host::HostServices::hostQueryInterface,
    .post_to_host    = &Host::HostServices::postToHost,
};
