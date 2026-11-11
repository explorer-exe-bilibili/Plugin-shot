#pragma once

#include <cstdint>
#include <string>

#include "plugin_api.h"

// ============================================================================
//  HostConfigurable —— 宿主自己的 shot.configurable 实现（"core" 伪插件）
//
//  设置页里"宿主设置"不是特例：它和插件配置走同一套 ShotConfigDesc，
//  由配置目录把它作为索引 0 暴露。配置项见 document/SETTINGS.md §5。
//
//  这里的 key 是【不带前缀】的（"logging.level"），"core/" 前缀只由配置目录
//  在持久化时拼上；插件侧则各自用自己声明过的 key。
// ============================================================================

namespace Host {

/**
 * @brief 宿主设置的提供者：把语言、日志开关与主题色映射到宿主的实际状态。
 *
 * 读值返回的是"现在的真实状态"（不是磁盘缓存），写值立即生效并同步影响日志等
 * 运行期状态。主题色的运行期值只保存在这里的静态存储里，落盘仍由配置目录负责。
 * 因为宿主本身不会被卸载，这份静态服务表在进程整个生命周期内都有效。
 */
class HostConfigurable {
public:
    /** @brief 提供者 id：配置目录索引 0 固定为 "core"。 */
    static const char* id();
    /** @brief 提供者在设置页里的显示名。 */
    static const char* name();
    /** @brief 宿主版本号（来自 CMake 生成的 version.h）。 */
    static const char* version();
    /** @brief 取静态函数表；生命周期与进程相同，调用方只读、不得释放。 */
    static const ShotConfigurableV1* service();

private:
    /**
     * @brief 配置项个数。
     * @param[in] self 忽略：宿主设置只有一个实现。
     * @return 当前 5 项：语言、日志级别、控制台日志、文件日志、主题色。
     */
    static uint32_t getConfigCount(void* self);

    /**
     * @brief 声明第 index 个配置项。
     * @param[in]  index 0 .. 4。
     * @param[out] out   调用方填好 struct_size；长度不够时返回 2。
     * @return 0 = 成功；1 = 参数非法；2 = out 太短。
     */
    static int32_t getConfigDesc(void* self, uint32_t index, ShotConfigDesc* out);

    /**
     * @brief 读一个配置项的当前实际值。
     * @return >= 0 = 值长度（不含 NUL）；< 0 = key 不认识。
     */
    static int32_t getConfig(void* self, const char* key, char* buf, uint32_t bufSize);

    /**
     * @brief 写一个配置项并立即生效。
     * @return 0 = 已生效；< 0 = key 不认识、值非法或对应 sink 不可用。
     */
    static int32_t setConfig(void* self, const char* key, const char* value);

    /**
     * @brief 按契约把字符串值写进宿主缓冲区。
     * @param[out] buf     接收缓冲区，可为空（只查长度）。
     * @param[in]  bufSize 缓冲区字节数；大于值长度（含 NUL）才写入。
     * @param[in]  value   要返回的字符串。
     * @return 值的字节数（不含 NUL）。
     */
    static int32_t copyValue(char* buf, uint32_t bufSize, const std::string& value);
};

}  // namespace Host
