#pragma once

// ============================================================================
//  plugins/translator/include/pack_types.h —— 解析后的语言包（纯数据，跨 TU 共享）
//
//  语言包的解析结果同时被三处使用：PackLoader 产出它、PackScanner 记录扫描顺序、
//  SourcePublisher 按 priority 合并它。所以单独放一个头，避免三个类互相包含。
//
//  包里只有"文本"和"元数据"，没有任何宿主服务依赖：解析阶段不判断 owner 是否
//  已加载（那是发布阶段的策略，见 SourcePublisher），这样解析器可以在纯单测/离线
//  工具里复用。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace translator {

/**
 * @brief 一个成功解析的语言包：语言元数据 + owner→key→文本。
 *
 * @note 文本里的空串在解析阶段已剔除（空串视为"这条还没翻"，让它走回退链，
 *       而不是在界面上留一块空白），所以 `texts` 里只有非空文本。
 */
struct ParsedPack {
    std::string path;            /**< 包文件路径（UTF-8，日志/排障用） */
    std::string tag;             /**< 规范 BCP-47 语言标签（来自 @meta.tag） */
    std::string name;            /**< 语言显示名（@meta.name；缺省 = tag） */
    std::string fallback;        /**< @meta.fallback（规范；空串 = 用宿主默认回退） */
    std::string origin;          /**< 来源说明："官方" / "用户" / "配置"（日志用） */
    std::int32_t priority = 0;   /**< 200 = 官方包，300 = 用户包/配置目录 */
    std::size_t order = 0;       /**< 全局扫描顺序：同 priority 时 order 大者胜（与宿主规则一致） */
    std::map<std::string, std::map<std::string, std::string>> texts; /**< owner → key → 文本 */
};

/**
 * @brief 包内全部文本条数（所有 owner 合计）。
 * @param[in] pack 已解析的包。
 * @return 文本条数；空包返回 0。
 */
inline std::size_t textCount(const ParsedPack& pack) {
    std::size_t count = 0;
    for (const auto& owner : pack.texts) {
        count += owner.second.size();
    }
    return count;
}

}  // namespace translator
