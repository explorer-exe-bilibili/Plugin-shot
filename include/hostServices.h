#pragma once

#include <cstdint>
#include <queue>
#include <string>
#include <vector>

#include "plugin_api.h"

namespace Host {
using ::ShotHostServices;

/**
 * @brief 宿主收到的一条事件；数据是深拷贝。
 *
 * 投递方（插件）的缓冲区在 postToHost / broadcastEvent 返回后就不再归它管，
 * 所以这里必须存拷贝；存指针会读到别人的栈。
 */
struct HostEvent {
    std::string name;  /**< 事件名，约定为编译期静态字符串。 */
    std::string data;  /**< 事件负载字节的拷贝，可为空。 */
};

namespace HostServices {
    /** @brief 宿主配置读写服务表。 */
    extern const ShotHostConfigV1 g_hostConfig;
    /** @brief 宿主看板广播服务表。 */
    extern const ShotHostBoardEventV1 g_hostBoardEvent;
    /** @brief 宿主事件总线服务表（主题发布/订阅，见 document/EVENT_BUS.md）。 */
    extern const ShotHostEventBusV1 g_eventBus;
    /**
     * @brief 接收插件日志并转写进宿主日志。
     *
     * 全部静态、全部线程安全：这些函数可能在任何插件线程上被调用。
     * @param[in] ctx 未使用，保留给以后的服务上下文。
     * @param[in] level 插件侧等级 0=DEBUG … 3=ERROR。
     * @param[in] msg 日志正文；NULL 时直接返回。
     * @param[in] len 正文字节数；负数表示按 NUL 结尾字符串处理。
     */
    void hostLog(void* ctx, int32_t level, const char* msg, int32_t len);
    /**
     * @brief 按 interface id 向插件提供宿主服务表。
     *
     * 分发宿主配置、看板广播、事件总线、配置目录（shot.host.plugin_config）与
     * 插件注册表（shot.host.plugin_registry），其余 id 返回 nullptr。
     * @param[in] ctx 未使用。
     * @param[in] ifaceId 接口 id；为空时返回 nullptr。
     * @param[in] version 请求的接口版本，目前忽略。
     * @return 服务表指针；不认识的能力（或宿主服务还没装配好）返回 nullptr。
     */
    void* hostQueryInterface(void* ctx, const char* ifaceId, uint32_t version);
    /**
     * @brief 插件向宿主投递一条事件。
     *
     * 可能在插件任意线程被调用：先深拷贝再入队，返回后与投递方缓冲区无关。
     * @param[in] ctx 未使用。
     * @param[in] ev 事件；不能为空，name 不能为空，且必须带 data_len 字段。
     * @return 0 = 已入队；-1 = 参数不合法或缺少 data_len。
     */
    int32_t postToHost(void* ctx, const ShotEvent* ev);
    /**
     * @brief 广播一条事件，同样深拷贝后入队。
     * @param[in] ev 事件；约束同 postToHost。
     * @return 0 = 已入队；-1 = 参数不合法或缺少 data_len。
     */
    int32_t broadcastEvent(const ShotEvent* ev);
    /**
     * @brief 发布一条事件到宿主事件总线（任意线程可调，非阻塞）。
     *
     * 负载由总线深拷贝；队列满或参数非法时返回 -1，事件被丢弃。
     * @param[in] ctx 未使用。
     * @param[in] topic 主题；不能为空。
     * @param[in] data 负载，可为 NULL（len 必须为 0）。
     * @param[in] len 负载字节数，上限 64 KiB。
     * @return 0 = 已入队；-1 = 参数非法、超限或队列满。
     */
    int32_t eventBusPublish(void* ctx, const char* topic, const void* data, uint32_t len);
    /**
     * @brief 订阅一个主题模式（精确 / "prefix.*" / "*"）。
     * @param[in] ctx 未使用。
     * @param[in] topic 主题模式；不能为空。
     * @param[in] callback 匹配时的回调；不能为 NULL。
     * @param[in] user 回调上下文，也是退订句柄。
     * @return 0 = 成功；-1 = 参数非法。
     */
    int32_t eventBusSubscribe(void* ctx, const char* topic,
                              void (*callback)(void* user, const char* topic,
                                               const void* data, uint32_t len),
                              void* user);
    /**
     * @brief 退订某个 user 的全部订阅。
     * @param[in] ctx 未使用。
     * @param[in] user subscribe 时传入的 user；NULL 时返回 -1。
     * @return 0 = 至少退订一条；-1 = 没有匹配。
     */
    int32_t eventBusUnsubscribe(void* ctx, void* user);
    /**
     * @brief 在宿主主线程上把队列中的事件分发给订阅者。
     *
     * main 循环每轮调用一次；回调就在调用者的线程上执行，所以必须由宿主主线程调。
     * 回调里 publish 的新事件会进入下一轮，不会在本次分发里重入。
     */
    void dispatchEvents();
    /**
     * @brief 读一个配置项到调用方缓冲区。
     * @param[in] ctx 未使用。
     * @param[in] key 配置键；NULL 时返回 -1。
     * @param[out] buf 输出缓冲区；可为 NULL（此时只查询长度）。
     * @param[in] buf_size 缓冲区字节数；必须大于值长度（给 NUL 留位），否则返回 -1。
     * @return 值的字节数（不含 NUL）；参数不合法或缓冲区不足返回 -1。
     */
    int32_t hostGetConfig(void* ctx, const char* key, char* buf, uint32_t buf_size);
    /**
     * @brief 写一个配置项（只改内存，落盘需再调 hostSaveConfig）。
     * @param[in] ctx 未使用。
     * @param[in] key 配置键；NULL 时返回 -1。
     * @param[in] value 新值；NULL 时返回 -1。
     * @return 0 = 成功；-1 = 参数不合法。
     */
    int32_t hostSetConfig(void* ctx, const char* key, const char* value);
    /**
     * @brief 把当前配置写盘。
     * @param[in] ctx 未使用。
     */
    void hostSaveConfig(void* ctx);
}// namespace HostServices
/** @brief 宿主侧广播事件队列。 */
extern std::queue<HostEvent> g_broadcastQueue;
/** @brief 宿主事件队列；main 循环从这里取事件。 */
extern std::queue<HostEvent> g_eventQueue;
/** @brief 交给插件的完整宿主服务表；静态生命周期，插件可长期保存指针。 */
extern const ShotHostServices g_hostServices;
}  // namespace Host
