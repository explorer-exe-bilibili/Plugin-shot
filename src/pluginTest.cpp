#include "pluginTest.h"

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "base.h"
#include "config.h"
#include "hostI18n.h"
#include "hostLogging.h"
#include "hostRuntime.h"
#include "plugin_meta.h"
#include "version.h"

// ============================================================================
//  pluginTest.cpp —— `--test-plugin` 自检模式的编排
//
//  这个文件只负责"把真实宿主装起来"和"把结果说清楚"：
//    1. 解析参数、建隔离数据目录并设置 PLUGIN_SHOT_DATA_DIR（必须在 getDataPath 之前）；
//    2. 把被测 DLL 与同目录里能解析到的依赖提供者拷进 <dataDir>/plugins；
//    3. 初始化真实的日志 / 配置 / HostRuntime（PluginManager 扫描→校验→依赖→加载）；
//    4. 调 pluginTestChecks.cpp 做逐插件检查；
//    5. 输出人类摘要 / JSON，最后真实卸载（HostRuntime 析构）并清理临时目录。
//
//  两个容易踩的点：
//    · 日志是异步的、文件 sink 被 Host::Logging 的静态引用持有：删除数据目录
//      之前必须先 Host::Logging::shutdown()（释放 sink），否则 Windows 上文件句柄
//      占用会导致删不干净；spdlog::shutdown() 本身不够；
//    · 插件挂起在进程内没有安全的恢复手段（不能安全地 CancelSynchronousIo 任意
//      DLL 代码），所以 --timeout-ms 用看门狗线程 + std::_Exit(2) 兜底。
// ============================================================================

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// 这些类型/工具都定义在 Host 命名空间里（pluginTest.h）；本文件的辅助函数在全局
// 匿名命名空间，用 using 让签名保持可读（不要改写成 Host::PluginTestCheck 到处飞）。
using Host::addPluginTestCheck;
using Host::countPluginTestChecks;
using Host::PluginTestCheck;
using Host::PluginTestCheckCounts;
using Host::PluginTestOptions;
using Host::PluginTestPluginReport;
using Host::PluginTestReport;
using Host::PluginTestStatus;
using Host::pluginTestStatusName;

constexpr char kTestRootDirName[] = "plugin-shot-test"; /**< 系统临时目录下的自检根目录。 */
constexpr char kDataDirEnvName[] = "PLUGIN_SHOT_DATA_DIR"; /**< 隔离数据目录环境变量。 */
constexpr int kDefaultTimeoutMs = 30000; /**< 默认整体超时。 */
constexpr int kMaxTimeoutMs = 600000;    /**< --timeout-ms 上限，防止手滑写个天文数字。 */
constexpr std::size_t kLogTailLines = 60; /**< 失败时从 host.log 里回显的行数。 */
constexpr std::size_t kLogTailMaxLines = 500; /**< 读日志时最多记住的行数。 */

// ---- 看门狗：整体超时后硬退出 ------------------------------------------------
static std::mutex watchdogMutex;                 /**< 保护下面三个状态。 */
static std::condition_variable watchdogCondition;/**< 停止信号 / 超时等待。 */
static bool watchdogStopRequested = false;       /**< 主流程是否已经走完。 */
static std::thread watchdogThread;               /**< 看门狗线程；正常结束时 join。 */

/**
 * @brief 看门狗线程体：等到超时就把"插件挂起"的事实写到 stderr 并硬退出。
 *
 * 为什么硬退出：宿主没有"打断插件调用"的合法手段，继续等只会让 CI/skill 挂死。
 * 退出码与用法/IO 错误一致（2），并打印明确原因，避免被误读成插件检查失败。
 * @param[in] timeoutMs 整体超时毫秒数。
 */
void watchdogLoop(int timeoutMs) {
    std::unique_lock<std::mutex> lock(watchdogMutex);
    const bool stopped = watchdogCondition.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [] { return watchdogStopRequested; });
    if (stopped) {
        return;
    }
    lock.unlock();
    // _Exit 绕过析构与 atexit：插件可能正卡在任意 DLL 代码里，不能指望正常收尾。
    // TODO: 硬退出来不及清理隔离目录、也写不出 JSON；要做"可恢复的取消 + 事故现场保留"
    //       需要独立子进程 + 超时 kill（本期刻意保持简单，排错用 --keep-data 手动排查）。
    std::fputs("[plugin-test] 超时：插件调用或自检流程在 --timeout-ms 内没有完成，"
               "强制退出（exit 2）\n", stderr);
    std::fflush(stderr);
    std::_Exit(2);
}

/** @brief 启动看门狗（每个进程只调一次）。 */
void startWatchdog(int timeoutMs) {
    std::lock_guard<std::mutex> lock(watchdogMutex);
    watchdogStopRequested = false;
    watchdogThread = std::thread(watchdogLoop, timeoutMs);
}

/** @brief 停止看门狗并等待线程退出；没启动过时是空操作。 */
void stopWatchdog() {
    {
        std::lock_guard<std::mutex> lock(watchdogMutex);
        watchdogStopRequested = true;
    }
    watchdogCondition.notify_all();
    if (watchdogThread.joinable()) {
        watchdogThread.join();
    }
}

// ---- 命令行 -----------------------------------------------------------------

