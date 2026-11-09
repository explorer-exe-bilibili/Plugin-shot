#pragma once

// ============================================================================
//  include/clipboard_plugin.h —— 剪贴板插件的主体
//
//  职责只有一件事：订阅 capture.done，把保存插件刚写好的文件复制进剪贴板。
//   · start(host)  在实例创建路径里绑定宿主并订阅（本插件没有 ui_provider 的
//                  init/deinit 生命周期，create(index, host) 是唯一能拿到宿主服务
//                  表的时机）；
//   · stop()       在实例销毁前退订；之后宿主不会再回调本实例（EVENT_BUS.md §5）。
//
//  没有窗口、没有线程、没有全局状态：订阅回调在宿主事件分发线程（main）执行，
//  剪贴板写入也在这个线程完成，所以不存在跨线程的剪贴板竞争。
// ============================================================================

#include <cstdint>
#include <string>

#include "clipboard_config.h"
#include "clipboard_writer.h"
#include "plugin_api.h"

namespace clipboard {

/**
 * @brief 剪贴板插件实例：两个配置项 + 一次 capture.done 订阅。
 *
 * 生命周期由 ABI 层的 create/destroy 驱动：create 分配本对象并调 start()，
 * destroy 调 stop() 后释放。所有方法都只在宿主 main 线程调用（插件创建、事件
 * 分发、插件卸载都在 main 线程串行发生），因此内部状态不需要加锁；配置值本身
 * 仍用原子量保存，因为 set_config 可能来自设置页线程。
 */
class ClipboardPlugin {
public:
    /** @brief 构造：订阅尚未建立，start() 之后才收事件。 */
    ClipboardPlugin() = default;

    /** @brief 析构前兜底退订（正常路径由 destroy 调 stop()）。 */
    ~ClipboardPlugin();

    /** @brief 不可拷贝：订阅句柄（this）必须唯一。 */
    ClipboardPlugin(const ClipboardPlugin&) = delete;
    /** @brief 不可赋值。 */
    ClipboardPlugin& operator=(const ClipboardPlugin&) = delete;

    /**
     * @brief 绑定宿主并订阅 capture.done。
     * @param[in] host 宿主服务表；可以为 nullptr（老宿主），此时安静降级为"不订阅"。
     * @return true = 已订阅；false = 宿主没有事件总线，剪贴板功能不可用（实例照常存活）。
     */
    bool start(const ShotHostServices* host);

    /**
     * @brief 退订 capture.done 并释放订阅句柄；重复调用是空操作。
     * @note 必须在实例销毁前、且宿主不再分发本回调之后调用（destroy 路径满足）。
     */
    void stop();

    /**
     * @brief 配置项个数（供 shot.configurable 的 get_config_count 转发）。
     * @return 固定为 2。
     */
    std::uint32_t configCount() const noexcept;

    /**
     * @brief 声明第 index 个配置项（转发给 ClipboardConfig）。
     * @param[in]  index 越界返回 1。
     * @param[out] out   由调用方填好 struct_size；结构太短返回 2。
     * @return 0 = 已填入。
     */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const;

    /**
     * @brief 读配置项当前值（转发给 ClipboardConfig）。
     * @param[in]  key      配置键，不能为空。
     * @param[out] buf      接收缓冲区，可为 nullptr（只查长度）。
     * @param[in]  buf_size buf 的字节数。
     * @return >= 0 = 值的字节数；< 0 = key 不认识或参数非法。
     */
    std::int32_t getConfig(const char* key, char* buf, std::uint32_t buf_size) const;

    /**
     * @brief 写配置项（转发给 ClipboardConfig）。
     * @param[in] key   配置键，不能为空。
     * @param[in] value 字符串形式的新值，不能为空。
     * @return 0 = 已生效；< 0 = key 不认识或参数非法。
     */
    std::int32_t setConfig(const char* key, const char* value);

private:
    /**
     * @brief 事件总线回调：运行在宿主分发线程，只做"拷负载 + 交 handleCaptureDone"。
     * @param[in] user  start() 时订阅的上下文（即本对象）。
     * @param[in] data  负载字节；可能为空。
     * @param[in] len   负载字节数。
     */
    static void onCaptureDone(void* user, const char* topic, const void* data, std::uint32_t len);

    /**
     * @brief 解析 capture.done 负载，按配置调用 ClipboardWriter。
     * @param[in] payload UTF-8 JSON 文本（{"path","width","height","processors"}）。
     */
    void handleCaptureDone(const std::string& payload);

    const ShotHostServices* m_host = nullptr;   /**< 宿主服务表；生命周期 = 插件包。 */
    ClipboardConfig m_config;                   /**< 两个配置项。 */
    ClipboardWriter m_writer;                   /**< 剪贴板写入器（无状态）。 */
    bool m_subscribed = false;                  /**< 是否已成功订阅 capture.done。 */
};

}  // namespace clipboard
