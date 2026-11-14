#pragma once

// ============================================================================
//  include/watermark_plugin.h —— 时间水印 demo 的插件状态
//
//  用户原话："每次截图后，在图片右下角加上拍摄时间水印。"
//
//  实现为 shot.image_processor + shot.configurable：
//    · create 时发布语言表；process 时按配置取本地时间，用内置 5x7 点阵字体
//      画到图像右下角（白字 + 1px 黑描边）；
//    · 配置项：enabled（开关）、format（strftime 格式）、scale（放大 1~4）、
//      margin（距边缘像素）。
//
//  线程模型：process 跑在采集流水线 worker 线程，set_config 可能在宿主 main
//  线程（用户拖设置页滑块），所以配置值用互斥量保护，process 每次读一份快照。
// ============================================================================

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "plugin_api.h"
#include "shot_plugin_helpers/shot_i18n.h"
#include "shot_plugin_helpers/shot_logger.h"

namespace watermark_demo {

/**
 * @brief 时间水印处理器：把本地时间文本画到图像右下角。
 */
class WatermarkPlugin {
public:
    WatermarkPlugin();
    ~WatermarkPlugin();

    WatermarkPlugin(const WatermarkPlugin&) = delete;
    WatermarkPlugin& operator=(const WatermarkPlugin&) = delete;

    /**
     * @brief 绑定宿主并发布语言表。
     * @param[in] host 宿主服务表；可为 nullptr（老宿主，界面文案回退成 key）。
     * @return true = 初始化完成（语言表失败只降级，仍返回 true）。
     */
    bool start(const ShotHostServices* host);

    /** @brief 销毁语言表；可重复调用。 */
    void stop();

    /**
     * @brief 就地给一张 BGRA8 图像加时间水印（shot.image_processor::process）。
     * @param[in,out] image 宿主提供的图像；只改像素，不改结构体字段。
     * @return 0 = 成功（含"关闭/图像太小跳过"）；1 = 图像格式不支持。
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
    /**
     * @brief 按 strftime 格式取当前本地时间（localtime_s / localtime_r 线程安全）。
     * @param[in]  format 格式串；空串或非法串返回 false。
     * @param[out] out    成功时写入文本。
     * @return true = 已生成；false = 时间获取失败或格式化结果为空/过长。
     */
    static bool formatTimestamp(const std::string& format, std::string& out);

    const ShotHostServices* m_host = nullptr; /**< 宿主服务表，生命周期覆盖本实例。 */
    std::unique_ptr<shot::I18n> m_i18n;       /**< 本插件的语言表。 */

    mutable std::mutex m_configMutex;         /**< 保护下面四个配置值（process 与 set 跨线程）。 */
    bool m_enabled = true;                    /**< BOOL 配置项 enabled 的当前值。 */
    std::string m_format = "%Y-%m-%d %H:%M:%S"; /**< STRING 配置项 format 的当前值。 */
    int m_scale = 2;                          /**< INT 配置项 scale（1~4）的当前值。 */
    int m_margin = 16;                        /**< INT 配置项 margin（0~64）的当前值。 */
};

}  // namespace watermark_demo
