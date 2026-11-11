#pragma once

// ============================================================================
//  plugins/translator/include/pack_scanner.h —— 目录扫描：位置列表 → 解析后的包
//
//  只做"找文件 + 按文件名排序 + 交给 PackLoader"，不解释包内容。顺序很重要：
//  同一 priority 下后加载的包覆盖先加载的（与宿主"同优先级后到者胜"一致），
//  所以目录内按文件名排序，保证同一批文件每次扫描结果都一样。
// ============================================================================

#include <filesystem>
#include <vector>

#include "pack_dirs.h"
#include "pack_loader.h"
#include "pack_types.h"
#include "plugin_api.h"

namespace translator {

/**
 * @brief 语言包扫描器：遍历若干目录，返回可用的包（坏包已 WARN 并跳过）。
 *
 * 非标准库操作只有"枚举目录"这一件，std::filesystem 在各平台都可用；
 * 因此除路径解析（PackDirs）外本插件没有平台分支，Linux/macOS 同样能编译。
 */
class PackScanner {
public:
    /**
     * @brief 绑定宿主服务表（只用于日志）。
     * @param[in] host 宿主服务表；可为 nullptr。
     */
    explicit PackScanner(const ShotHostServices* host);

    /**
     * @brief 扫描全部目录。
     *
     * 目录不存在不算错误（用户语言包目录在没装过包时本来就不存在），只记一条
     * INFO 方便排障；目录不可读才 WARN。
     *
     * @param[in] locations 要扫描的目录列表（顺序即优先级顺序）。
     * @return 解析成功的包，按扫描顺序排列；每一个都带 priority 与全局 order。
     */
    std::vector<ParsedPack> scan(const std::vector<PackLocation>& locations) const;

private:
    /**
     * @brief 列出目录里的 *.json（扩展名大小写不敏感），按文件名排序。
     * @param[in] location 目录。
     * @return 文件路径列表；目录不可读时为空（已 WARN）。
     */
    std::vector<std::filesystem::path> listFiles(const PackLocation& location) const;

    const ShotHostServices* m_host = nullptr; /**< 只用于 shot::Logger，可为 nullptr */
    PackLoader m_loader;                      /**< 单文件解析；构造时把宿主指针转发给它 */
};

}  // namespace translator
