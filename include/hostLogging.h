#pragma once

#include <string>

// ============================================================================
//  Host::Logging —— 宿主日志的初始化与运行期开关
//
//  日志统一走 spdlog 的异步 logger：宿主与插件各一个（"host" / "plugin"），
//  各有两个 sink（控制台 + <dataPath>/logs/host.log）。宿主设置里的
//  debug.console / logging.file / logging.level 三个核心配置项就落在这里。
//
//  运行期开关用"过滤 sink"包一层，只决定要不要 forward，绝不增删 sink：
//  异步 logger 在后台线程遍历 sink 列表，运行期改列表会直接撞上数据竞争。
// ============================================================================

namespace Host {

/**
 * @brief 宿主日志：初始化、级别与控制台/文件开关。
 *
 * 所有函数都线程安全（内部用一把状态锁 + 原子开关），可以在插件线程上被调用；
 * 例外是 init()，它只在 main 启动时调一次。
 */
namespace Logging {

/**
 * @brief 初始化 spdlog：异步线程池、host/plugin 两个 logger 与两个可开关的 sink。
 *
 * 文件日志写到 <dataPath>/logs/host.log（路径来自 core::dataPath）；目录建不出来
 * 或文件 sink 创建失败时静默降级成只打控制台，不阻止宿主启动。
 *
 * @note 只应在 main 启动时调用一次；重复调用是空操作。
 */
void init();

/**
 * @brief 设置日志级别（host 与 plugin 两个 logger 一起改）。
 * @param[in] level "debug" / "info" / "warn" / "error"，大小写不敏感。
 * @note 不认识的名字只打一条警告，保持原级别不变。
 */
void setLevel(const std::string& level);

/**
 * @brief 读当前日志级别名。
 * @return "debug" / "info" / "warn" / "error"；未初始化时为 "info"。
 */
std::string level();

/**
 * @brief 开/关控制台日志。
 * @param[in] enabled true = 输出，false = 静默。
 * @return true = 已生效；false = 控制台 sink 还没建好（宿主未初始化）。
 * @note 文件日志不受影响。
 */
bool setConsoleEnabled(bool enabled);

/**
 * @brief 开/关文件日志。
 * @param[in] enabled true = 输出，false = 静默。
 * @return true = 已生效；false = 文件 sink 没建出来（此时文件日志恒为关闭）。
 * @note 控制台日志不受影响。
 */
bool setFileEnabled(bool enabled);

/** @brief 控制台日志是否打开；sink 没建好时返回 false。 */
bool consoleEnabled();

/** @brief 文件日志是否打开；sink 没建出来时恒为 false。 */
bool fileEnabled();

}  // namespace Logging
}  // namespace Host
