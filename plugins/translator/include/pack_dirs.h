#pragma once

// ============================================================================
//  plugins/translator/include/pack_dirs.h —— 语言包目录解析（唯一的平台相关代码）
//
//  默认两个目录：
//    · 官方：<exe 目录>/files/lang   —— 随安装包分发，priority 200；
//    · 用户：%LOCALAPPDATA%\plugin-shot\langs —— 升级不覆盖，priority 300。
//  exe 目录在 ABI 里没有暴露（数据目录不在契约里），所以 Windows 用
//  GetModuleFileNameW(nullptr) 取，Linux/macOS 走 /proc/self/exe / dyld。
//
//  配置项 translator.main/pack_dirs 非空时只用它（分号分隔多个目录，priority 300）：
//  "填了就别再猜" —— 用户显式指定目录时混入默认目录会让"这个包到底从哪来"变糊。
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace translator {

/** @brief 一个语言包目录：路径 + 该目录贡献的优先级 + 中文来源名（日志用）。 */
struct PackLocation {
    std::filesystem::path path; /**< 目录路径；不存在不是错误（扫描时安静跳过） */
    std::int32_t priority = 0;  /**< 该目录里包的优先级：官方 200、用户/配置 300 */
    std::string origin;         /**< "官方" / "用户" / "配置" */
};

/**
 * @brief 语言包目录解析器：把配置值翻译成"按顺序扫描哪些目录"。
 *
 * 只做路径计算，不访问文件内容（是否存在由 PackScanner 判断），所以可以单独
 * 验证；平台差异全部收敛在 executableDirectory()/defaultUserDirectory()。
 */
class PackDirs {
public:
    /**
     * @brief 按配置解析出要扫描的目录列表。
     * @param[in] configured 配置项 pack_dirs 的值；空串 = 使用官方 + 用户两个默认目录。
     * @return 目录列表（官方在前、用户在后）；路径无法解析时可能为空。
     */
    static std::vector<PackLocation> resolve(const std::string& configured);

    /**
     * @brief 宿主 exe 所在目录。
     * @return 目录；平台 API 失败时返回空路径（调用方据此跳过官方目录）。
     */
    static std::filesystem::path executableDirectory();

    /**
     * @brief 用户语言包目录（默认 %LOCALAPPDATA%\plugin-shot\langs）。
     * @return 目录；取不到用户目录时返回空路径。
     */
    static std::filesystem::path defaultUserDirectory();

private:
    /**
     * @brief 当前可执行文件的完整路径。
     * @return 路径；任何平台 API 失败时返回空路径。
     */
    static std::filesystem::path executablePath();

    /**
     * @brief 把 "a;b;c" 拆成去空白的非空项。
     * @param[in] value 配置值；空串返回空列表。
     * @return 拆分结果，保持书写顺序。
     */
    static std::vector<std::string> splitList(const std::string& value);
};

/**
 * @brief 把路径转成 UTF-8 文本（日志与配置值用）。
 *
 * Windows 的 path 内部是宽字符，直接经窄串构造会按 ANSI 解释、中文目录变乱码；
 * 统一走 C++20 的 char8_t 通道。路径→文本的转换只在这里实现一次。
 *
 * @param[in] path 任意路径。
 * @return UTF-8 文本；空路径返回空串。
 */
std::string pathToUtf8(const std::filesystem::path& path);

}  // namespace translator
