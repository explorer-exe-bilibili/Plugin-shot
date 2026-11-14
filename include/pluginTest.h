#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "plugin_meta.h"

// ============================================================================
//  Host::runPluginTest —— `--test-plugin` 插件自检模式
//
//  用法（详细说明见 document/PLUGIN_TEST.md）：
//
//    Plugin-shot --test-plugin <dll> [--json] [--json-out <file>]
//                [--keep-data] [--timeout-ms <n>] [--language <tag>] [--verbose]
//
//  它不是"假宿主"：先把被测 DLL（以及它在同目录里能找到的依赖提供者）拷进一个
//  隔离插件目录，再把 PLUGIN_SHOT_DATA_DIR 指到临时数据目录，然后走真实的
//  HostRuntime::init（PluginManager 扫描/校验/依赖/加载 + 真实服务表），检查完
//  再把 HostRuntime 析构掉（真实卸载）。因此它验证的是"宿主真的会怎么加载它"。
//
//  为什么要隔离数据目录：自检是 CI/skill 会反复调用的东西，绝不能读写用户真实的
//  配置与日志；插件在 create 阶段贡献的列表项、配置项也只在临时目录里生效。
//
//  退出码：0 = 没有 fail；1 = 至少一条 fail；2 = 用法/IO 错误或超时。
// ============================================================================

