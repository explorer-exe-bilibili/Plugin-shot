#pragma once

// ============================================================================
//  include/long_capture_config.h —— 本插件的两个配置项
//
//  值一律按契约用字符串表达（跨边界不需要宽度/对齐约定），这里负责字符串与内部
//  类型的校验转换：非法值安静回退默认、不认识的 key 返回负值，宿主升级后多传一项
//  也不会坏（document/ABI_POLICY.md §5）。
//
//  label / tooltip 写的是语言表 key，不是中文：设置页按
//  display(owner, key) = i18n_list[(owner, key)] ?? key 解析，文案本体在
//  long_capture.cpp 的 kLangTables 里（两边的 key 拼写必须严格一致）。
// ============================================================================

#include <cstdint>
#include <mutex>
#include <string>

#include "plugin_api.h"

namespace long_capture {

/**
 * @brief 长截图插件的配置：触发热键 + 画布高度上限。
 *
 * @note set_config 可能来自宿主 main 线程（设置页应用 / 启动回灌），而会话线程
 *       每帧读一次快照；两处用同一把互斥锁串起来，持锁期间只做字符串拷贝，
 *       不做任何窗口/文件操作。
 */
class LongCaptureConfig {
public:
    /** @brief 配置快照：一次会话开始时取一次，会话中途改配置只影响下一次。 */
    struct Snapshot {
        std::string hotkey;      /**< 触发组合键文本，如 "Ctrl+Alt+L"；空串 = 不用热键触发。 */
        int maxHeight = 30000;   /**< 长图高度上限（像素），画布达到后停止继续拼接。 */
    };

    /** @brief 构造：两个配置项取默认值。 */
    LongCaptureConfig();

    /** @brief 不可拷贝：内部持有互斥锁。 */
    LongCaptureConfig(const LongCaptureConfig&) = delete;
    /** @brief 不可赋值。 */
    LongCaptureConfig& operator=(const LongCaptureConfig&) = delete;

    /**
     * @brief 返回配置项个数；当前固定为 2。
     * @return 配置项个数，用于 ShotConfigurableV1::get_config_count。
     */
    static std::uint32_t itemCount() noexcept;

    /**
     * @brief 声明第 index 个配置项（描述符全部是插件内的静态数据）。
     * @param[in]  index 从 0 开始，必须小于 itemCount()。
     * @param[out] out   调用方提供并填好 struct_size；插件据此判断能写哪些字段。
     * @return 0 = 已填入；1 = index 越界或 out 为空；2 = out 结构太短。
     */
    static std::int32_t describe(std::uint32_t index, ShotConfigDesc* out) noexcept;

    /**
     * @brief 读一个配置项；buf 装得下（含结尾 NUL）时才写入。
     * @param[in]  key      配置键，不能为 nullptr。
     * @param[out] buf      接收缓冲区，可为 nullptr（此时只查询长度）。
     * @param[in]  bufSize  buf 的字节数。
     * @return >= 0 = 值的字节数（不含 NUL）；负值 = key 不认识或参数非法。
     */
    std::int32_t get(const char* key, char* buf, std::uint32_t bufSize) const;

    /**
     * @brief 写一个配置项；值非法时返回负值并保持原值不变。
     * @param[in] key   配置键，不能为 nullptr。
     * @param[in] value 字符串形式的新值，不能为 nullptr。
     * @return 0 = 已生效；负值 = key 不认识或值非法。
     */
    std::int32_t set(const char* key, const char* value);

    /**
     * @brief 取一份一致的配置快照（加锁拷贝）。
     * @return 当前配置值。
     */
    Snapshot snapshot() const;

private:
    /**
     * @brief 按契约把配置值写进宿主缓冲区，并报告值本身的字节数。
     * @param[out] buf     宿主缓冲区，可为 nullptr（只查长度）。
     * @param[in]  bufSize buf 的字节数；大于返回值才会写入（含结尾 NUL）。
     * @param[in]  value   要返回的字符串，不能为 nullptr。
     * @return >= 0 = 值的字节数（不含 NUL）；-1 = value 为空或超长。
     */
    static std::int32_t copyValue(char* buf, std::uint32_t bufSize, const char* value) noexcept;

    /**
     * @brief 把插件内的静态配置声明整段拷进宿主给的 ShotConfigDesc。
     * @param[in]  src 插件内的静态配置声明，不能为空。
     * @param[out] out 宿主提供的输出结构；进入时读 struct_size。
     * @return 0 = 已填入；1 = src 或 out 为空；2 = out->struct_size 太短。
     */
    static std::int32_t copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) noexcept;

    mutable std::mutex m_mutex; /**< 保护 m_values：get/set/snapshot 可能来自不同线程。 */
    Snapshot m_values;          /**< 当前配置值；只在持有 m_mutex 时读写。 */
};

}  // namespace long_capture
