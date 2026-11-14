#pragma once

// ============================================================================
//  include/archive_plugin.h —— 日期归档 demo 的插件状态
//
//  用户原话："截图保存后自动按日期归档到「年-月-日」文件夹。"
//
//  实现为纯订阅者（shot.configurable + 订阅 capture.done）：
//    · start / create 路径：发布语言表 → 起 worker 线程 → 订阅 capture.done；
//    · 事件回调只做一件事：把 path 拷进入队缓冲并唤醒 worker，绝不做文件 IO
//      （宿主事件分发线程被阻塞会拖住所有插件，document/EVENT_BUS.md §5）；
//    · worker 线程：<目标根>/YYYY-MM-DD/ 建目录 → 重名加 _1/_2 → move 或 copy；
//    · stop / destroy 路径：先退订（保证不再有新任务），再让 worker 处理完已入队
//      的任务后退出，最后 join。
//
//  线程模型：配置值（根目录、move/copy）由互斥量保护；事件回调与 set_config 可能
//  在宿主 main 线程，worker 在插件自己的线程。
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "plugin_api.h"
#include "shot_plugin_helpers/shot_event_bus.h"
#include "shot_plugin_helpers/shot_i18n.h"
#include "shot_plugin_helpers/shot_logger.h"

namespace archive_demo {

/** @brief 归档方式：移动（默认，节省空间）或复制（保留原文件）。 */
enum class ArchiveMode {
    Move, /**< rename 优先；跨卷/被占用时回退 copy + 删除。 */
    Copy, /**< 只复制，原文件保留。 */
};

/**
 * @brief 日期归档插件实例：两个配置项 + 一条 capture.done 订阅 + 一个 worker 线程。
 */
class ArchivePlugin {
public:
    ArchivePlugin();
    ~ArchivePlugin();

    ArchivePlugin(const ArchivePlugin&) = delete;
    ArchivePlugin& operator=(const ArchivePlugin&) = delete;

    /**
     * @brief 绑定宿主：发布语言表、起 worker 线程、订阅 capture.done。
     * @param[in] host 宿主服务表；可为 nullptr（老宿主，事件订阅不可用）。
     * @return true = 订阅成功；false = 老宿主无事件总线或线程创建失败（安静降级）。
     */
    bool start(const ShotHostServices* host);

    /** @brief 退订事件总线、等 worker 处理完已入队任务后 join；重复调用是空操作。 */
    void stop();

    /** @brief 配置项个数（shot.configurable::get_config_count）。 */
    std::uint32_t configCount() const noexcept;

    /** @brief 声明第 index 个配置项（shot.configurable::get_config_desc）。 */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const;

    /** @brief 读配置值（shot.configurable::get_config）。 */
    std::int32_t getConfig(const char* key, char* buf, std::uint32_t bufSize) const;

    /** @brief 写配置值（shot.configurable::set_config）；不认识的 key 返回负数。 */
    std::int32_t setConfig(const char* key, const char* value);

private:
    /**
     * @brief capture.done 的宿主回调：解析 path 后入队，立刻返回（不在分发线程做 IO）。
     * @param[in] user  this。
     * @param[in] topic 主题（固定 "capture.done"）。
     * @param[in] data  负载字节；回调返回后失效。
     * @param[in] len   负载字节数。
     */
    static void onCaptureDone(void* user, const char* topic, const void* data, std::uint32_t len);

    /** @brief worker 主循环：等队列 → 取一个路径 → 归档；收到停止信号且队列空后退出。 */
    void workerLoop();

    /**
     * @brief 归档一个文件：建日期目录、处理重名、按配置 move/copy。
     * @param[in] sourcePath 源文件路径（UTF-8，来自 capture.done）。
     */
    void archiveOne(const std::string& sourcePath);

    const ShotHostServices* m_host = nullptr; /**< 宿主服务表，生命周期覆盖本实例。 */
    std::unique_ptr<shot::I18n> m_i18n;       /**< 本插件的语言表。 */
    bool m_subscribed = false;                /**< 是否已成功订阅 capture.done。 */

    mutable std::mutex m_configMutex;         /**< 保护下面两个配置值。 */
    std::string m_rootDirectory;              /**< PATH 配置项 directory 的当前值；空 = 源目录。 */
    ArchiveMode m_mode = ArchiveMode::Move;   /**< ENUM 配置项 mode 的当前值。 */

    std::mutex m_queueMutex;                  /**< 保护任务队列与停止标记。 */
    std::condition_variable m_queueCv;        /**< worker 等待/唤醒用。 */
    std::deque<std::string> m_queue;          /**< 待归档的源文件路径（UTF-8）。 */
    bool m_stopping = false;                  /**< true = worker 处理完队列后退出。 */
    std::thread m_worker;                     /**< 归档 worker 线程；未启动时 !joinable。 */
};

}  // namespace archive_demo
