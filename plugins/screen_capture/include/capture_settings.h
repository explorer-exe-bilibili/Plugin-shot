#pragma once

#include <atomic>
#include <cstdint>

#include "plugin_api.h"

namespace screen_capture {

/**
 * @brief 采集插件的配置状态（`shot.configurable`）：include_cursor 与 source 两项。
 *
 * 抓屏在流水线 worker 线程、配置写入在宿主 main 线程（设置页"应用"/启动回灌），
 * 两边并发访问同一个插件实例，所以这里用 std::atomic 保存：一次抓屏只在开头读
 * 一次快照，不会出现"同一张图上半截带光标、下半截不带"这种半新半旧的结果。
 *
 * 插件只声明与读写配置值，界面与落盘归宿主（document/SETTINGS.md）；不认识的 key
 * 一律返回负值，宿主升级后多灌一项配置也不会踩到这里。
 */
class CaptureSettings {
public:
    /** @brief 构造：include_cursor 默认开启、source 默认 monitor（抓整个输出）。 */
    CaptureSettings() noexcept = default;

    /**
     * @brief 本次抓屏是否要把鼠标光标画进截图（仅 Windows 有可执行语义）。
     * @return true = 合成光标；false = 只要干净桌面图。
     * @note 流水线 worker 线程读；单次抓屏只读一次，取到的是调用瞬间的值。
     */
    bool includeCursor() const noexcept {
        return m_includeCursor.load(std::memory_order_relaxed);
    }

    /**
     * @brief 覆盖 include_cursor 的值（只改内存，落盘由宿主负责）。
     * @param[in] enabled true = 合成光标。
     * @note 可能在宿主 main 线程上调用；atomic 保证 worker 线程不会读到撕裂的值。
     */
    void setIncludeCursor(bool enabled) noexcept {
        m_includeCursor.store(enabled, std::memory_order_relaxed);
    }

    /**
     * @brief Wayland 门户要共享的类型：true = 单个窗口，false = 整个输出（monitor）。
     * @return true = 共享一个窗口；false = 共享整个输出。
     * @note 只影响下一次建立/重建门户会话；Windows 实现忽略它。
     */
    bool captureWindow() const noexcept {
        return m_captureWindow.load(std::memory_order_relaxed);
    }

    /**
     * @brief 覆盖 source 的值（"monitor" → false，"window" → true）。
     * @param[in] window true = 共享单个窗口。
     * @note 可能在宿主 main 线程上调用；Wayland 采集层在下一次调用时比较并重建会话。
     */
    void setCaptureWindow(bool window) noexcept {
        m_captureWindow.store(window, std::memory_order_relaxed);
    }

private:
    std::atomic<bool> m_includeCursor{true};  /**< 为什么要 atomic：见类注释的线程模型。 */
    std::atomic<bool> m_captureWindow{false}; /**< 为什么是 atomic：set_config 与采集线程并发。 */
};

// ---------------------------------------------------------------------------
//  shot.configurable 的四个 ABI 回调
//
//  签名由契约固定（ShotConfigurableV1），只能是自由函数（STYLE.md §6.7 的合法例外）；
//  定义在 capture_settings.cpp，与配置声明放同一个文件（一个能力一个 .cpp）。
//  self 是 PluginInstance*，不是插件状态本身，回调里先 state_of(self) 再转回
//  ScreenCaptureState —— 这是能力回调最容易写错的一步。
// ---------------------------------------------------------------------------

/**
 * @brief 配置项个数（`get_config_count`）。
 * @param[in] self 插件实例；忽略：配置项数量与实例无关。
 * @return 配置项个数（当前为 2）。
 */
std::uint32_t capture_config_count(void* self);

/**
 * @brief 声明第 index 个配置项（`get_config_desc`）。
 * @param[in]  self  插件实例；忽略。
 * @param[in]  index 从 0 开始，越界返回 1。
 * @param[out] out   由宿主提供并填好 struct_size；太短时返回 2，绝不越界写。
 * @return 0 = 已填入；1 = index 越界/参数不合法；2 = out 结构太短。
 */
std::int32_t capture_config_desc(void* self, std::uint32_t index, ShotConfigDesc* out);

/**
 * @brief 读一个配置项的当前值（`get_config`）。
 * @param[in]  self     插件实例；为空时按默认值回答（宿主可能只查长度、不带实例）。
 * @param[in]  key      配置键；nullptr 或未知键返回 -1。
 * @param[out] buf      接收缓冲区，可为 nullptr（此时只查长度）。
 * @param[in]  buf_size buf 的字节数；大于值长度（含 NUL）才写入。
 * @return >= 0 = 值的字节数（不含 NUL）；-1 = key 不认识或参数非法。
 */
std::int32_t capture_config_get(void* self, const char* key, char* buf, std::uint32_t buf_size);

/**
 * @brief 写一个配置项（`set_config`），立即对下一次抓屏生效。
 * @param[in] self  插件实例，为空时返回 -1（没有实例就没有可写的状态）。
 * @param[in] key   配置键；nullptr 或未知键返回 -1。
 * @param[in] value 字符串形式的新值；只接受 "1"/"0"/"true"/"false"（含大写），
 *                  其它写法按非法值拒绝并保留原值，避免把 "yes"/"2" 静默当成真。
 * @return 0 = 已生效；-1 = key 不认识、值非法或实例为空。
 */
std::int32_t capture_config_set(void* self, const char* key, const char* value);

}  // namespace screen_capture
