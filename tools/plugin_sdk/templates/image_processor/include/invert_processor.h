#pragma once

// ============================================================================
//  include/invert_processor.h —— 图像处理器模板的插件状态
//
//  演示 shot.image_processor 的两条硬规则：
//    1) 只改像素、不改 ShotImage 结构体字段（宿主按自己的结构体管理缓冲）；
//    2) 必须按行寻址：stride 以字节计、可以为负（自底向上存储）、也可以大于
//       width*channels（行尾有填充），把 data 当成连续块是最常见的越界来源。
//
//  处理内容故意选最简单的反色（B/G/R 取反、A 保留），强度 0~100 控制混合比例：
//  真正做滤镜时只要替换 process() 里逐像素那一段。
//
//  线程模型：process 跑在采集流水线 worker 线程，set_config 可能在宿主 main
//  线程；两者共享的配置用互斥量保护，process 每次读一份快照。
// ============================================================================

#include <cstdint>
#include <memory>
#include <mutex>

#include "plugin_api.h"
#include "shot_plugin_helpers/shot_i18n.h"
#include "shot_plugin_helpers/shot_logger.h"

namespace image_processor_template {

/**
 * @brief 反色处理器：可开关、可调强度的 BGRA8 逐像素反色。
 */
class InvertProcessor {
public:
    InvertProcessor();
    ~InvertProcessor();

    InvertProcessor(const InvertProcessor&) = delete;
    InvertProcessor& operator=(const InvertProcessor&) = delete;

    /**
     * @brief 绑定宿主并发布语言表。
     * @param[in] host 宿主服务表；可为 nullptr（老宿主，界面文案回退成 key）。
     * @return true = 初始化完成（语言表失败只降级，仍返回 true）。
     */
    bool start(const ShotHostServices* host);

    /** @brief 销毁语言表；可重复调用。 */
    void stop();

    /**
     * @brief 就地处理一张 BGRA8 图像（shot.image_processor::process）。
     * @param[in,out] image 宿主提供的图像；像素可改，结构体字段不可改。
     * @return 0 = 成功；1 = 参数/格式不支持（宿主当成"这个插件没处理成功"并记 warn）。
     */
    std::int32_t process(ShotImage* image);

    /** @brief 配置项个数（shot.configurable::get_config_count）。 */
    std::uint32_t configCount() const noexcept;

    /** @brief 声明第 index 个配置项（shot.configurable::get_config_desc）。 */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const;

    /** @brief 读配置值（shot.configurable::get_config）。 */
    std::int32_t getConfig(const char* key, char* buf, std::uint32_t bufSize) const;

    /** @brief 写配置值（shot.configurable::set_config）；不认识的 key 返回负数。 */
    std::int32_t setConfig(const char* key, const char* value);

private:
    const ShotHostServices* m_host = nullptr; /**< 宿主服务表，生命周期覆盖本实例。 */
    std::unique_ptr<shot::I18n> m_i18n;       /**< 本插件的语言表。 */

    mutable std::mutex m_configMutex;         /**< 保护下面两个配置值（process 与 set 跨线程）。 */
    bool m_enabled = true;                    /**< BOOL 配置项 enabled 的当前值。 */
    int m_strength = 100;                     /**< INT 配置项 strength（0~100）的当前值。 */
};

}  // namespace image_processor_template