/** @brief 测试模式用法（错误走 stderr，--help 走 stdout）。 */
void printTestUsage(std::ostream& out) {
    out << "用法: Plugin-shot --test-plugin <dll> [选项]\n"
        << "选项:\n"
        << "  --test-plugin <dll>   被测插件 DLL/SO（必需；也可写 --test-plugin=<dll>）\n"
        << "  --json                把结构化 JSON 输出到 stdout（人类摘要改写进日志）\n"
        << "  --json-out <file>     把结构化 JSON 写到文件（可与 --json 同时用）\n"
        << "  --keep-data           保留隔离数据目录，便于排错（默认结束时删除）\n"
        << "  --timeout-ms <n>      整体超时毫秒数，默认 " << kDefaultTimeoutMs
        << "，上限 " << kMaxTimeoutMs << "\n"
        << "  --language <tag>      i18n 快照使用的语言标签，默认 en-US\n"
        << "  --verbose             人类摘要里同时打印 PASS/SKIP 明细\n"
        << "退出码: 0=无失败 1=至少一条失败 2=用法/IO 错误或超时\n";
}

/** @brief 参数解析结果。 */
enum class ParseOutcome {
    Ok,   /**< 解析成功，可以继续。 */
    Help, /**< 用户要 --help：打印用法后退出 0。 */
    Error /**< 参数错误：打印原因 + 用法后退出 2。 */
};

/**
 * @brief 从字符串解析一个正整数。
 * @param[in]  text 输入文本（必须整体是数字）。
 * @param[out] value 解析结果。
 * @return true = 合法。
 */
bool parsePositiveInt(const std::string& text, int& value) {
    if (text.empty()) {
        return false;
    }
    int parsed = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed <= 0) {
        return false;
    }
    value = parsed;
    return true;
}

/**
 * @brief 解析自检模式的全部参数。
 * @param[in]  argc    参数个数。
 * @param[in]  argv    参数数组。
 * @param[out] options 解析结果。
 * @param[out] error   失败原因。
 * @return 解析结果枚举；Error 时 error 有可读原因。
 */
ParseOutcome parseOptions(int argc, char** argv, PluginTestOptions& options, std::string& error) {
    bool sawTestPlugin = false;
    const auto valueAfter = [&](const std::string& arg, const char* prefix) -> std::string {
        // "--name=value" 形式：返回等号后面的内容（允许空，由调用方判错）。
        const std::string name = prefix;
        if (arg.rfind(name + "=", 0) == 0) {
            return arg.substr(name.size() + 1);
        }
        return std::string();
    };
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            return ParseOutcome::Help;
        }
        if (arg == "--test-plugin" || arg.rfind("--test-plugin=", 0) == 0) {
            if (sawTestPlugin) {
                error = "--test-plugin 只能给一次";
                return ParseOutcome::Error;
            }
            sawTestPlugin = true;
            if (arg == "--test-plugin") {
                if (i + 1 >= argc) {
                    error = "--test-plugin 缺少 DLL 路径";
                    return ParseOutcome::Error;
                }
                // `--test-plugin --help` 里的 --help 不是 DLL 路径：用户显然想看用法。
                // 不特殊处理会得到 "DLL 不存在: ...\--help" 这种莫名其妙的报错（实测）。
                if (std::string(argv[i + 1]) == "--help" || std::string(argv[i + 1]) == "-h") {
                    return ParseOutcome::Help;
                }
                options.dllPath = argv[++i];
            } else {
                options.dllPath = valueAfter(arg, "--test-plugin");
            }
            if (options.dllPath.empty()) {
                error = "--test-plugin 的 DLL 路径为空";
                return ParseOutcome::Error;
            }
        } else if (arg == "--json") {
            options.jsonStdout = true;
        } else if (arg == "--json-out" || arg.rfind("--json-out=", 0) == 0) {
            if (arg == "--json-out") {
                if (i + 1 >= argc) {
                    error = "--json-out 缺少文件路径";
                    return ParseOutcome::Error;
                }
                options.jsonOutPath = argv[++i];
            } else {
                options.jsonOutPath = valueAfter(arg, "--json-out");
            }
            if (options.jsonOutPath.empty()) {
                error = "--json-out 的文件路径为空";
                return ParseOutcome::Error;
            }
        } else if (arg == "--keep-data") {
            options.keepData = true;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--timeout-ms" || arg.rfind("--timeout-ms=", 0) == 0) {
            std::string text;
            if (arg == "--timeout-ms") {
                if (i + 1 >= argc) {
                    error = "--timeout-ms 缺少数值";
                    return ParseOutcome::Error;
                }
                text = argv[++i];
            } else {
                text = valueAfter(arg, "--timeout-ms");
            }
            int timeoutMs = 0;
            if (!parsePositiveInt(text, timeoutMs) || timeoutMs > kMaxTimeoutMs) {
                error = "--timeout-ms 需要 1.." + std::to_string(kMaxTimeoutMs) + " 的整数，收到: " + text;
                return ParseOutcome::Error;
            }
            options.timeoutMs = timeoutMs;
        } else if (arg == "--language" || arg.rfind("--language=", 0) == 0) {
            std::string text;
            if (arg == "--language") {
                if (i + 1 >= argc) {
                    error = "--language 缺少语言标签";
                    return ParseOutcome::Error;
                }
                text = argv[++i];
            } else {
                text = valueAfter(arg, "--language");
            }
            if (text.empty()) {
                error = "--language 的语言标签为空";
                return ParseOutcome::Error;
            }
            options.language = text;
        } else if (arg == "--capture" || arg == "--settings") {
            // main 已经做了一次互斥检查；这里兜底覆盖 "--test-plugin x --capture" 的顺序。
            error = "自检模式不能与 --capture / --settings 同时使用";
            return ParseOutcome::Error;
        } else {
            error = "未知参数: " + arg;
            return ParseOutcome::Error;
        }
    }
    if (!sawTestPlugin) {
        error = "缺少 --test-plugin <dll>";
        return ParseOutcome::Error;
    }
    return ParseOutcome::Ok;
}

