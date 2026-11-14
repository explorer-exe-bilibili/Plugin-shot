#pragma once

// ============================================================================
//  include/image_saver_plugin.h —— 图像保存模板的插件状态
//
//  实现 shot.image_saver：把宿主流水线处理完的 ShotImage 落盘成 PNG / JPEG / BMP。
//  格式、质量、目录全部来自本插件自己的 shot.configurable 配置项——宿主不硬编码
//  任何保存策略（document/CAPTURE.md §2.2）。
//
//  编码用 SDK 内置的 stb_image_write（header-only，SDK 的 third_party/include 里
//  已有），所以不需要 vcpkg / find_package。
//
//  两条容易踩的契约细节：
//    · ShotImage 必须按行寻址（stride 可负、可大于 width*4），不能当连续块；
//    · save 的 path 缓冲由宿主提供（v1 固定 1024 字节），装不下返回 2 而不是截断。
//
//  线程模型：save 只在采集流水线 worker 线程上调用，同一实例不会并发；set_config
//  可能在宿主 main 线程，所以配置值用互斥量保护，save 每次读一份快照。
// ============================================================================

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "plugin_api.h"
#include "shot_plugin_helpers/shot_i18n.h"
#include "shot_plugin_helpers/shot_logger.h"

namespace image_saver_template {

/** @brief 支持的图片格式；配置 ENUM 的候选值是 format.png / format.jpeg / format.bmp。 */
enum class SaverFormat {
    Png,  /**< 无损 PNG（保留 alpha）。 */
    Jpeg, /**< 有损 JPEG（丢弃 alpha，质量 1~100）。 */
    Bmp,  /**< 无压缩 BMP（保留 alpha）。 */
};

/**
 * @brief 图像保存插件实例：三个配置项 + 一次 save 调用。
 */
class ImageSaverPlugin {
public:
    ImageSaverPlugin();
    ~ImageSaverPlugin();

    ImageSaverPlugin(const ImageSaverPlugin&) = delete;
    ImageSaverPlugin& operator=(const ImageSaverPlugin&) = delete;

    /**
     * @brief 绑定宿主并发布语言表。
     * @param[in] host 宿主服务表；可为 nullptr（老宿主，界面文案回退成 key）。
     * @return true = 初始化完成（语言表失败只降级，仍返回 true）。
     */
    bool start(const ShotHostServices* host);

    /** @brief 销毁语言表；可重复调用。 */
    void stop();

    /**
     * @brief 把一张 BGRA8 图像保存到文件（shot.image_saver::save）。
     * @param[in]  image     待保存图像；只读，不得修改像素或结构体。
     * @param[in]  directory 覆盖输出目录（UTF-8）；nullptr 或空串 = 用插件配置。
     * @param[in]  basename  覆盖文件名（不含扩展名，UTF-8）；nullptr 或空串 = 用时间戳。
     * @param[out] path      接收最终文件路径的缓冲（UTF-8），不能为 nullptr。
     * @param[in]  pathSize  path 的字节数。
     * @return 0 = 已落盘；1 = 参数/图像格式非法；2 = path 缓冲不足；3 = 保存失败。
     */
    std::int32_t save(const ShotImage* image, const char* directory, const char* basename,
                      char* path, std::uint32_t pathSize);

    /** @brief 配置项个数（shot.configurable::get_config_count）。 */
    std::uint32_t configCount() const noexcept;

    /** @brief 声明第 index 个配置项（shot.configurable::get_config_desc）。 */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const;

    /** @brief 读配置值（shot.configurable::get_config）。 */
    std::int32_t getConfig(const char* key, char* buf, std::uint32_t bufSize) const;

    /** @brief 写配置值（shot.configurable::set_config）；不认识的 key 返回负数。 */
    std::int32_t setConfig(const char* key, const char* value);

private:
    /** @brief save 开始时读到的配置快照，避免中途被 set_config 改一半。 */
    struct Settings {
        SaverFormat format = SaverFormat::Png; /**< 输出格式。 */
        int quality = 90;                      /**< JPEG 质量（1~100，PNG/BMP 忽略）。 */
        std::string directory;                 /**< 输出目录（UTF-8）；空 = 当前工作目录。 */
    };

    /** @brief 读一份配置快照（加锁）。 */
    Settings settingsSnapshot() const;

    /**
     * @brief 把图像编码并写进 filePath。
     * @param[in]  image    待保存图像（已校验过坐标与像素格式）。
     * @param[in]  settings 配置快照。
     * @param[in]  filePath 目标文件路径（已含扩展名）。
     * @param[out] error    失败时写入中文原因；成功时清空。
     * @return true = 已写盘；false = 编码或写文件失败。
     */
    bool encode(const ShotImage& image, const Settings& settings,
                const std::string& filePath, std::string& error) const;

    const ShotHostServices* m_host = nullptr; /**< 宿主服务表，生命周期覆盖本实例。 */
    std::unique_ptr<shot::I18n> m_i18n;       /**< 本插件的语言表。 */

    mutable std::mutex m_configMutex;         /**< 保护下面三个配置值（save 与 set 跨线程）。 */
    SaverFormat m_format = SaverFormat::Png;  /**< ENUM 配置项 format 的当前值。 */
    int m_quality = 90;                       /**< INT 配置项 quality 的当前值。 */
    std::string m_directory;                  /**< PATH 配置项 directory 的当前值（UTF-8）。 */
};

}  // namespace image_saver_template
