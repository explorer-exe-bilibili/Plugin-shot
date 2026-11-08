#pragma once

#include <cstdint>
#include <cstring>
#include <memory>

#include "plugin_api.h"
#include "shot_plugin_helpers.h"

namespace setting_window {

/**
 * @brief 插件侧对一个插件的完整描述：身份证 + 能力目录 + 生命周期。
 *
 * 与 example_processor 保持同一套结构，宿主侧完全不需要区分插件类型。
 */
struct PluginEntry {
    const char* id;                /**< 全局唯一 id（"作者.名字"），必须与 metadata.json 一致。 */
    const char* name;              /**< 界面显示名。 */
    const char* version;           /**< 插件自己的语义化版本，如 "1.0.0"。 */
    void* (*create)();             /**< 创建插件状态；失败返回 nullptr，写 nullptr 表示无状态。 */
    void (*destroy)(void* self);   /**< 释放 create 返回的状态；与 create 成对，可为 nullptr。 */
    const ShotIfaceEntry* ifaces;  /**< 能力目录，必须以 { nullptr, 0, nullptr } 结尾。 */
};

/** @brief 插件 setting_window.main 的静态描述：窗口由插件自己开，实现 shot.ui_provider。 */
extern const PluginEntry kSettingWindow;

/**
 * @brief 宿主拿到的不透明实例：哪个插件 + 插件自己的状态。
 *
 * 能力函数表里收到的 self 也是这个指针，不是插件状态本身；所以每个回调
 * 都要先 state_of(self) 取回插件自己的状态，直接 static_cast<XxxState*>(self)
 * 会把 entry 指针当成状态，读出来是垃圾，写进去则损坏内存。
 */
struct PluginInstance {
    const PluginEntry* entry;  /**< 指向静态插件描述；生命周期 = 整个 DLL。 */
    void* state;               /**< 插件 create 出来的状态，由插件自己解释与释放。 */
};

/**
 * @brief 从宿主给的 self 取回插件状态指针。
 *
 * self 是 PluginInstance*，不是状态本身；这是能力回调最容易写错的一步。
 *
 * @param[in] self 宿主传入的不透明实例，允许为 nullptr。
 * @return 插件状态；self 为空时返回 nullptr。
 */
inline void* state_of(void* self) {
    auto* instance = static_cast<PluginInstance*>(self);
    return (instance == nullptr) ? nullptr : instance->state;
}

/**
 * @brief 按契约把配置值写进宿主缓冲区，并报告值的字节数。
 *
 * 返回值是值本身的长度（不含结尾 NUL）：buf 不够长时只报告长度、不写，
 * 宿主据此扩容后重试。
 *
 * @param[out] buf 宿主缓冲区，可为 nullptr（此时只查询长度）。
 * @param[in]  buf_size buf 的字节数；大于返回值时才会写入，写入内容含结尾 NUL。
 * @param[in]  text 要返回的字符串；不允许为 nullptr。
 * @return >= 0 = 值的字节数（不含结尾 NUL）；-1 = text 为空（调用方传了非法参数）。
 * @warning 返回值 >= 0 只表示长度，负数只表示 key 不认识——不要把"buf 装不下"当成失败。
 */
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

/**
 * @brief 把插件侧的配置项声明整段拷进宿主给的 ShotConfigDesc。
 *
 * 宿主要多少就填多少：先按 out->struct_size 判断宿主认识的结构够不够长，
 * 不够就报 2，绝不把字段写到宿主内存之外。
 *
 * @param[in]  src 插件内的静态配置声明，不能为空。
 * @param[out] out 宿主提供的输出结构；进入时读 struct_size。
 * @return 0 = 已填入；1 = src 或 out 为空；2 = out->struct_size 太短，装不下 tooltip 字段。
 */
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

/**
 * @brief 插件自带的设置窗口：自己开线程、跑 Win32 消息循环，宿主只调 start/stop/post。
 *
 * 窗口归插件自己，宿主从不接触 HWND，也不知道插件开了几个线程。Impl 是不透明
 * （PIMPL）：Win32 类型一个都不出现在头文件里，也就不可能顺着契约漏出去——
 * 这样做不是为了编译快，而是让"泄漏 Win32 类型"在物理上不可能发生。
 */
class SettingWindow {
public:
    /** @brief 构造空的设置窗口对象；此时还没有线程与窗口。 */
    SettingWindow();
    /** @brief 析构：等价于调用 stop()，保证不留下窗口线程。 */
    ~SettingWindow();

    /**
     * @brief 建窗口并启动窗口线程；返回时窗口已就绪（或已失败）。
     *
     * 幂等：已在运行时直接返回 true，重复 init 是空操作。建线程可能抛
     * （线程资源耗尽），这里就地兜住，异常绝不越过 DLL 边界。
     *
     * @param[in] host 宿主服务表，可为 nullptr（老宿主没有这张表）。
     * @return true = 窗口已就绪；false = 线程或窗口创建失败。
     */
    bool start(const ShotHostServices* host);

    /**
     * @brief 关窗口并等待窗口线程退出。
     *
     * 可重复调用；返回时窗口一定已经不在了——宿主不需要、也没办法自己关窗口。
     */
    void stop();

    /**
     * @brief 任意线程可调：把事件排进窗口线程队列并立即返回。
     *
     * 事件名与 data 的 data_len 字节会被拷进队列，投递方在调用返回后即可释放
     * 自己的缓冲区。
     *
     * @param[in] name 事件名；约定是编译期静态字符串，不能为空。
     * @param[in] data 不透明事件数据，可为 nullptr（按空数据入队）。
     * @param[in] len data 的字节数；data 非空时按此长度拷走。
     * @return 0 = 已入队；1 = 窗口不在（已 stop）；2 = 投递失败；-1 = 参数非法。
     */
    std::int32_t post(const char* name, const char* data, std::uint32_t len);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  /**< PIMPL：Win32 类型与线程状态全藏在 Impl 里。 */
};

}  // namespace setting_window
