#pragma once

// ============================================================================
//  plugins/translator/include/pack_loader.h —— 单个语言包文件的解析与校验
//
//  包格式（files/lang/README.md 与 document/I18N.md）：
//    {
//      "@meta": {"tag":"ja-JP","name":"日本語","fallback":"en-US"},   // 也可写成 "ja-JP"
//      "texts": {"core": {"settings.open":"設定"},
//                "hotkey.main": {"config.capture_hotkey.label":"撮影ホットキー"}}
//    }
//
//  校验原则：坏包只 WARN + 跳过，绝不让插件加载失败、更不打印 ERROR。
//  语言包是用户可手改的输入，把它当不可信数据：字段类型逐层检查，能做主的
//  部分（有 texts 的包）照常加载，做不了主的部分静默剔除。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "pack_types.h"
#include "plugin_api.h"

namespace translator {

/**
 * @brief 语言包解析器：一个文件进、一个 ParsedPack 出。
 *
 * 每个包最多读 4 MiB（kMaxPackBytes）：语言包是纯文本，正常几 KB 到几十 KB，
 * 再大只可能是放错了文件；不设上限会把"误放一个视频到 lang 目录"变成卡顿。
 *
 * @note 不判断 texts 里的 owner 是否是已加载插件——那是发布阶段的策略
 *       （SourcePublisher），解析器保持纯函数式，方便离线复用与单测。
 */
class PackLoader {
public:
    /**
     * @brief 绑定宿主服务表（只用于写 WARN 日志）。
     * @param[in] host 宿主服务表；可为 nullptr（日志安静丢弃）。
     */
    explicit PackLoader(const ShotHostServices* host) noexcept;

    /**
     * @brief 读取并解析一个语言包文件。
     *
     * 失败一律已经记过 WARN（内容/原因都在日志里），调用方直接跳过这个文件即可。
     * 文件名与 @meta.tag 不一致时记 WARN，但**以 @meta.tag 为准**：用户重命名
     * 文件很常见，而文本只有按 tag 才能进对语言。
     *
     * @param[in]  file     包文件路径。
     * @param[in]  priority 该目录对应的优先级（官方 200 / 用户 300）。
     * @param[in]  origin   来源说明（"官方" / "用户" / "配置"，写进日志）。
     * @param[in]  order    全局扫描顺序，同 priority 时 order 大者胜。
     * @param[out] out      成功时填入；失败时内容未定义（调用方不要用）。
     * @return true = 包可用；false = 坏包/不可读/没有可用文案（已 WARN）。
     */
    bool load(const std::filesystem::path& file, std::int32_t priority,
              const std::string& origin, std::size_t order, ParsedPack& out) const;

private:
    /**
     * @brief 读整个文件到内存（二进制模式，保留 BOM 供上层剥离）。
     * @param[in]  file 文件路径。
     * @param[out] out  成功时是文件字节。
     * @return true = 读取成功且大小在限制内；false = 已 WARN。
     */
    bool readFile(const std::filesystem::path& file, std::string& out) const;

    /**
     * @brief 把语言标签规范成 BCP-47 写法（语言小写、地区大写、下划线换短横）。
     *
     * 与宿主 Host::I18n::normalizeTag 及 helper 同一套规则：不统一的话
     * "zh_CN" 与 "zh-CN" 会被当成两种语言，用户写错一个分隔符就整包失效。
     *
     * @param[in] tag 任意写法。
     * @return 规范标签；空输入或没有有效片段时返回空串。
     */
    static std::string normalizeLanguageTag(std::string_view tag);

    const ShotHostServices* m_host = nullptr; /**< 只用于 shot::Logger，可为 nullptr */
};

}  // namespace translator
