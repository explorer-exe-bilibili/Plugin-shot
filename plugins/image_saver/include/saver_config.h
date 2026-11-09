#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "plugin_api.h"

namespace image_saver {

/**
 * @brief 保存插件的四个配置项：输出目录、格式、质量、文件名模板。
 *
 * 值一律按契约用字符串表达（跨边界不需要宽度/对齐约定），这里负责字符串与内部
 * 类型的校验转换：非法值安静回退默认值，宿主升级后多传一个不认识的 key 只返回
 * 负值、插件照常可用（document/ABI_POLICY.md §5）。
 *
 * @note set_config 可能在运行中被任意线程调用（用户拖滑块），而 save 在宿主调用
 *       线程执行，两者会并发读写同一份配置——所以这里用一把互斥锁保护；读写都只
 *       做一次字符串拷贝，不在持锁期间做文件 IO 或图像编码。
 */
class SaverConfig {
public:
    /** @brief 配置快照：save 先取快照再编码，避免拿着锁做慢操作。 */
    struct Snapshot {
        std::string directory;  /**< 输出目录（UTF-8）；空 = 使用系统图片目录。 */
        std::string format;     /**< 文件格式："png" / "jpeg"。 */
        int quality = 90;       /**< JPEG 质量 1..100；PNG 忽略。 */
        std::string filename;   /**< 文件名模板，不含扩展名，可含时间占位符。 */
    };

    /** @brief 构造：四个配置项全部取默认值（默认值只在配置描述符里写一遍）。 */
    SaverConfig();

    /** @brief 不可拷贝：内部持有互斥锁，拷贝语义没有意义。 */
    SaverConfig(const SaverConfig&) = delete;
    /** @brief 不可赋值。 */
    SaverConfig& operator=(const SaverConfig&) = delete;

    /**
     * @brief 返回配置项个数；当前固定为 4。
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
     * @brief 读一个配置项的值；buf 装得下（含结尾 NUL）时才写入。
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
     * @return 当前配置值的拷贝；调用方拿到后不再持有锁。
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

    mutable std::mutex m_mutex;  /**< 保护 m_values：get/set/snapshot 可能来自不同线程。 */
    Snapshot m_values;           /**< 当前配置值；只在持有 m_mutex 时读写。 */
};

}  // namespace image_saver
