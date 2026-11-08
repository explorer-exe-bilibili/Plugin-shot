#include "hostLogging.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "base.h"

namespace {

constexpr std::size_t kAsyncQueueSize = 8192;  /**< 异步日志队列上限，满了阻塞 */
constexpr int kAsyncThreadCount = 1;           /**< 异步写线程数：单线程保证文件顺序 */

/**
 * @brief 可开关的 sink 过滤层：包住真正的 sink，只决定要不要 forward。
 *
 * 为什么不是运行时增删 sink：异步 logger 的后台线程在遍历 sink 列表，
 * 增删会让它读到一半被改掉的 vector（数据竞争 + 迭代器失效）。这里一个原子
 * 开关就够了，关闭后连 flush 也不透传，避免关掉控制台后仍每轮刷屏。
 *
 * @note 转发对象只在 init() 里换一次，之后只读；开关本身是 relaxed 原子，
 *       日志顺序不要求与开关顺序严格一致。
 */
class FilterSink final : public spdlog::sinks::sink {
public:
    /**
     * @brief 包住一个真实 sink。
     * @param[in] target 真正的 sink，非空；生命周期比本对象长（本对象被 logger 持有）。
     */
    explicit FilterSink(spdlog::sink_ptr target) : m_target(std::move(target)) {}

    /** @brief 开关打开时把日志交给目标 sink。 */
    void log(const spdlog::details::log_msg& msg) override {
        if (m_enabled.load(std::memory_order_relaxed)) {
            m_target->log(msg);
        }
    }

    /** @brief 开关打开时 flush 目标 sink。 */
    void flush() override {
        if (m_enabled.load(std::memory_order_relaxed)) {
            m_target->flush();
        }
    }

    /** @brief 直接转给目标 sink（设置格式是初始化期行为）。 */
    void set_pattern(const std::string& pattern) override { m_target->set_pattern(pattern); }

    /** @brief 直接转给目标 sink（设置格式是初始化期行为）。 */
    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override {
        m_target->set_formatter(std::move(formatter));
    }

    /** @brief 运行期开关；任意线程可调。 */
    void setEnabled(bool enabled) { m_enabled.store(enabled, std::memory_order_relaxed); }

    /** @brief 当前开关状态。 */
    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }

private:
    spdlog::sink_ptr m_target;
    std::atomic<bool> m_enabled{true};
};

/**
 * @brief 把配置里的级别名解析成 spdlog 级别。
 * @param[in] level 级别名，大小写不敏感。
 * @return 解析结果；不认识时返回 off + ok = false。
 */
spdlog::level::level_enum parseLevel(const std::string& level, bool& ok) {
    std::string lower;
    lower.reserve(level.size());
    for (const char ch : level) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    ok = true;
    if (lower == "debug") {
        return spdlog::level::debug;
    }
    if (lower == "info") {
        return spdlog::level::info;
    }
    if (lower == "warn" || lower == "warning") {
        return spdlog::level::warn;
    }
    if (lower == "error" || lower == "err") {
        return spdlog::level::err;
    }
    ok = false;
    return spdlog::level::off;
}

/**
 * @brief 把 spdlog 级别转回配置里用的名字。
 * @param[in] level spdlog 级别。
 * @return 级别名；off 及其它未知值按 "error" 处理（配置只暴露四档）。
 */
std::string levelName(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::debug: return "debug";
        case spdlog::level::info:  return "info";
        case spdlog::level::warn:  return "warn";
        case spdlog::level::err:   return "error";
        default:                   return "info";
    }
}

}  // namespace

// 只在本 TU 可见的日志状态。指针由 logger 持有、这里也留一份用于开关。
static std::mutex logStateMutex;                       /**< 保护下面三个状态 */
static std::shared_ptr<FilterSink> consoleSink;        /**< 控制台过滤 sink；未初始化时为空 */
static std::shared_ptr<FilterSink> fileSink;           /**< 文件过滤 sink；建不出来时为空 */
static std::string currentLevel = "info";              /**< 当前级别名，给 get_config 读 */

void Host::Logging::init() {
    std::lock_guard<std::mutex> lock(logStateMutex);
    if (consoleSink != nullptr) {
        return;  // 已经初始化过：重复调用是空操作
    }

    spdlog::init_thread_pool(kAsyncQueueSize, kAsyncThreadCount);

    std::vector<spdlog::sink_ptr> sinks;
    consoleSink = std::make_shared<FilterSink>(
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.push_back(consoleSink);

    bool fileOpened = false;
    if (!core::dataPath.empty()) {
        const std::filesystem::path logDir = std::filesystem::path(core::dataPath) / "logs";
        std::error_code ec;
        std::filesystem::create_directories(logDir, ec);
        if (!ec) {
            // 文件 sink 构造可能抛 spdlog_ex（权限/磁盘），失败就只留控制台。
            try {
                fileSink = std::make_shared<FilterSink>(
                    std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                        (logDir / "host.log").string(), /*truncate=*/false));
                sinks.push_back(fileSink);
                fileOpened = true;
            } catch (const std::exception&) {
                fileSink.reset();
            }
        }
    }

    auto hostLogger = std::make_shared<spdlog::async_logger>(
        "host", sinks.begin(), sinks.end(), spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    auto pluginLogger = std::make_shared<spdlog::async_logger>(
        "plugin", sinks.begin(), sinks.end(), spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);

    for (const auto& logger : {hostLogger, pluginLogger}) {
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%-7l] [%n] %v");
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::warn);
    }

    spdlog::set_default_logger(hostLogger);
    spdlog::register_logger(pluginLogger);
    spdlog::flush_every(std::chrono::seconds(1));

    if (!fileOpened) {
        spdlog::warn("File logging disabled: cannot create {}", core::dataPath + "/logs/host.log");
    }
}

void Host::Logging::setLevel(const std::string& level) {
    bool ok = false;
    const spdlog::level::level_enum parsed = parseLevel(level, ok);
    if (!ok) {
        spdlog::warn("Unknown log level '{}', keeping current one", level);
        return;
    }

    std::lock_guard<std::mutex> lock(logStateMutex);
    currentLevel = levelName(parsed);
    for (const char* name : {"host", "plugin"}) {
        const auto logger = spdlog::get(name);
        if (logger != nullptr) {
            logger->set_level(parsed);
        }
    }
}

std::string Host::Logging::level() {
    std::lock_guard<std::mutex> lock(logStateMutex);
    return currentLevel;
}

bool Host::Logging::setConsoleEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(logStateMutex);
    if (consoleSink == nullptr) {
        return false;
    }
    consoleSink->setEnabled(enabled);
    return true;
}

bool Host::Logging::setFileEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(logStateMutex);
    if (fileSink == nullptr) {
        return false;
    }
    fileSink->setEnabled(enabled);
    return true;
}

bool Host::Logging::consoleEnabled() {
    std::lock_guard<std::mutex> lock(logStateMutex);
    return consoleSink != nullptr && consoleSink->enabled();
}

bool Host::Logging::fileEnabled() {
    std::lock_guard<std::mutex> lock(logStateMutex);
    return fileSink != nullptr && fileSink->enabled();
}