// ---- 路径与文件 --------------------------------------------------------------

/** @brief 当前进程 id（隔离目录名的一部分）。 */
std::string processIdText() {
#ifdef _WIN32
    return std::to_string(static_cast<long long>(_getpid()));
#else
    return std::to_string(static_cast<long long>(getpid()));
#endif
}

/** @brief 当前 Unix 毫秒时间戳（隔离目录名的一部分）。 */
std::string epochMillisText() {
    using namespace std::chrono;
    return std::to_string(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

/**
 * @brief 把路径归一化成可比较的字符串（不要求文件存在）。
 * @param[in] path 目标路径。
 * @return 绝对且尽量规范化的路径文本；解析失败时退回词法规范化结果。
 */
std::string pathKey(const fs::path& path) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    if (!ec) {
        return canonical.string();
    }
    return fs::absolute(path, ec).lexically_normal().string();
}

/**
 * @brief 判断两个路径是否指向同一个文件。
 * @param[in] left  路径一。
 * @param[in] right 路径二。
 * @return true = 同一个文件（任一不存在时退回字符串比较）。
 */
bool sameFile(const std::string& left, const std::string& right) {
    std::error_code ec;
    const bool equivalent = fs::equivalent(left, right, ec);
    if (!ec) {
        return equivalent;
    }
    return pathKey(fs::path(left)) == pathKey(fs::path(right));
}

/** @brief 路径是否是当前平台的插件动态库（按扩展名判断）。 */
bool isPluginFileName(const fs::path& path) {
    const std::string extension = path.extension().string();
#ifdef _WIN32
    return extension == ".dll";
#else
    return extension == ".so" || extension == ".dylib";
#endif
}

/**
 * @brief 解析被测 DLL 路径：转绝对路径并校验存在性。
 * @param[in]  argument 命令行给的路径（相对/绝对均可）。
 * @param[out] out      解析后的绝对路径。
 * @param[out] error    失败原因。
 * @return true = 可用。
 */
bool resolveSourceDll(const std::string& argument, fs::path& out, std::string& error) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(fs::path(argument), ec);
    if (ec) {
        error = "无法解析 DLL 路径: " + argument + "（" + ec.message() + "）";
        return false;
    }
    const fs::path normalized = absolute.lexically_normal();
    if (!fs::exists(normalized, ec) || ec) {
        error = "DLL 不存在: " + normalized.string();
        return false;
    }
    if (!fs::is_regular_file(normalized, ec) || ec) {
        error = "不是普通文件: " + normalized.string();
        return false;
    }
    if (!isPluginFileName(normalized)) {
        error = "扩展名不是本平台的插件动态库（Windows 需要 .dll，Linux 需要 .so/.dylib）: " +
                normalized.string();
        return false;
    }
    out = normalized;
    return true;
}

/**
 * @brief 建隔离数据目录：<系统临时目录>/plugin-shot-test/<pid>-<毫秒>。
 * @param[out] dataDir 创建成功的目录。
 * @param[out] error   失败原因。
 * @return true = 已创建（含 plugins 子目录）。
 */
bool createIsolatedDataDir(fs::path& dataDir, std::string& error) {
    std::error_code ec;
    const fs::path tempRoot = fs::temp_directory_path(ec);
    if (ec) {
        error = "无法取得系统临时目录: " + ec.message();
        return false;
    }
    const fs::path dir = tempRoot / kTestRootDirName / (processIdText() + "-" + epochMillisText());
    fs::create_directories(dir / "plugins", ec);
    if (ec) {
        error = "无法创建隔离数据目录 " + dir.string() + ": " + ec.message();
        return false;
    }
    dataDir = dir;
    return true;
}

/**
 * @brief 设置 PLUGIN_SHOT_DATA_DIR，让 core::getDataPath() 指向隔离目录。
 *
 * 必须在 getDataPath() 之前调用；Windows 用 _putenv_s、POSIX 用 setenv。
 * @param[in]  dataDir 隔离数据目录。
 * @param[out] error   失败原因。
 * @return true = 已设置。
 */
bool setDataDirEnvironment(const fs::path& dataDir, std::string& error) {
#ifdef _WIN32
    const errno_t rc = _putenv_s(kDataDirEnvName, dataDir.string().c_str());
    if (rc != 0) {
        error = std::string("无法设置环境变量 ") + kDataDirEnvName;
        return false;
    }
#else
    if (setenv(kDataDirEnvName, dataDir.string().c_str(), 1) != 0) {
        error = std::string("无法设置环境变量 ") + kDataDirEnvName;
        return false;
    }
#endif
    return true;
}

