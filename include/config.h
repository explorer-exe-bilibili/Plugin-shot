#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "configItem.h"

namespace core {

/**
 * @brief 进程级配置单例：JSON 文件读写 + 内存键值表。
 *
 * 主实例与"默认配置"实例分开：主实例缺失的 key 会回落到默认实例。
 * 读写都串行化在 rwMutex 上。
 */
class Config {
    static std::string ConfigFilePath;
    bool isDefault = false;
    void readConfigFile();
    void setDefaultConfig();
    Config(bool isDefault = false);
    std::unordered_map<std::string, ConfigItem> configItems;
    mutable std::mutex rwMutex;
public:
    /**
     * @brief 取配置单例。
     * @param[in] isDefault true = 取"默认配置"实例（由 setDefaultConfig 填充）；
     *            false = 取主实例。
     * @return 单例引用，进程生命周期内有效。
     */
    static Config& getInstance(bool isDefault = false);

    /**
     * @brief 指定配置文件路径并加载。
     *
     * 先 readConfigFile() 再 setDefaultConfig()；文件不存在或解析失败只记日志。
     * @param[in] configFilePath JSON 配置文件路径。
     */
    void init(const std::string& configFilePath);

    /**
     * @brief 读一个配置项。
     *
     * 主实例找不到 key 时回落到默认实例；两边都没有时记 ERROR 并返回空键项。
     * @param[in] key 配置键名。
     * @return 配置项引用；指向内部表，其它线程 set 新键后可能失效。
     * @note 持锁读取。
     */
    ConfigItem& get(const std::string& key);

    /**
     * @brief 判断主实例里是否存在某个键。
     *
     * 与 get() 的区别：**不**回落到默认配置实例，也不打日志——它回答的是
     * "配置文件里有没有存过这个键"，用于决定"用已存值还是用默认值"。
     * @param[in] key 配置键名。
     * @return true = 主实例里有这个键（值可能是空串）；false = 没有。
     * @note 持锁读取。
     */
    bool contains(const std::string& key) const;

    /**
     * @brief 写入/覆盖一个配置项（只改内存，不落盘）。
     * @param[in] key 配置键名。
     * @param[in] value 配置项值。
     */
    void set(const std::string& key, const ConfigItem& value);

    /**
     * @brief 仅在 key 不存在时写入。
     *
     * 主实例调用时同时写入默认实例，保证两边的默认值一致。
     * @param[in] key 配置键名。
     * @param[in] value 配置项值。
     * @return true = 原先不存在且已写入；false = 已存在、未修改。
     * @note 判断存在性时不持锁，只适合初始化阶段单线程调用。
     */
    bool setifno(const std::string& key, const ConfigItem& value);

    /**
     * @brief 把当前全部配置项写成 JSON 文件。
     *
     * 写入 version、fullVersion、appName 与 configItems 列表；文件打不开或
     * 写入失败只记日志、不抛异常，调用方以返回值判断是否真的落盘。
     * @return true = 已写完；false = 文件打不开或写入失败。
     */
    bool saveConfigFile();
};

}  // namespace core
