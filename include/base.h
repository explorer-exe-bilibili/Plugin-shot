#pragma once

#include <string>

/**
 * @brief 宿主核心基础设施：全局路径、配置、日志与插件实例包装。
 */
namespace core {
    /** @brief 可执行文件所在目录；末尾不带路径分隔符。 */
    extern std::string appPath;

    /**
     * @brief 用户数据目录；Windows 在 %LOCALAPPDATA% 下，Linux 在 ~ 下。
     *
     * 环境变量 `PLUGIN_SHOT_DATA_DIR` 非空时会覆盖默认位置（见 getDataPath），
     * 自检模式（`--test-plugin`）与便携模式都靠它把日志/配置/插件目录隔离出去。
     */
    extern std::string dataPath;

    /**
     * @brief 解析当前可执行文件路径并写入 appPath。
     *
     * Windows 用 GetModuleFileNameA，Linux 读 /proc/self/exe，
     * macOS 用 _NSGetExecutablePath + realpath；结果会去掉末尾的路径分隔符。
     * 解析失败时 appPath 保持空串（调用方需自行容忍）。
     */
    void getAppPath();

    /**
     * @brief 解析用户数据目录并写入 dataPath。
     *
     * 优先级：
     *   1. 环境变量 `PLUGIN_SHOT_DATA_DIR` 非空 → 直接用它的值（Windows / Linux 都生效）；
     *   2. Windows：%LOCALAPPDATA%\\plugin-shot；POSIX：$HOME/.plugin-shot。
     *
     * 环境变量覆盖是给**隔离测试与便携模式**用的：宿主自检模式在调用本函数之前把它
     * 指向临时目录，日志、配置与 `plugin-shot-test` 的痕迹就都落在那里面，不碰用户
     * 真实数据。变量为空或未设置时行为与以前完全一致。结果会去掉末尾的路径分隔符；
     * 不保证目录已存在。
     */
    void getDataPath();
}