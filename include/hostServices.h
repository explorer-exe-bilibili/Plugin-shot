#pragma once

#include <cstdint>
#include <queue>
#include <string>
#include <vector>

#include "plugin_api.h"

namespace Host {
using ::ShotHostServices;

// 宿主收到的事件。data 必须是【拷贝】：投递方的缓冲区在 postToHost 返回之后
// 就不归它管了，这里存指针会读到别人的栈。
struct HostEvent {
    std::string name;
    std::string data;
};

class HostServices {
public:
    // 全部静态、全部线程安全：这些函数可能在任何插件线程上被调用。
    static void hostLog(void* ctx, int32_t level, const char* msg, int32_t len);
    static void* hostQueryInterface(void* ctx, const char* ifaceId, uint32_t version);
    static int32_t postToHost(void* ctx, const ShotEvent* ev);
    static int32_t broadcastEvent(const ShotEvent* ev);
};
extern std::queue<HostEvent> g_broadcastQueue;
extern std::queue<HostEvent> g_eventQueue;
extern const ShotHostServices g_hostServices;
}  // namespace Host
