#pragma once

// ============================================================================
//  include/plugin_requirement.h —— 一条插件依赖声明（metadata.json 的 requires）
//
//  形如 name[op version]：name 是提供者名字（插件 id / provides 名 / core），
//  op 是版本运算符。语法坏掉的依赖不抛异常，而是标记成 broken 并永远不满足——
//  宁可跳过这个插件，也不误加载。
// ============================================================================

#include <string>

#include "shot_semver.h"

namespace Plugin {

/**
 * @brief 一条已解析的依赖声明，形如 name[op version]。
 *
 * 例："opencv>=4.12" / "core>=0.1.0" / "acme.shaper"。
 * 语法坏掉的依赖不抛异常，而是标记成 broken 并在 satisfies() 里永远返回
 * false —— 宁可跳过这个插件，也不误加载。
 */
class PluginRequirement {
    bool m_isBroken = false;     /**< 语法坏的依赖当成"无法满足"，宁可跳过这个插件也不误加载 */
    std::string name;            /**< 被依赖的提供者名字（插件 id，或 provides 里的名字） */
    Op op = Op::Invalid;         /**< 版本运算符；只写名字时为 Op::None */
    ShotVersion version;         /**< op 非空时有效 */
    bool hasVersion = false;     /**< 是否写了版本（只写名字 = 任意版本） */
    std::string raw;             /**< 原始文本，日志与错误信息用 */

    /**
     * @brief 去掉首尾空白。
     * @param[in] str 待处理文本。
     * @return 去空白后的副本；全空白时返回空串。
     */
    static std::string trim(const std::string& str);

    /**
     * @brief 把运算符文本解析成 Op。
     * @param[in] opText ">" / ">=" / "<" / "<=" / "==" / "="。
     * @return 对应运算符；认不出来时返回 Op::Invalid（当作永不满足）。
     */
    static Op parseOp(const std::string& opText);

    /**
     * @brief 提供者名字是否合法。
     *
     * 允许字符：a-z A-Z 0-9 _ . -，且不能为空。名字里出现其它字符说明整条依赖
     * 写坏了，直接判成语法错误，比"找不到提供者"更接近真相。
     *
     * @param[in] name 待校验的名字。
     * @return true = 合法。
     */
    static bool isValidProviderName(const std::string& name);
public:
    /** @brief 默认构造：一个未初始化的依赖（satisfies 时按不满足处理）。 */
    PluginRequirement() = default;

    /**
     * @brief 从一条声明文本解析。
     * @param[in] text 形如 "opencv" / "opencv>=4.12"；空串或语法坏时 isBroken() 为 true。
     */
    PluginRequirement(const std::string& text);

    /** @brief 提供者名字（不含版本部分）。 */
    const std::string& getName() const { return name; }
    /** @brief 原始声明文本，日志与错误信息用（保留用户写法，比如 ">=" 与 "=" 的差异）。 */
    const std::string& getRaw() const { return raw; }
    /** @brief 版本运算符；Op::None 表示"任意版本"。 */
    Op getOp() const { return op; }
    /** @brief 声明的版本；hasVersion 为 false 时无意义。 */
    const ShotVersion& getVersion() const { return version; }
    /** @brief 原始文本的隐式转换，方便拼错误信息。 */
    operator std::string() const;

    /**
     * @brief 判断一个实际提供的版本是否满足这条依赖。
     * @param[in] requirementName 依赖的名字（应与 getName() 相同，不同直接 false）。
     * @param[in] have 提供者实际报告的版本（运行期事实，不是 metadata 里的声明）。
     * @return true = 满足；写法坏、名字不符、版本不够都是 false。
     */
    bool satisfies(const std::string& requirementName, const ShotVersion& have) const;
    /** @brief 声明本身是否语法坏（坏依赖永远不满足，加载时跳过整个插件）。 */
    bool isBroken() const { return m_isBroken; }
};

}  // namespace Plugin
