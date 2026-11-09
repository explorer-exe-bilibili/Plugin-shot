#pragma once

// ============================================================================
//  include/tray_config.h —— 托盘的两个开关配置（shot.configurable）
//
//  托盘只需要两个布尔开关：保存成功/截屏失败时要不要弹气泡。它们必须能在任意
//  线程被读（托盘线程弹气泡时）和写（宿主/设置页在别处推新值），所以用原子量
//  而不是普通 bool —— 配置服务明确允许"运行中被任意线程调用 set_config"。
// ============================================================================

#include <atomic>
#include <cstdint>

#include "plugin_api.h"

namespace tray {

/**
 * @brief 托盘的两个开关配置，对应 `shot.configurable` 的 `notify` / `notify_on_error`。
 *
 * 为什么单独成一个类：配置的声明（ShotConfigDesc 表）、字符串值解析与"当前值"
 * 是托盘的一件独立职责，和窗口/线程（TrayIcon）混在一起会让头文件不再是单一契约
 * （STYLE.md §6.6 / §6.8）。ABI 回调在 plugin_entry.cpp 里只做参数转发。
 *
 * 线程安全：读/写都可在任意线程发生，内部用 std::atomic<bool> 保存当前值。
 */
class TrayConfig {
public:
    /** @brief 构造：两个开关都取默认值（都为开）。 */
    TrayConfig();

    /** @brief 不可拷贝：值是跨线程共享的原子状态，复制没有意义。 */
    TrayConfig(const TrayConfig&) = delete;
    /** @brief 不可赋值。 */
    TrayConfig& operator=(const TrayConfig&) = delete;

    /**
     * @brief 配置项个数（供 shot.configurable 的 get_config_count 转发）。
     * @return 固定为 2。
     */
    std::uint32_t configCount() const noexcept;

    /**
     * @brief 声明第 index 个配置项。
     * @param[in]  index 从 0 开始；越界返回 1。
     * @param[out] out   由调用方提供并填好 struct_size；结构太短返回 2。
     * @return 0 = 已填入；1 = index 越界或 out 为空；2 = out 结构太短。
     */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const;

    /**
     * @brief 读一个配置项的当前值（值一律是字符串 "1" / "0"）。
     * @param[in]  key      配置键，不能为空。
     * @param[out] buf      接收缓冲区，可为 nullptr（此时只查长度）。
     * @param[in]  buf_size buf 的字节数；大于返回值时才写入（含结尾 NUL）。
     * @return >= 0 = 值的字节数（不含结尾 NUL）；< 0 = key 不认识或参数非法。
     */
    std::int32_t getValue(const char* key, char* buf, std::uint32_t bufSize) const;

    /**
     * @brief 写一个配置项；任意线程可调。
     * @param[in] key   配置键，不能为空；不认识的键返回负值且不改变任何状态。
     * @param[in] value 字符串形式的新值；"0" / "false" 视为关，其余视为开。
     * @return 0 = 已生效；< 0 = key 不认识或参数非法。
     */
    std::int32_t setValue(const char* key, const char* value);

    /** @brief 截屏成功后是否弹气泡通知（`notify`）。 */
    bool notifyOnCaptureDone() const noexcept;

    /** @brief 截屏失败后是否弹气泡通知（`notify_on_error`）。 */
    bool notifyOnCaptureError() const noexcept;

private:
    /**
     * @brief 按契约把字符串值写进宿主缓冲区并报告值的长度。
     * @param[in]  value   要返回的字符串；不能为 nullptr。
     * @param[out] buf     宿主缓冲区，可为 nullptr（只查询长度）。
     * @param[in]  buf_size buf 的字节数。
     * @return >= 0 = 值的字节数（不含结尾 NUL）；-1 = value 为空。
     */
    static std::int32_t copyValue(const char* value, char* buf, std::uint32_t bufSize);

    /**
     * @brief 把静态配置声明整段拷进宿主给的 ShotConfigDesc。
     * @param[in]  src 插件内的静态声明，不能为空。
     * @param[out] out 宿主提供的输出结构；进入时读 struct_size。
     * @return 0 = 已填入；1 = 参数为空；2 = out->struct_size 装不下声明。
     */
    static std::int32_t copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out);

    std::atomic<bool> m_notifyOnDone{true};   /**< notify：保存成功后弹气泡。 */
    std::atomic<bool> m_notifyOnError{true};  /**< notify_on_error：失败时弹气泡。 */
};

}  // namespace tray
