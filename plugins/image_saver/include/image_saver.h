#pragma once

#include <cstdint>
#include <memory>

#include "plugin_api.h"
#include "saver_config.h"

namespace shot {
class I18n;  // 多语言封装：定义在 shot_plugin_helpers/shot_i18n.h，只在本类 .cpp 里用完整类型
}  // namespace shot

namespace image_saver {

/**
 * @brief 图像保存插件的主体：把契约回调转成类方法，自己持有配置。
 *
 * 保存能力和可配置能力在同一个类里，因为它们是同一份状态的两个门面——配置就是
 * 保存策略；编码与路径解析已经拆到 ImageEncoder / OutputPath，这里只剩参数校验
 * 和把两者串起来（STYLE §6.6 / §6.8）。
 *
 * @note 能力函数表里的 self 是 PluginInstance*，不是本类；回调里先 state_of(self)
 *       再 static_cast 回 ImageSaver（STYLE §7 的硬约束）。
 */
class ImageSaver {
public:
    /** @brief 构造：配置取默认值；语言表在拿到宿主服务表后由 start() 加载。 */
    ImageSaver();

    /** @brief 析构：兜底调用 stop()，保证语言表先于插件状态释放。 */
    ~ImageSaver();

    /** @brief 不可拷贝：配置内部有互斥锁。 */
    ImageSaver(const ImageSaver&) = delete;
    /** @brief 不可赋值。 */
    ImageSaver& operator=(const ImageSaver&) = delete;

    /**
     * @brief 挂上宿主服务并把本插件的语言表发布到 i18n_source_list。
     *
     * 本插件没有 ui_provider 的 init/deinit，create(index, host) 是唯一能拿到宿主
     * 服务表的时机，所以由 create_instance 在状态建好后调用一次。失败不致命：
     * 配置项 label/tooltip 会原样显示成 key，保存能力照常工作。
     *
     * @param[in] host 宿主服务表；老宿主可能为 nullptr（安静降级）。
     * @return true = 语言表已解析并发布；false = 宿主没有列表服务或表解析失败。
     */
    bool start(const ShotHostServices* host);

    /** @brief 释放语言表；I18n 析构会退订 lists.changed。重复调用是空操作。 */
    void stop();

    /**
     * @brief 实现 ShotImageSaverV1::save：把图像落盘并写回最终绝对路径。
     *
     * @param[in]  image     待保存图像；只读，插件不得修改像素或结构体字段，不能为 nullptr。
     * @param[in]  directory 覆盖输出目录（UTF-8）；nullptr = 用配置项 directory。
     * @param[in]  basename  覆盖文件名（不含扩展名）；nullptr = 用配置里的模板。
     * @param[out] path      接收最终绝对路径（UTF-8，NUL 结尾）；不能为 nullptr。
     * @param[in]  pathSize  path 的字节数。
     * @return 0 = 已落盘；1 = 参数非法；2 = path 缓冲不足（此时不落盘、不建目录）；
     *         3 = 保存失败（目录不可写、编码失败等）。
     */
    std::int32_t save(const ShotImage* image, const char* directory, const char* basename,
                      char* path, std::uint32_t pathSize);

    /**
     * @brief 配置项个数，转调 SaverConfig。
     * @return 配置项个数。
     */
    std::uint32_t configCount() const noexcept;

    /**
     * @brief 声明第 index 个配置项，转调 SaverConfig。
     * @param[in]  index 配置项下标。
     * @param[out] out   宿主提供并填好 struct_size 的输出结构。
     * @return 契约返回码：0 成功 / 1 参数非法 / 2 out 结构太短。
     */
    std::int32_t configDesc(std::uint32_t index, ShotConfigDesc* out) const noexcept;

    /**
     * @brief 读一个配置项，转调 SaverConfig。
     * @return >= 0 = 值长度（不含 NUL）；负值 = key 不认识或参数非法。
     */
    std::int32_t getConfig(const char* key, char* buf, std::uint32_t bufSize) const;

    /**
     * @brief 写一个配置项，转调 SaverConfig。
     * @return 0 = 已生效；负值 = key 不认识或值非法。
     */
    std::int32_t setConfig(const char* key, const char* value);

private:
    SaverConfig m_config;               /**< 四个配置项 + 线程安全读写。 */
    std::unique_ptr<shot::I18n> m_i18n; /**< 多语言：start() 构造，stop()/析构释放。 */
};

}  // namespace image_saver
