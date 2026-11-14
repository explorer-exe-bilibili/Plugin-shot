// 日期归档 demo 的插件状态实现：订阅 capture.done → 入队 → worker 线程做文件操作。
//
// 设计要点（用户原话："截图保存后自动按日期归档到「年-月-日」文件夹。"）：
//   · 事件回调绝不碰文件系统：它跑在宿主事件分发线程，文件 IO 的抖动会拖慢所有
//     插件；回调只把路径拷贝进入队缓冲并唤醒 worker（document/EVENT_BUS.md §5）。
//   · 目标根为空时用"源文件所在目录"，这样默认行为只是就地按日期整理；
//   · 同名文件不覆盖：filename、filename_1、filename_2……；
//   · move 优先用 rename（同卷原子且快）；跨卷/目标被占用会让 rename 失败，
//     此时回退 copy_file + 删除源文件；任何一步失败都只 WARN 并保留原文件。
//   · 关闭/卸载时先退订（不再有新任务），再让 worker 把已入队任务做完才退出，
//     否则"截图刚落盘、插件被禁用"时会丢一张图的归档。
//
// label / tooltip 与 ENUM 候选值都是语言表 key，不是中文：设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析，文案本体在 kLangTables。

#include "archive_plugin.h"

#include <cstring>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "plugin_entry.h"

namespace archive_demo {
namespace {

// capture.done 的主题名：与 document/CAPTURE.md §3 一致。
const char kCaptureDoneTopic[] = "capture.done";

// 队列上限：正常一次截图只有一条事件；上限只是防御"事件洪水/宿主异常"导致内存
// 无限增长，满了丢最旧还是最新？这里选择丢新，并 WARN 提示用户。
constexpr std::size_t kMaxQueueLength = 64;

const char kKeyDirectory[] = "directory";
const char kKeyMode[] = "mode";

// 默认值只在描述符里写一遍，构造函数也从这两个常量取，避免两处漂移。
const char kDefaultDirectory[] = "";  // 空 = 源文件所在目录
const char kDefaultMode[] = "mode.move";

const char kOptionMove[] = "mode.move";
const char kOptionCopy[] = "mode.copy";

const char kLabelDirectory[] = "config.directory.label";
const char kTooltipDirectory[] = "config.directory.tooltip";
const char kLabelMode[] = "config.mode.label";
const char kTooltipMode[] = "config.mode.tooltip";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_PATH,
        .key           = kKeyDirectory,
        .label         = kLabelDirectory,
        .default_value = kDefaultDirectory,
        // PATH 借 options 声明对话框类型：空默认值时不写会被推断成"选文件"。
        .options       = "directory",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipDirectory,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeyMode,
        .label         = kLabelMode,
        .default_value = kDefaultMode,
        .options       = "mode.move|mode.copy",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipMode,
    },
};

// 语言表：每语言一张单行 JSON（@meta 必填）；整行会超过 120 列，所以用相邻
// raw 字面量拼接——拼出来仍然是一行合法 JSON。各语言的 key 集必须一致。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"日期归档",)json"
    R"json("plugin.description":"截图保存后自动按「年-月-日」文件夹归档",)json"
    R"json("config.directory.label":"归档根目录",)json"
    R"json("config.directory.tooltip":"留空 = 源文件所在目录；子目录按 YYYY-MM-DD 自动创建",)json"
    R"json("config.mode.label":"归档方式",)json"
    R"json("config.mode.tooltip":"移动 = 原文件移进日期目录；复制 = 保留原文件",)json"
    R"json("mode.move":"移动",)json"
    R"json("mode.copy":"复制"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"Date archive",)json"
    R"json("plugin.description":"Archives saved screenshots into a YYYY-MM-DD folder",)json"
    R"json("config.directory.label":"Archive root",)json"
    R"json("config.directory.tooltip":"Empty = the source file's folder; YYYY-MM-DD )json"
    R"json("subfolders are created automatically",)json"
    R"json("config.mode.label":"Archive mode",)json"
    R"json("config.mode.tooltip":"Move = move the original into the date folder; Copy = keep the original",)json"
    R"json("mode.move":"Move",)json"
    R"json("mode.copy":"Copy"})json",
};

/** @brief 把 ENUM 候选值解析成内部枚举；不认识的值回退 move。 */
ArchiveMode parseMode(const char* value) {
    if (value != nullptr &&
        (std::strcmp(value, "mode.copy") == 0 || std::strcmp(value, "copy") == 0)) {
        return ArchiveMode::Copy;
    }
    return ArchiveMode::Move;
}

/** @brief 内部枚举 → 配置里存的值（带命名空间的 i18n key）。 */
const char* modeOption(ArchiveMode mode) {
    return mode == ArchiveMode::Copy ? kOptionCopy : kOptionMove;
}