/**
 * @brief 拷贝一个文件到目标（覆盖同名文件）。
 * @param[in]  source      源文件。
 * @param[in]  destination 目标文件。
 * @param[out] error       失败原因。
 * @return true = 已拷贝。
 */
bool copyOneFile(const fs::path& source, const fs::path& destination, std::string& error) {
    std::error_code ec;
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "拷贝 " + source.string() + " -> " + destination.string() + " 失败: " + ec.message();
        return false;
    }
    return true;
}

/** @brief 一并拷进隔离目录的依赖提供者。 */
struct DependencyCopy {
    std::vector<std::string> copied;  /**< 实际拷贝的依赖文件名（不含被测 DLL 本身）。 */
    std::vector<std::string> missing; /**< metadata 声明了、但在源目录找不到的依赖。 */
};

/**
 * @brief 把被测 DLL 与它的依赖提供者拷进隔离插件目录。
 *
 * 为什么还要拷依赖：宿主的依赖校验是真的（metadata.requires），像
 * example_processor 需要 opencv 提供者；只拷被测 DLL 会让"插件本身没问题"的包
 * 因为缺依赖被跳过。做法很保守：只扫被测 DLL 所在目录，按 metadata 的 id /
 * provides 找本机的其它插件包，能找多少拷多少；找不到的留给宿主按缺少依赖失败，
 * 报告里也会列出（这是"部署环境不完整"，不是插件代码缺陷）。
 *
 * @param[in]  sourceDll       被测 DLL 的绝对路径。
 * @param[in]  targetMeta      被测 DLL 的 metadata。
 * @param[in]  metadataAvailable metadata 是否读成功（失败时无法解析依赖）。
 * @param[in]  pluginsDir      隔离插件目录。
 * @param[out] result          拷贝/缺失清单。
 * @param[out] error           失败原因。
 * @return true = 被测 DLL 及找到的依赖都已拷贝。
 */
bool copyDllWithDependencies(const fs::path& sourceDll,
                             const Plugin::PluginPackageMeta& targetMeta, bool metadataAvailable,
                             const fs::path& pluginsDir, DependencyCopy& result, std::string& error) {
    std::error_code ec;
    fs::create_directories(pluginsDir, ec);
    if (ec) {
        error = "无法创建隔离插件目录 " + pluginsDir.string() + ": " + ec.message();
        return false;
    }
    if (!copyOneFile(sourceDll, pluginsDir / sourceDll.filename(), error)) {
        return false;
    }
    if (!metadataAvailable) {
        return true;  // 没有声明可解析：只拷 DLL 本身，宿主会按无元数据包处理。
    }

    // 建"提供者名字 -> DLL"索引。只扫被测 DLL 所在目录：插件部署时依赖提供者与
    // 依赖者通常同目录（构建产物 bin/plugins 就是这样），不递归、不去 PATH 里猜。
    // TODO: 插件把提供者放在别的目录时这里会找不到；需要时应增加 --plugin-dir 之类的
    //       显式参数，而不是偷偷扩大搜索范围（会引入"测的到底是谁"的歧义）。
    struct ProviderCandidate {
        fs::path path; /**< 提供者 DLL 路径。 */
    };
    std::map<std::string, ProviderCandidate> providers;
    const fs::path sourceDir = sourceDll.parent_path();
    for (const auto& entry : fs::directory_iterator(sourceDir, ec)) {
        if (ec) {
            break;
        }
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc) {
            continue;
        }
        if (!isPluginFileName(entry.path())) {
            continue;
        }
        if (sameFile(entry.path().string(), sourceDll.string())) {
            continue;
        }
        Plugin::PluginPackageMeta package;
        std::string ignoredError;
        if (!package.readFromFile(entry.path().string(), ignoredError)) {
            continue;  // 没有 metadata 的包无法当提供者解析，跳过。
        }
        for (const Plugin::PluginMetaEntry& item : package.getMetaData()) {
            providers.emplace(item.id, ProviderCandidate{entry.path()});
            for (const std::string& provided : item.provides) {
                providers.emplace(provided, ProviderCandidate{entry.path()});
            }
        }
    }

    std::set<std::string> visited;
    visited.insert(pathKey(sourceDll));
    std::set<std::string> missing;
    std::vector<fs::path> toCopy;
    // 递归解析依赖（深度通常 <= 2）；visited 用路径归一化结果防同一包被拷两次。
    std::function<void(const Plugin::PluginPackageMeta&)> collect =
        [&](const Plugin::PluginPackageMeta& meta) {
            for (const Plugin::PluginMetaEntry& item : meta.getMetaData()) {
                for (const Plugin::PluginRequirement& requirement : item.requireList) {
                    if (requirement.getName() == "core") {
                        continue;  // core 是宿主自身，不需要拷贝。
                    }
                    const auto provider = providers.find(requirement.getName());
                    if (provider == providers.end()) {
                        if (!requirement.isBroken()) {
                            missing.insert(requirement.getRaw());
                        }
                        continue;
                    }
                    const std::string key = pathKey(provider->second.path);
                    if (!visited.insert(key).second) {
                        continue;
                    }
                    Plugin::PluginPackageMeta providerMeta;
                    std::string ignoredError;
                    const bool providerMetaAvailable =
                        providerMeta.readFromFile(provider->second.path.string(), ignoredError);
                    toCopy.push_back(provider->second.path);
                    if (providerMetaAvailable) {
                        collect(providerMeta);
                    }
                }
            }
        };
    collect(targetMeta);

    for (const fs::path& dependency : toCopy) {
        const fs::path destination = pluginsDir / dependency.filename();
        std::error_code existsEc;
        if (fs::exists(destination, existsEc) && !existsEc) {
            continue;
        }
        if (!copyOneFile(dependency, destination, error)) {
            return false;
        }
        result.copied.push_back(dependency.filename().string());
    }
    result.missing.assign(missing.begin(), missing.end());
    return true;
}

