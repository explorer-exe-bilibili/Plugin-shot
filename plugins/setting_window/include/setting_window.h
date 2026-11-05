#pragma once

#include <cstdint>
#include <cstring>
#include <memory>

#include "plugin_api.h"
#include "shot_plugin_helpers.h"

namespace setting_window {

// 插件侧对"一个插件"的完整描述：身份证 + 能力目录 + 生命周期。
// 与 example_processor 保持同一套结构，宿主侧完全不需要区分插件类型。
struct PluginEntry {
    const char* id;
    const char* name;
    const char* version;
    void* (*create)();
    void (*destroy)(void* self);
    const ShotIfaceEntry* ifaces;  // 以 { nullptr, 0, nullptr } 结尾
};

extern const PluginEntry kSettingWindow;

// 宿主拿到的不透明指针就是它。能力函数表里收到的 self 也是它，
// 所以每个函数都要先 state_of(self) 取回插件自己的状态。
struct PluginInstance {
    const PluginEntry* entry;
    void* state;
};

inline void* state_of(void* self) {
    auto* instance = static_cast<PluginInstance*>(self);
    return (instance == nullptr) ? nullptr : instance->state;
}

// 配置读值的小工具：把一段字符串值按契约写进宿主给的 buf。
// 返回值 = 值的字节数（不含结尾 NUL）；buf 不够长时只报告长度、不写。
inline std::int32_t copy_config_value(char* buf, std::uint32_t buf_size, const char* text) {
    if (text == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(text);
    if (buf != nullptr && buf_size > len) {
        std::memcpy(buf, text, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

inline std::int32_t copy_config_desc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (out == nullptr || src == nullptr) {
        return 1;
    }
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;  // 宿主的结构比本插件认识的还短，装不下
    }
    *out = *src;
    return 0;
}

// 窗口归插件自己：本类在自己开的线程上建窗口、跑消息循环，宿主只调下面三个
// 成员，从不接触 HWND。Impl 是不透明（PIMPL）：Win32 类型一个都不出现在头文件里，
// 也就不可能顺着契约漏出去。
class SettingWindow {
public:
    SettingWindow();
    ~SettingWindow();

    bool start(const ShotHostServices* host);  // 建窗口 + 起线程；返回时窗口已就绪（或已失败）
    void stop();                               // 关窗口 + join；可重复调用

    // 任意线程可调：把事件排进窗口线程的队列，立刻返回。
    // 0 = 已入队；1 = 窗口不在（已 stop）；2 = 投递失败；负值 = 参数非法。
    std::int32_t post(const char* name, const char* data, std::uint32_t len);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace setting_window
