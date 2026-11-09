#pragma once

#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>

namespace image_saver {

/**
 * @brief 把「目录 + 文件名模板 + 扩展名」解析成一个可用的绝对文件路径。
 *
 * 职责只到"算出路径"：不建目录、不落盘。save 先拿到路径、确认宿主 path 缓冲
 * 装得下，再建目录并编码——这样返回 2（缓冲不足）时不会留下任何副作用。
 *
 * 命名规则：
 *   · 模板里的时间占位符（%Y %m %d %H %M %S %%）按本地时间展开，默认模板
 *     shot_%Y%m%d_%H%M%S 每秒一个名字；不认识的占位符原样保留；
 *   · 展开结果里的路径分隔符与 Windows 非法字符替换成 '_'，避免一个配置值把
 *     文件写到目录之外（配置文件是可以被手改的，不能当可信输入）；
 *   · 同名文件已存在时依次尝试 name-1、name-2…，绝不覆盖用户已有文件。
 *
 * @note 目录为空（配置项默认值）时使用系统图片目录：Windows 取
 *       %USERPROFILE%\Pictures，POSIX 取 $HOME/Pictures；都取不到就退回当前
 *       目录，保证默认配置下第一次调用就能落盘。
 */
class OutputPath {
public:
    /**
     * @brief 构造：参数来自配置或调用方的覆盖参数，尚未做任何磁盘访问。
     * @param[in] directory 输出目录（UTF-8）；空 = 系统图片目录。
     * @param[in] basename  文件名模板（不含扩展名）；空 = "shot"。
     * @param[in] extension 扩展名（含点），由 ImageEncoder 按格式给出。
     */
    OutputPath(std::string directory, std::string basename, std::string extension);

    /**
     * @brief 解析最终绝对路径：展开模板 → 清洗非法字符 → 绝对化 → 加唯一后缀。
     * @return 绝对路径；解析失败（不存在可用名字）时返回空 path。
     */
    std::filesystem::path resolve() const;

    /**
     * @brief 创建文件所在目录（含多级）；目录已存在不算失败。
     * @param[in] filePath 已经解析好的目标文件路径。
     * @return true = 目录已就绪；false = 创建失败（调用方按保存失败处理）。
     */
    static bool ensureParentDirectory(const std::filesystem::path& filePath);

    /**
     * @brief 把 UTF-8 字符串转成标准库路径。
     *
     * C++20 起 path 有 char8_t 构造，按 UTF-8 解释字节并在 Windows 内部转宽字符；
     * 直接 path(std::string) 用的是平台本地窄编码（Windows = ANSI），中文路径会坏。
     *
     * @param[in] utf8 UTF-8 字节串，可为空。
     * @return 对应的 path；空输入返回空 path。
     */
    static std::filesystem::path fromUtf8(std::string_view utf8);

    /**
     * @brief 把标准库路径转成 UTF-8 字符串（写回宿主 path 缓冲用）。
     * @param[in] path 任意路径。
     * @return UTF-8 字节串；Windows 上分隔符是反斜杠。
     */
    static std::string toUtf8(const std::filesystem::path& path);

    /**
     * @brief 系统图片目录的 UTF-8 路径。
     * @return Windows = %USERPROFILE%\Pictures，POSIX = $HOME/Pictures；
     *         环境变量缺失时退回当前工作目录，再失败返回 "."。
     */
    static std::string defaultPicturesDirectory();

private:
    /**
     * @brief 用本地时间展开文件名模板里的时间占位符。
     *
     * 不用 std::strftime：MSVC 对非法占位符会触发 invalid parameter handler，
     * 而模板来自可被手改的配置，不能让它有机会把进程弄崩；自己实现也保证各平台
     * 展开结果一致。
     *
     * @param[in] pattern 名字模板；为空时返回空串。
     * @return 展开后的文件名（未清洗非法字符）。
     */
    static std::string expandTimestamp(const std::string& pattern);

    /**
     * @brief 取本地时间，成功返回 true。
     * @param[out] out   本地时间结构。
     * @param[in]  value 时间戳。
     * @return true = 成功；false = 系统调用失败。
     */
    static bool localTime(std::tm& out, std::time_t value) noexcept;

    /**
     * @brief 替换文件名里非法的字符，并去掉 Windows 不允许的尾随空格/点。
     * @param[in] name 展开后的文件名（不含扩展名）。
     * @return 清洗后的名字；可能为空串，由调用方回退默认名。
     */
    static std::string sanitizeBasename(const std::string& name);

    /**
     * @brief 判断是否是 Windows 保留设备名（CON / NUL / COM1 / LPT1 …）。
     * @param[in] name 已清洗的文件名（不含扩展名）。
     * @return true = 需要加前缀避让。
     */
    static bool isReservedWindowsName(const std::string& name) noexcept;

    /**
     * @brief 给 base 依次尝试 -1/-2… 后缀，直到找到一个不存在的名字。
     * @param[in] base 原始绝对路径。
     * @return 不冲突的路径；尝试次数用尽返回空 path。
     */
    static std::filesystem::path makeUnique(const std::filesystem::path& base);

    std::string m_directory;  /**< 输出目录（UTF-8）；空 = 系统图片目录。 */
    std::string m_basename;   /**< 文件名模板（不含扩展名）。 */
    std::string m_extension;  /**< 扩展名，含点。 */
};

}  // namespace image_saver