/**
 * @brief 往 --json-out 写文本，父目录不存在时自动创建。
 * @param[in]  path    目标文件。
 * @param[in]  content 内容（UTF-8）。
 * @param[out] error   失败原因。
 * @return true = 已写完。
 */
bool writeTextFile(const fs::path& path, const std::string& content, std::string& error) {
    std::error_code ec;
    if (path.has_parent_path() && !path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "无法创建输出目录 " + path.parent_path().string() + ": " + ec.message();
            return false;
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "无法打开文件 " + path.string();
        return false;
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();
    if (!stream) {
        error = "写入文件失败 " + path.string();
        return false;
    }
    return true;
}

/**
 * @brief 失败时把隔离目录里的 host.log 末尾回显到 stderr。
 *
 * 日志在 Host::Logging::shutdown() 之后才读（sink 已释放、文件已关闭）。仍保留
 * 一个短暂的稳定性等待：异步落盘线程与调用方曾有竞态（刚 shutdown 时文件可能
 * 还没写完），最多等 2 秒，避免把"日志为空"误当成没有日志。
 * @param[in] dataDir 隔离数据目录。
 */
void dumpLogTail(const fs::path& dataDir) {
    const fs::path logPath = dataDir / "logs" / "host.log";
    std::uintmax_t lastSize = 0;
    int stableRounds = 0;
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(logPath, ec);
        if (!ec && size > 0 && size == lastSize) {
            if (++stableRounds >= 2) {
                break;
            }
        } else {
            stableRounds = 0;
        }
        lastSize = ec ? 0 : size;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::ifstream stream(logPath, std::ios::binary);
    if (!stream) {
        std::cerr << "[plugin-test] 没有找到日志文件: " << logPath.string() << "\n";
        return;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
        if (lines.size() > kLogTailMaxLines) {
            lines.erase(lines.begin());
        }
    }
    const std::size_t begin = lines.size() > kLogTailLines ? lines.size() - kLogTailLines : 0;
    std::cerr << "---- host.log 末尾 " << (lines.size() - begin) << " 行（" << logPath.string()
              << "）----\n";
    for (std::size_t i = begin; i < lines.size(); ++i) {
        std::cerr << lines[i] << "\n";
    }
    std::cerr << "---- host.log 结束 ----\n";
}

/** @brief 把字符串列表拼成一个逗号分隔的句子。 */
std::string joinText(const std::vector<std::string>& items, const char* separator) {
    std::string text;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            text += separator;
        }
        text += items[i];
    }
    return text;
}

/** @brief 单条检查转 JSON 对象。 */
json checkToJson(const PluginTestCheck& check) {
    return json{
        {"id", check.id},
        {"status", pluginTestStatusName(check.status)},
        {"detail", check.detail},
    };
}

/**
 * @brief 组装 --json / --json-out 的结构化结果。
 * @param[in] report   自检报告。
 * @param[in] options  自检选项（--keep-data 影响 dataKept）。
 * @param[in] exitCode 最终退出码。
 * @param[in] totals   全部检查的计数。
 * @return JSON 对象。
 */
json buildReportJson(const PluginTestReport& report, const PluginTestOptions& options,
                     int exitCode, const PluginTestCheckCounts& totals) {
    json root = json::object();
    root["tool"] = "plugin-shot-test";
    root["version"] = VERSION_STRING;
    root["versionFull"] = VERSION_FULL;
    root["abiComment"] = report.abiComment;
    root["hostAbi"] = json{{"major", SHOT_ABI_MAJOR}, {"minor", SHOT_ABI_MINOR}};
    root["build"] = json{
        {"platform", PLATFORM_NAME_STR},
        {"architecture", ARCHITECTURE_STR},
        {"compiler", COMPILER_NAME_STR},
        {"type", BUILD_TYPE_STR},
    };
    root["dll"] = report.dll;
    root["copiedDll"] = report.copiedDll;
    root["dataDir"] = report.dataDir;
    root["dataKept"] = options.keepData;
    root["language"] = report.language;
    root["dependencies"] = report.dependencies;
    root["exitCode"] = exitCode;
    root["result"] = report.result;

    json checks = json::array();
    for (const PluginTestCheck& check : report.checks) {
        checks.push_back(checkToJson(check));
    }
    root["checks"] = std::move(checks);

    json plugins = json::array();
    for (const PluginTestPluginReport& plugin : report.plugins) {
        json item = json::object();
        item["id"] = plugin.id;
        item["name"] = plugin.name;
        item["version"] = plugin.version;
        item["state"] = plugin.state;
        item["type"] = plugin.type;
        if (!plugin.stateReason.empty()) {
            item["stateReason"] = plugin.stateReason;
        }
        if (!plugin.declaredInterfaces.empty()) {
            item["declaredInterfaces"] = plugin.declaredInterfaces;
        }
        json pluginChecks = json::array();
        for (const PluginTestCheck& check : plugin.checks) {
            pluginChecks.push_back(checkToJson(check));
        }
        item["checks"] = std::move(pluginChecks);
        plugins.push_back(std::move(item));
    }
    root["plugins"] = std::move(plugins);
    root["summary"] = json{
        {"pass", totals.pass},
        {"fail", totals.fail},
        {"skip", totals.skip},
        {"warn", totals.warn},
    };
    root["durationMs"] = report.durationMs;
    return root;
}