/** @brief 按契约把字符串写进调用方缓冲（buf_size 大于长度才写入，含结尾 NUL）。 */
std::int32_t copyValue(const char* value, char* buf, std::uint32_t bufSize) {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    if (buf != nullptr && bufSize > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

/** @brief 把静态描述符拷进宿主提供的 ShotConfigDesc。 */
std::int32_t copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (out == nullptr || src == nullptr) {
        return 1;
    }
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = *src;
    return 0;
}

/** @brief UTF-8 文本 → std::filesystem::path（Windows 上必须走 u8string 才是真 UTF-8）。 */
std::filesystem::path pathFromUtf8(const std::string& text) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
    return std::filesystem::path(text);
#endif
}

/** @brief std::filesystem::path → UTF-8 文本（日志与重名构造用）。 */
std::string toUtf8(const std::filesystem::path& path) {
#if defined(__cpp_char8_t)
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return path.u8string();
#endif
}

/** @brief 当前本地日期，格式 YYYY-MM-DD（localtime_s / localtime_r 线程安全）。 */
std::string currentDateFolder() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &now) != 0) {
        return "0000-00-00";
    }
#else
    if (localtime_r(&now, &local) == nullptr) {
        return "0000-00-00";
    }
#endif
    char buffer[16] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local) == 0) {
        return "0000-00-00";
    }
    return buffer;
}

}  // namespace

ArchivePlugin::ArchivePlugin()
    : m_rootDirectory(kDefaultDirectory), m_mode(parseMode(kDefaultMode)) {}

ArchivePlugin::~ArchivePlugin() {
    stop();
}

bool ArchivePlugin::start(const ShotHostServices* host) {
    m_host = host;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_rootDirectory = kDefaultDirectory;
        m_mode = parseMode(kDefaultMode);
    }

    // 语言表要在宿主生成 config_list 快照之前发布；create() 拿不到服务表，
    // 这里是第一个能拿到 host 的生命周期路径。
    m_i18n = std::make_unique<shot::I18n>(host, kArchive.id);
    if (!m_i18n->loadTables(kLangTables, std::size(kLangTables))) {
        shot::Logger(host).warn("[archive] 语言表解析失败，配置项文案回退为 key");
    }

    // 先起 worker 再订阅：保证订阅之后到达的每条事件都有消费者，不会出现
    // "事件入队了但线程还没起来"的窗口。
    try {
        m_worker = std::thread(&ArchivePlugin::workerLoop, this);
    } catch (...) {
        shot::Logger(host).error("[archive] 创建归档线程失败，日期归档不可用");
        m_i18n.reset();
        return false;
    }

    shot::EventBus bus(host);
    if (!bus.subscribe(kCaptureDoneTopic, &ArchivePlugin::onCaptureDone, this)) {
        // 老宿主没有事件总线（或订阅被拒）：安静降级，不能留下空转的 worker 线程。
        shot::Logger(host).warn("[archive] 订阅 capture.done 失败（宿主无事件总线），日期归档不可用");
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_stopping = true;
        }
        m_queueCv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        m_i18n.reset();
        return false;
    }
    m_subscribed = true;
    shot::Logger(host).info("[archive] 已订阅 capture.done：截图保存后按日期归档");
    return true;
}

void ArchivePlugin::stop() {
    // 先退订：保证之后不会再有新任务入队，worker 可以把"已入队"的任务做完。
    if (m_subscribed) {
        shot::EventBus bus(m_host);
        bus.unsubscribe(this);
        m_subscribed = false;
    }
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopping = true;
    }
    m_queueCv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();  // 返回后本实例不会再有任何后台任务
    }
    // I18n 析构会退订 lists.changed；放在 worker 退出之后，避免回调一个正在销毁的状态。
    m_i18n.reset();
}

void ArchivePlugin::onCaptureDone(void* user, const char* /*topic*/, const void* data,
                                  std::uint32_t len) {
    auto* self = static_cast<ArchivePlugin*>(user);
    if (self == nullptr) {
        return;
    }
    // 回调期间负载有效：立刻拷成本地字符串，之后不再碰 data 指针。
    // 这里只解析 + 入队，绝不在这里做文件 IO。
    try {
        std::string payload;
        if (data != nullptr && len > 0) {
            payload.assign(static_cast<const char*>(data), len);
        }
        const nlohmann::json event = nlohmann::json::parse(payload, nullptr, false);
        if (event.is_discarded()) {
            shot::Logger(self->m_host).warn("[archive] capture.done 负载不是合法 JSON，忽略");
            return;
        }
        const auto pathIt = event.find("path");
        if (pathIt == event.end() || !pathIt->is_string() || pathIt->get<std::string>().empty()) {
            // 保存插件没写回路径（或宿主在别的阶段）时没有可归档的文件，安静跳过。
            shot::Logger(self->m_host).debug("[archive] capture.done 没有 path，跳过归档");
            return;
        }
        std::string source = pathIt->get<std::string>();
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(self->m_queueMutex);
            if (self->m_stopping) {
                return;
            }
            if (self->m_queue.size() >= kMaxQueueLength) {
                dropped = true;
            } else {
                self->m_queue.push_back(std::move(source));
            }
        }
        if (dropped) {
            shot::Logger(self->m_host).warn("[archive] 归档队列已满，丢弃本次任务");
            return;
        }
        self->m_queueCv.notify_one();
    } catch (...) {
        shot::Logger(self->m_host).warn("[archive] 处理 capture.done 时异常，已忽略本次事件");
    }
}