namespace Host {

class HostRuntime;

/** @brief 自检单条检查的结果。 */
enum class PluginTestStatus {
    Pass, /**< 检查通过。 */
    Fail, /**< 检查失败：插件不符合契约，退出码会变成 1。 */
    Skip, /**< 不适用（例如无显示器的机器上测屏幕采集、GUI 类插件不做 UI 检查）。 */
    Warn  /**< 可疑但不判定失败（字段缺失、版本号不可解析等）。 */
};

/**
 * @brief 一条检查结果。
 *
 * id 是稳定的机器标识（如 `capability.image_processor.process`），skill/CI 可以
 * 按 id 断言；detail 是给人看的中文说明，FAIL 时必须能直接指向可行动的原因。
 */
struct PluginTestCheck {
    std::string id;     /**< 稳定检查 id。 */
    PluginTestStatus status = PluginTestStatus::Skip; /**< 结果。 */
    std::string detail; /**< 人类可读说明；允许为空。 */
};

/** @brief 一个插件（一条 metadata 记录 / 一个实例）的检查报告。 */
struct PluginTestPluginReport {
    std::string id;               /**< 插件 id，如 "example.invert"。 */
    std::string name;             /**< 界面显示名。 */
    std::string version;          /**< 插件版本。 */
    std::string state;            /**< "loaded" / "disabled" / "failed"。 */
    std::string type;             /**< "image_processor" 等；没有实例时为 "none"。 */
    std::string stateReason;      /**< 非 loaded 时的原因（宿主本地化前的固定中文）。 */
    std::vector<std::string> declaredInterfaces; /**< metadata 声明的能力 id。 */
    std::vector<PluginTestCheck> checks;         /**< 该插件的全部检查。 */
};

/** @brief 一次自检的完整结果（--json 输出的数据源）。 */
struct PluginTestReport {
    std::string dll;          /**< 被测 DLL 的原始路径（解析成绝对路径后）。 */
    std::string copiedDll;    /**< 隔离插件目录里的副本路径。 */
    std::string dataDir;      /**< 隔离数据目录。 */
    std::string language;     /**< 自检使用的界面语言标签。 */
    std::string abiComment;   /**< 宿主 ABI 与构建兼容性说明（给 JSON 读者的人话）。 */
    std::string result;       /**< "pass" / "fail"。 */
    std::vector<std::string> dependencies;   /**< 一起拷进去的依赖提供者文件名。 */
    std::vector<PluginTestCheck> checks;     /**< 工具/环境级检查（加载、i18n、事件等）。 */
    std::vector<PluginTestPluginReport> plugins; /**< 按 metadata 记录逐个插件的结果。 */
    std::int64_t durationMs = 0;             /**< 整个自检耗时（毫秒）。 */
};

/** @brief 自检命令行选项（main 已确认进入自检模式，由 runPluginTest 完整解析）。 */
struct PluginTestOptions {
    std::string dllPath;     /**< --test-plugin 的值。 */
    std::string jsonOutPath; /**< --json-out 的值；空 = 不写文件。 */
    bool jsonStdout = false; /**< --json：stdout 只输出 JSON，人类摘要写日志。 */
    bool keepData = false;   /**< --keep-data：保留隔离数据目录（排错用）。 */
    int timeoutMs = 30000;   /**< --timeout-ms：整体超时；插件挂起时由看门狗硬退出。 */
    std::string language = "en-US"; /**< --language：i18n 快照使用的语言标签。 */
    bool verbose = false;    /**< --verbose：人类摘要里连 PASS/SKIP 明细一起打印。 */
};

/** @brief 状态转字符串（JSON 使用的 "pass" / "fail" / "skip" / "warn"）。 */
inline const char* pluginTestStatusName(PluginTestStatus status) {
    switch (status) {
        case PluginTestStatus::Pass: return "pass";
        case PluginTestStatus::Fail: return "fail";
        case PluginTestStatus::Skip: return "skip";
        case PluginTestStatus::Warn: return "warn";
    }
    return "skip";
}

/** @brief 各状态检查条数（summary / 人类摘要共用）。 */
struct PluginTestCheckCounts {
    int pass = 0; /**< 通过条数。 */
    int fail = 0; /**< 失败条数；> 0 时退出码为 1。 */
    int skip = 0; /**< 跳过条数。 */
    int warn = 0; /**< 警告条数。 */
};

/**
 * @brief 内部辅助：统计一组检查的各状态条数（pluginTest.cpp / pluginTestChecks.cpp 共用）。
 * @param[in] checks 检查列表。
 * @return 各状态计数。
 */
inline PluginTestCheckCounts countPluginTestChecks(const std::vector<PluginTestCheck>& checks) {
    PluginTestCheckCounts counts;
    for (const PluginTestCheck& check : checks) {
        switch (check.status) {
            case PluginTestStatus::Pass: ++counts.pass; break;
            case PluginTestStatus::Fail: ++counts.fail; break;
            case PluginTestStatus::Skip: ++counts.skip; break;
            case PluginTestStatus::Warn: ++counts.warn; break;
        }
    }
    return counts;
}

/**
 * @brief 内部辅助：追加一条检查（两个自检实现文件共用）。
 * @param[in,out] checks 目标列表。
 * @param[in]     id     稳定检查 id。
 * @param[in]     status 结果。
 * @param[in]     detail 人类可读说明。
 */
inline void addPluginTestCheck(std::vector<PluginTestCheck>& checks, std::string id,
                               PluginTestStatus status, std::string detail) {
    checks.push_back(PluginTestCheck{std::move(id), status, std::move(detail)});
}

/**
 * @brief `--test-plugin` 自检模式入口。
 *
 * 负责：解析全部参数 → 建隔离数据目录并设 PLUGIN_SHOT_DATA_DIR → 初始化日志/配置
 * → 拷贝被测 DLL（及能解析到的依赖提供者）→ HostRuntime 真实加载 → 逐插件检查
 * → 输出人类摘要 / JSON → 卸载并清理（--keep-data 时保留）。
 *
 * @param[in] argc 原始 argc（main 原样传入，含 argv[0]）。
 * @param[in] argv 原始 argv。
 * @return 0 = 无 fail；1 = 有 fail；2 = 用法/IO 错误或超时。
 * @note 只应在 main 里、`core::getAppPath()/getDataPath()` 之前调用一次：隔离目录要
 *       在解析 dataPath 之前设置，否则日志/配置会落到用户真实目录。
 */
int runPluginTest(int argc, char** argv);

/**
 * @brief 在已初始化的 HostRuntime 上执行逐插件检查（pluginTestChecks.cpp 实现）。
 *
 * 内部会像宿主主循环那样调用 dispatchEvents + tick 若干轮，让插件在 create 阶段
 * 贡献的列表项/配置项生效，然后检查加载状态、能力、配置回写、列表快照，最后做
 * 一次 capture.done / capture.failed 事件试运行。
 *
 * @param[in]     options          自检选项。
 * @param[in,out] runtime          已经 init 成功的宿主运行期。
 * @param[in]     copiedDllPath    隔离目录里的被测 DLL（与已安装记录的 dllPath 比对）。
 * @param[in]     metadata         被测 DLL 的 metadata；metadataAvailable 为 false 时无效。
 * @param[in]     metadataAvailable metadata 是否读成功。
 * @param[in,out] report           追加检查结果与逐插件报告。
 * @note 只允许在宿主 main 线程调用；不负责 runtime 的创建与销毁。
 */
void runPluginChecks(const PluginTestOptions& options, HostRuntime& runtime,
                     const std::string& copiedDllPath,
                     const Plugin::PluginPackageMeta& metadata, bool metadataAvailable,
                     PluginTestReport& report);

}  // namespace Host