/** @brief 把一组检查里的 FAIL/WARN（--verbose 时连 PASS/SKIP）追加进人类摘要。 */
void appendCheckDetails(std::ostringstream& out, const std::vector<PluginTestCheck>& checks,
                        bool verbose) {
    for (const PluginTestCheck& check : checks) {
        if (check.status == PluginTestStatus::Fail) {
            out << "    FAIL " << check.id << ": " << check.detail << "\n";
        } else if (check.status == PluginTestStatus::Warn) {
            out << "    WARN " << check.id << ": " << check.detail << "\n";
        } else if (verbose) {
            out << "    " << (check.status == PluginTestStatus::Pass ? "PASS" : "SKIP") << " "
                << check.id << (check.detail.empty() ? "" : ": ") << check.detail << "\n";
        }
    }
}

/**
 * @brief 组装人类可读摘要。
 * @param[in] report   自检报告。
 * @param[in] options  自检选项（--keep-data / --verbose 影响文案）。
 * @param[in] exitCode 最终退出码。
 * @param[in] totals   全部检查的计数。
 * @return 多行文本（UTF-8）。
 */
std::string buildHumanSummary(const PluginTestReport& report, const PluginTestOptions& options,
                              int exitCode, const PluginTestCheckCounts& totals) {
    std::ostringstream out;
    out << "== Plugin-shot 插件自检（" << (report.result == "pass" ? "PASS" : "FAIL") << "）==\n";
    out << "被测 DLL: " << report.dll << "\n";
    out << "隔离数据目录: " << report.dataDir
        << (options.keepData ? "（保留，--keep-data）" : "（结束后删除）") << "\n";
    out << "语言: " << report.language << " | 宿主 ABI " << SHOT_ABI_MAJOR << "." << SHOT_ABI_MINOR
        << " | " << PLATFORM_NAME_STR << " " << ARCHITECTURE_STR << " " << BUILD_TYPE_STR << "\n";
    if (!report.dependencies.empty()) {
        out << "一并拷贝的依赖: " << joinText(report.dependencies, ", ") << "\n";
    }
    out << "工具/环境检查:\n";
    appendCheckDetails(out, report.checks, options.verbose);
    for (const PluginTestPluginReport& plugin : report.plugins) {
        const PluginTestCheckCounts counts = countPluginTestChecks(plugin.checks);
        const char* label = counts.fail > 0 ? "FAIL" : (counts.warn > 0 ? "WARN" : "PASS");
        out << "[" << label << "] " << plugin.id << "  state=" << plugin.state
            << " type=" << plugin.type << "  pass=" << counts.pass << " fail=" << counts.fail
            << " skip=" << counts.skip << " warn=" << counts.warn
            << (plugin.stateReason.empty() ? "" : "  reason=" + plugin.stateReason) << "\n";
        if (!plugin.declaredInterfaces.empty()) {
            out << "    metadata 声明能力: " << joinText(plugin.declaredInterfaces, ", ") << "\n";
        }
        appendCheckDetails(out, plugin.checks, options.verbose);
    }
    out << "检查汇总: pass=" << totals.pass << " fail=" << totals.fail << " skip=" << totals.skip
        << " warn=" << totals.warn << " | 耗时 " << report.durationMs << " ms\n";
    out << "结果: " << (report.result == "pass" ? "PASS" : "FAIL") << "（exit " << exitCode
        << "）\n";
    return out.str();
}

}  // namespace