void ArchivePlugin::workerLoop() {
    for (;;) {
        std::string sourcePath;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) {
                break;  // 已停止且队列空：退出
            }
            sourcePath = std::move(m_queue.front());
            m_queue.pop_front();
        }
        try {
            archiveOne(sourcePath);
        } catch (...) {
            shot::Logger(m_host).warn("[archive] 归档时异常，已跳过：" + sourcePath);
        }
    }
}

void ArchivePlugin::archiveOne(const std::string& sourcePath) {
    const std::filesystem::path source = pathFromUtf8(sourcePath);
    std::error_code fsError;
    if (!std::filesystem::exists(source, fsError) || fsError) {
        shot::Logger(m_host).warn("[archive] 源文件不存在或不可访问，跳过：" + sourcePath);
        return;
    }

    std::string rootDirectory;
    ArchiveMode mode = ArchiveMode::Move;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        rootDirectory = m_rootDirectory;
        mode = m_mode;
    }

    // 目标根为空 = 源文件所在目录；这一步让"默认行为"只是就地按日期整理。
    std::filesystem::path rootPath = rootDirectory.empty()
                                         ? source.parent_path()
                                         : pathFromUtf8(rootDirectory);
    if (rootPath.empty()) {
        rootPath = std::filesystem::current_path(fsError);
    }
    if (fsError) {
        shot::Logger(m_host).warn("[archive] 取当前目录失败，跳过：" + sourcePath);
        return;
    }

    const std::filesystem::path destDir = rootPath / currentDateFolder();
    std::filesystem::create_directories(destDir, fsError);
    if (fsError) {
        shot::Logger(m_host).warn("[archive] 创建日期目录失败（" + fsError.message() +
                                  "），跳过：" + sourcePath);
        return;
    }

    // 同名文件不覆盖：filename、filename_1、filename_2……最多试 1000 次。
    const std::string stem = toUtf8(source.stem());
    const std::string extension = toUtf8(source.extension());
    std::filesystem::path target = destDir / source.filename();
    for (int suffix = 1; std::filesystem::exists(target, fsError) && suffix < 1000; ++suffix) {
        target = destDir / pathFromUtf8(stem + "_" + std::to_string(suffix) + extension);
    }
    if (std::filesystem::exists(target, fsError)) {
        shot::Logger(m_host).warn("[archive] 目标目录里同名文件过多，跳过：" + sourcePath);
        return;
    }

    if (mode == ArchiveMode::Copy) {
        std::error_code copyError;
        std::filesystem::copy_file(source, target, copyError);
        if (copyError) {
            shot::Logger(m_host).warn("[archive] 复制失败（" + copyError.message() +
                                      "），保留原文件：" + sourcePath);
            return;
        }
        shot::Logger(m_host).info("[archive] 已复制：" + toUtf8(target));
        return;
    }

    // move 优先 rename：同卷时是原子操作，也最省 IO。
    std::error_code renameError;
    std::filesystem::rename(source, target, renameError);
    if (!renameError) {
        shot::Logger(m_host).info("[archive] 已移动：" + toUtf8(target));
        return;
    }

    // 跨卷 / 目标被占用 / 权限不足：rename 会失败，回退 copy + 删除。
    std::error_code copyError;
    std::filesystem::copy_file(source, target, copyError);
    if (copyError) {
        shot::Logger(m_host).warn("[archive] 移动失败且复制回退也失败（" + copyError.message() +
                                  "），保留原文件：" + sourcePath);
        return;
    }
    std::error_code removeError;
    std::filesystem::remove(source, removeError);
    if (removeError) {
        // 复制成功但删不掉源文件：目标文件已存在，保留原文件更安全（用户自己清理）。
        shot::Logger(m_host).warn("[archive] 已复制到目标，但删除原文件失败（" +
                                  removeError.message() + "）：" + sourcePath);
        return;
    }
    shot::Logger(m_host).info("[archive] 已移动（rename 失败后回退 copy）：" + toUtf8(target));
}

std::uint32_t ArchivePlugin::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t ArchivePlugin::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t ArchivePlugin::getConfig(const char* key, char* buf,
                                      std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (std::strcmp(key, kKeyDirectory) == 0) {
        return copyValue(m_rootDirectory.c_str(), buf, bufSize);
    }
    if (std::strcmp(key, kKeyMode) == 0) {
        return copyValue(modeOption(m_mode), buf, bufSize);
    }
    return -1;  // 不认识的 key：安全忽略
}

std::int32_t ArchivePlugin::setConfig(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (std::strcmp(key, kKeyDirectory) == 0) {
        m_rootDirectory = value;
        return 0;
    }
    if (std::strcmp(key, kKeyMode) == 0) {
        m_mode = parseMode(value);
        return 0;
    }
    return -1;
}

}  // namespace archive_demo
