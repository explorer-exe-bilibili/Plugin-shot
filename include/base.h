#pragma once

#include <string>

/**
 * @brief 宿主核心基础设施：全局路径、配置、日志与插件实例包装。
 */
namespace core {
    /** @brief 可执行文件所在目录；末尾不带路径分隔符。 */
    extern std::string appPath;

    /** @brief 用户数据目录；Windows 在 %LOCALAPPDATA% 下，Linux 在 ~ 下。 */
    extern std::string dataPath;

    /**
     * @brief 解析当前可执行文件路径并写入 appPath。
     *
     * Windows 用 GetModuleFileNameA，Linux 读 /proc/self/exe；
     * 结果会去掉末尾的路径分隔符。
     */
    void getAppPath();

    /**
     * @brief 解析用户数据目录并写入 dataPath。
     *
     * 结果会去掉末尾的路径分隔符；不保证目录已存在。
     */
    void getDataPath();
}