namespace Host {

int runPluginTest(int argc, char** argv) {
    PluginTestOptions options;
    std::string error;
    switch (parseOptions(argc, argv, options, error)) {
        case ParseOutcome::Help:
            printTestUsage(std::cout);
            return 0;
        case ParseOutcome::Error:
            std::cerr << "[plugin-test] " << error << "\n";
            printTestUsage(std::cerr);
            return 2;
        case ParseOutcome::Ok:
            break;
    }

    const auto startTime = std::chrono::steady_clock::now();

    fs::path sourceDll;
    if (!resolveSourceDll(options.dllPath, sourceDll, error)) {
        std::cerr << "[plugin-test] " << error << "\n";
        return 2;
    }

    fs::path dataDir;
    if (!createIsolatedDataDir(dataDir, error)) {
        std::cerr << "[plugin-test] " << error << "\n";
        return 2;
    }

    // 从这里开始任何提前失败都要把临时目录清掉；keep-data 只对"跑完"的结果生效。
    // 重试几轮：DLL/日志句柄在 Windows 上释放有延迟，一次失败不代表删不掉。
    const auto removeDataDir = [&dataDir] {
        std::error_code ec;
        bool removed = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            ec.clear();
            fs::remove_all(dataDir, ec);
            if (!ec) {
                removed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!removed) {
            std::cerr << "[plugin-test] 警告：清理隔离数据目录失败 " << dataDir.string() << ": "
                      << ec.message() << "\n";
            return;
        }
        // 顺手清掉空的父目录（<临时目录>/plugin-shot-test）；还有别的自检在跑时删不掉，
        // 属正常（fs::remove 只删空目录，失败忽略）。
        std::error_code parentEc;
        fs::remove(dataDir.parent_path(), parentEc);
    };

    if (!setDataDirEnvironment(dataDir, error)) {
        std::cerr << "[plugin-test] " << error << "\n";
        removeDataDir();
        return 2;
    }

    // 顺序与 main 保持一致：路径 -> 日志 -> 配置 -> 运行期。环境的覆盖必须在
    // 这里之前设置好，否则 core::dataPath 会落到用户真实目录。
    core::getAppPath();
    core::getDataPath();
    if (core::dataPath.empty()) {
        std::cerr << "[plugin-test] 隔离数据目录解析为空\n";
        removeDataDir();
        return 2;
    }
    {
        std::error_code ec;
        fs::create_directories(core::dataPath, ec);
        if (ec) {
            std::cerr << "[plugin-test] 无法创建数据目录 " << core::dataPath << ": " << ec.message()
                      << "\n";
            removeDataDir();
            return 2;
        }
    }

    Logging::init();
    // --json 时 stdout 只能有 JSON：人类摘要与插件日志都改走日志文件。
    Logging::setConsoleEnabled(!options.jsonStdout);
    spdlog::info("插件自检: dll={} dataDir={}", sourceDll.string(), core::dataPath);
    startWatchdog(options.timeoutMs);

    const auto abortRun = [&](const std::string& message) -> int {
        stopWatchdog();
        spdlog::error("插件自检提前失败: {}", message);
        Logging::shutdown();
        std::cerr << "[plugin-test] " << message << "\n";
        removeDataDir();
        return 2;
    };

    // 配置在 HostRuntime::init 之前初始化：--language 要赶在 I18n::loadPreference
    // 之前写进内存配置，否则 i18n 快照不会按请求的语言生成。
    core::Config::getInstance().init(core::dataPath + "/config.json");
    core::Config::getInstance().set(std::string("core/") + I18n::preferenceKey(),
                                    core::ConfigItem(options.language));
    // 自检是"显式点名要测这个插件"，不能被用户配置里的禁用列表跳过：真实宿主
    // 的默认禁用列表里有实验插件（example.invert / example.grayscale /
    // long_capture.main），不清掉的话自检永远测的是"已禁用"而不是插件本身。
    // 这里只改隔离配置的内存值，不碰用户真实配置。
    core::Config::getInstance().set("plugins.disabled", core::ConfigItem("[]"));
    if (options.jsonStdout) {
        // --json 时 stdout 只能有 JSON。只调 setConsoleEnabled(false) 不够：
        // HostRuntime::init 会把 core/debug.console 的默认值回灌给宿主设置（默认 1），
        // 重新打开控制台 sink；所以在隔离配置里把它置 0，让"回灌"本身就保持关闭。
        core::Config::getInstance().set("core/debug.console", core::ConfigItem("0"));
    }

    Plugin::PluginPackageMeta metadata;
    std::string metadataError;
    const bool metadataAvailable = metadata.readFromFile(sourceDll.string(), metadataError);

    const fs::path pluginsDir = dataDir / "plugins";
    DependencyCopy dependencyCopy;
    if (!copyDllWithDependencies(sourceDll, metadata, metadataAvailable, pluginsDir,
                                 dependencyCopy, error)) {
        return abortRun(error);
    }
    spdlog::info("插件目录: {}（被测 DLL + {} 个依赖）", pluginsDir.string(),
                 dependencyCopy.copied.size());

    PluginTestReport report;
    report.dll = sourceDll.string();
    report.copiedDll = (pluginsDir / sourceDll.filename()).string();
    report.dataDir = dataDir.string();
    report.language = options.language;
    report.dependencies = dependencyCopy.copied;
    report.abiComment = std::string("宿主 ABI ") + std::to_string(SHOT_ABI_MAJOR) + "." +
                        std::to_string(SHOT_ABI_MINOR) + "（" + ARCHITECTURE_STR + "/" +
                        BUILD_TYPE_STR +
                        "）：插件必须用同一 ABI 主版本、同一架构、同一构建配置（Debug/Release）"
                        "编译，且不能加壳/混淆；不满足时 Windows 上的表现可能是 LoadLibrary"
                        " 直接失败而不是可读错误。";

    if (metadataAvailable) {
        addPluginTestCheck(report.checks, "metadata", PluginTestStatus::Pass,
                           "metadata.json 可解析：schema=" + std::to_string(metadata.getSchema()) +
                               "，abi=" + std::to_string(metadata.getAbiMajor()) + "." +
                               std::to_string(metadata.getAbiMinor()) + "，插件条目 " +
                               std::to_string(metadata.getMetaData().size()) + " 个");
    } else {
        addPluginTestCheck(
            report.checks, "metadata", PluginTestStatus::Fail,
            "DLL 尾部没有可解析的 metadata.json：" + metadataError +
                "（第三方插件包必须带 metadata，字段见 document/METADATA.md；缺失时宿主只能按"
                "无元数据包兜底加载，依赖与能力声明全部不可用）");
    }
    if (!dependencyCopy.missing.empty()) {
        addPluginTestCheck(report.checks, "dependencies", PluginTestStatus::Warn,
                           "metadata 声明了这些依赖，但在被测 DLL 同目录找不到提供者：" +
                               joinText(dependencyCopy.missing, ", ") +
                               "（宿主会按缺少依赖跳过，属部署环境不完整）");
    } else if (dependencyCopy.copied.empty()) {
        addPluginTestCheck(report.checks, "dependencies", PluginTestStatus::Skip,
                           "metadata 没有声明 core 之外的依赖");
    } else {
        addPluginTestCheck(report.checks, "dependencies", PluginTestStatus::Pass,
                           "依赖提供者已一并拷入隔离目录：" +
                               joinText(dependencyCopy.copied, ", "));
    }

    int exitCode = 0;
    try {
        HostRuntime runtime;
        const bool runtimeReady = runtime.init(pluginsDir.string());
        if (!runtimeReady) {
            addPluginTestCheck(report.checks, "host.init", PluginTestStatus::Fail,
                               "HostRuntime::init 失败：插件目录不可用（" + pluginsDir.string() +
                                   "）");
        } else {
            addPluginTestCheck(report.checks, "host.init", PluginTestStatus::Pass,
                               "HostRuntime::init 成功（真实 PluginManager 加载路径）");
            runPluginChecks(options, runtime, report.copiedDll, metadata, metadataAvailable,
                            report);
        }
        // 离开作用域 = 真实卸载：UI deinit -> destroy -> module shutdown -> FreeLibrary。
    } catch (const std::exception& exception) {
        addPluginTestCheck(report.checks, "host.exception", PluginTestStatus::Fail,
                           std::string("宿主/插件抛出了未捕获的 C++ 异常（异常不得跨 DLL 边界）：") +
                               exception.what());
    } catch (...) {
        addPluginTestCheck(report.checks, "host.exception", PluginTestStatus::Fail,
                           "宿主/插件抛出了未知类型的 C++ 异常（异常不得跨 DLL 边界）");
    }

    PluginTestCheckCounts totals;
    for (const PluginTestCheck& check : report.checks) {
        switch (check.status) {
            case PluginTestStatus::Pass: ++totals.pass; break;
            case PluginTestStatus::Fail: ++totals.fail; break;
            case PluginTestStatus::Skip: ++totals.skip; break;
            case PluginTestStatus::Warn: ++totals.warn; break;
        }
    }
    for (const PluginTestPluginReport& plugin : report.plugins) {
        const PluginTestCheckCounts counts = countPluginTestChecks(plugin.checks);
        totals.pass += counts.pass;
        totals.fail += counts.fail;
        totals.skip += counts.skip;
        totals.warn += counts.warn;
    }
    report.result = totals.fail == 0 ? "pass" : "fail";
    report.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - startTime)
                            .count();
    exitCode = totals.fail == 0 ? 0 : 1;

