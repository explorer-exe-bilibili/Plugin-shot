#pragma once

#include <string>

namespace core {

/**
 * @brief 单个配置项：持有字符串原值，并提供按需转换。
 *
 * 值一律以 std::string 存储（落盘也是字符串）；转换失败不抛异常，
 * 记一条 ERROR 日志后返回 0 / 0.0 / false。
 */
class ConfigItem {
    std::string value;
public:
    /**
     * @brief 用字符串原值构造配置项。
     * @param[in] value 字符串原值，可为空串。
     */
    ConfigItem(const std::string& value);

    /** @brief 构造空配置项（原值为空串）。 */
    ConfigItem();

    /**
     * @brief 取字符串原值。
     * @return 原值的 const 引用，生命周期与配置项相同。
     */
    const std::string& getOriginalValue() const;

    /** @brief 原样返回字符串值。 */
    std::string toString() const;

    /**
     * @brief 转成路径。
     *
     * 已是绝对路径（Windows 盘符或根反斜杠、POSIX 根斜杠）时原样返回，
     * 否则拼到 core::appPath 之后。
     * @return 绝对路径；不检查路径是否存在。
     */
    std::string toPath() const;

    /**
     * @brief 按十进制解析为 int。
     * @return 解析结果；不是合法整数或超出范围时记 ERROR 并返回 0。
     */
    int toInt() const;

    /**
     * @brief 按浮点解析为 double。
     * @return 解析结果；非法或超出范围时记 ERROR 并返回 0.0。
     */
    double toDouble() const;

    /**
     * @brief 解析为 bool。
     * @return 仅 "true" 与 "1" 为 true，其余一律 false（区分大小写）。
     */
    bool toBool() const;

    /**
     * @brief 覆盖字符串原值。
     * @param[in] value 新原值。
     */
    void setValue(const std::string& value);

    /** @brief 等价于 toString()。 */
    operator std::string() const;

    /** @brief 等价于 toInt()。 */
    operator int() const;

    /** @brief 等价于 toDouble()。 */
    operator double() const;

    /** @brief 等价于 toBool()。 */
    operator bool() const;
};

}  // namespace core
