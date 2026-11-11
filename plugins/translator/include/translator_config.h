#pragma once

// ============================================================================
//  plugins/translator/include/translator_config.h —— 外部语言包的两个配置项
//
//  · enabled   BOOL    是否加载外部语言包（默认 "0" = 关）。
//                默认必须关：一开就会改写 core 文案（例如设置页/托盘的中文），
//                现有 e2e 断言按内置中文/英文写的，默认开会让它们误报。
//  · pack_dirs PATH    语言包目录（options="directory"）；空 = 官方 + 用户两个默认
//                目录，非空 = 只用它（分号分隔多个目录）。
//
//  ShotConfigDesc 的 label/tooltip 是文案 key（config.<配置 key>.label/.tooltip），
//  由设置页按 display("translator.main", key) 解析；语言表在 translator_plugin.cpp。
//
//  线程安全：set_config/get_config 可能在宿主任意线程被调用（启动回灌、设置页
//  应用），所以值用 mutex + atomic 保护；重扫描只登记脏标记，不在这里做 IO。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "plugin_api.h"

namespace translator {

/**
 * @brief 外部语言包的两个配置项：总开关 + 目录覆盖。
 *
 * 只负责"声明 + 存字符串值"：目录怎么解析（PackDirs）、包怎么读（PackScanner）、
 * 条目怎么发（SourcePublisher）都在各自的类里。这样这个类不需要知道任何宿主
 * 服务，宿主升级多传一个配置项也不会踩到这里。
 */
class TranslatorConfig {
public:
    /** @brief 构造：默认关闭、目录为空（= 用官方 + 用户默认目录）。 */
    TranslatorConfig();

    /** @brief 不可拷贝：内部有互斥量，值是跨线程共享状态。 */
    TranslatorConfig(const TranslatorConfig&) = delete;
    /** @brief 不可赋值。 */
    TranslatorConfig& operator=(const TranslatorConfig&) = delete;

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
     * @brief 读一个配置项的当前值（值一律是字符串）。
     * @param[in]  key      配置键，不能为空。
     * @param[out] buf      接收缓冲区，可为 nullptr（此时只查长度）。
     * @param[in]  bufSize  buf 的字节数；大于返回值时才写入（含结尾 NUL）。
     * @return >= 0 = 值的字节数（不含结尾 NUL）；< 0 = key 不认识或参数非法。
     */
    std::int32_t getValue(const char* key, char* buf, std::uint32_t bufSize) const;

    /**
     * @brief 写一个配置项；任意线程可调。
     *
     * 只保存字符串/布尔值，不做扫描也不发事件——重扫描由 TranslatorPlugin 在
     * 下一轮 main 线程的 lists.changed 回调里完成（lists.add 只允许 main 线程处理）。
     *
     * @param[in] key   配置键，不能为空；不认识的键返回负值且不改变任何状态。
     * @param[in] value 字符串形式的新值；enabled 的 "0" / "false" 视为关。
     * @return 0 = 已生效；< 0 = key 不认识或参数非法。
     */
    std::int32_t setValue(const char* key, const char* value);

    /** @brief 是否加载外部语言包（`enabled` 的当前值）。 */
    bool enabled() const noexcept;

    /** @brief 语言包目录配置值（`pack_dirs`）；空串 = 用默认目录。返回本地副本。 */
    std::string packDirs() const;

private:
    /**
     * @brief 按契约把字符串值写进宿主缓冲区并报告值的长度。
     * @param[in]  value   要返回的字符串；不能为 nullptr。
     * @param[out] buf     宿主缓冲区，可为 nullptr（只查询长度）。
     * @param[in]  bufSize buf 的字节数。
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

    mutable std::mutex m_mutex;         /**< 保护 m_packDirs。 */
    std::string m_packDirs;             /**< pack_dirs 的当前值（空 = 默认目录）。 */
    std::atomic<bool> m_enabled{false}; /**< enabled：关闭时撤回全部贡献并停止扫描。 */
};

}  // namespace translator