    // 输出：--json 时 stdout 只有 JSON（summary 与日志都进日志文件）；否则人类摘要
    // 走 stdout。FAIL 明细始终出现在人类摘要里，同时也在 JSON 的 checks.detail 里。
    const std::string humanSummary = buildHumanSummary(report, options, exitCode, totals);
    const json reportJson = buildReportJson(report, options, exitCode, totals);
    const std::string jsonText =
        reportJson.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    if (!options.jsonOutPath.empty()) {
        std::string writeError;
        if (!writeTextFile(fs::path(options.jsonOutPath), jsonText, writeError)) {
            spdlog::error("写 --json-out 失败: {}", writeError);
            std::cerr << "[plugin-test] 写 --json-out 失败: " << writeError << "\n";
            exitCode = 2;
        }
    }
    if (options.jsonStdout) {
        // spdlog 此时控制台 sink 已关闭：这些行的唯一去处是 <dataDir>/logs/host.log。
        std::istringstream lines(humanSummary);
        std::string line;
        while (std::getline(lines, line)) {
            spdlog::info("{}", line);
        }
    } else {
        std::cout << humanSummary;
        std::cout.flush();
    }

    // 先停看门狗（它可能正等超时），再关日志：异步 sink 关掉之后文件句柄才释放，
    // Windows 上才删得掉数据目录，也才能回读日志尾部。
    stopWatchdog();
    Logging::shutdown();
    if (exitCode != 0) {
        dumpLogTail(dataDir);
    }
    if (!options.keepData) {
        removeDataDir();
    } else if (!options.jsonStdout) {
        std::cout << "隔离数据目录已保留: " << dataDir.string() << "\n";
    }
    if (options.jsonStdout) {
        std::cout << jsonText;
        std::cout.flush();
    }
    return exitCode;
}

}  // namespace Host
