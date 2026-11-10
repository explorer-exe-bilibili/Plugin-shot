#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

#include <spdlog/spdlog.h>

#include "base.h"
#include "config.h"
#include "hostLogging.h"
#include "hostRuntime.h"
#include "hostServices.h"

namespace {

#ifdef _WIN32
/**
 * @brief 显式声明进程 DPI 感知，返回实际生效模式的描述（供日志记录）。
 *
 * 为什么必须显式声明：屏幕采集插件用 GDI 的 GetSystemMetrics/BitBlt 取虚拟桌面，
 * DPI-unaware 进程拿到的是被 DWM 虚拟化的逻辑坐标（150% 缩放下 3584x1600 会变成
 * 2389x1066），截出来的图尺寸和像素都不对。settings_ui 插件初始化 GLFW 时 GLFW 会
 * 顺手把进程设成 DPI 感知，所以"有设置页就碰巧对、没有设置页就错"——`--capture`、
 * 托盘被禁用、或者以后换掉 UI 插件时都会踩到。宿主必须在任何窗口/采集之前声明。
 *
 * 三档回退，全部动态解析：静态导入 SetProcessDpiAwarenessContext 会让老 Windows 在
 * 加载期就找不到符号（整个 exe 起不来），所以宁可 GetProcAddress：
 *   1. user32!SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)（Win10 1703+）；
 *   2. shcore!SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)（Win8.1+）；
 *   3. user32!SetProcessDPIAware()（Vista+，system aware）。
 *
 * @return 生效模式的简短描述；全部失败时返回失败说明（调用方只 warn，不阻止启动）。
 * @note 必须在任何窗口创建、任何采集之前调用；此时日志系统还没初始化，所以这里
 *       不写日志，由 main 拿到返回值后再记录。
 */
std::string enableProcessDpiAwareness() {
    if (const HMODULE user32 = GetModuleHandleW(L"user32.dll"); user32 != nullptr) {
        using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        const auto setContext =
            reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext != nullptr &&
            setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
            return "per-monitor v2（SetProcessDpiAwarenessContext）";
        }
    }
    // shcore 不是所有老系统都有；动态加载到了才用，用完立刻放掉。
    if (const HMODULE shcore = LoadLibraryW(L"shcore.dll"); shcore != nullptr) {
        using SetAwarenessFn = HRESULT(WINAPI*)(int);
        const auto setAwareness =
            reinterpret_cast<SetAwarenessFn>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
        // PROCESS_PER_MONITOR_DPI_AWARE = 2；用数值而不是枚举，避免老 SDK 缺头文件。
        const bool ok = setAwareness != nullptr && setAwareness(2) == S_OK;
        FreeLibrary(shcore);
        if (ok) {
            return "per-monitor（shcore!SetProcessDpiAwareness 回退）";
        }
    }
    if (SetProcessDPIAware() != FALSE) {
        return "system aware（SetProcessDPIAware 回退，多显示器 DPI 不同时仍可能不准）";
    }
    return "FAILED（多显示器/高 DPI 截图可能按逻辑坐标出图，尺寸与像素都会错）";
}
#endif

/**
 * @brief 打印命令行用法。
 *
 * 入口文件没有对应头文件，按 STYLE.md §6.7 的例外放在匿名命名空间。
 */
void printUsage() {
    std::cout << "用法: Plugin-shot [选项]\n"
              << "选项:\n"
              << "  --capture   截取一张屏幕截图并保存，打印路径后退出\n"
              << "  --settings  打开设置页并继续运行（调试入口）\n"
              << "  --help      显示本用法并退出\n"
              << "  不带参数    常驻运行（等 capture.request / app.quit 事件）\n";
}

}  // namespace

int main(int argc, char** argv) {
    // DPI 感知必须最先声明：任何窗口创建/屏幕采集之前，且日志系统初始化之前
    // （初始化本身不建窗口，但保持"最早"最稳妥；结果等日志就绪后再记）。
    std::string dpiAwareness;
#ifdef _WIN32
    dpiAwareness = enableProcessDpiAwareness();
    // Windows 控制台默认不是 UTF-8，中文路径会乱码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    // 命令行开关：--capture 截一张就退出；--settings 打开设置页后继续常驻。
    // 两者同时给出时按 --capture 处理（见下面的警告），--settings 被忽略。
    bool captureCommand = false;
    bool settingsCommand = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--capture") {
            captureCommand = true;
        } else if (arg == "--settings") {
            settingsCommand = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else {
            std::cerr << "未知参数: " << arg << "\n";
            printUsage();
            return 2;
        }
    }

    // 路径要在一切之前定下来：日志、配置、插件目录都依赖它们。
    core::getAppPath();
    core::getDataPath();
    std::error_code ec;
    std::filesystem::create_directories(core::dataPath, ec);

    Host::Logging::init();
    spdlog::info("Starting host application...");
    if (!dpiAwareness.empty()) {
        // 是否真的声明成功要能从日志里查：截图尺寸不对时第一眼就看这一行。
        spdlog::info("DPI awareness: {}", dpiAwareness);
    }
    if (ec) {
        spdlog::warn("Cannot create data directory {}: {}", core::dataPath, ec.message());
    }
    if (captureCommand && settingsCommand) {
        spdlog::warn("同时给出 --capture 与 --settings，按 --capture 处理（忽略 --settings）");
    }

    // 配置文件在插件加载之前读：插件加载阶段要读 plugins.disabled，配置目录
    // 在 init 里还会把已存值灌回插件。
    core::Config::getInstance().init(core::dataPath + "/config.json");

    int exitCode = 0;
    {
        Host::HostRuntime runtime;
        if (!runtime.init("./plugins")) {
            spdlog::critical("Failed to initialize host runtime from ./plugins");
            exitCode = 1;
        } else if (captureCommand) {
            // --capture：提交一次采集并同步等 worker 完成，打印最终路径后退出。
            // 抓屏/处理/编码都在 CapturePipeline 的工作线程上跑，main 线程只等结果；
            // 失败（含请求队列满被丢弃）一律返回非 0。
            std::string savedPath;
            const bool captured = runtime.capturePipeline().captureOnce(&savedPath);
            // 一次性模式也必须派发本轮事件：capture.done / capture.failed 是 worker
            // 发布到事件总线的（publish 只入队），剪贴板/通知等订阅者要等 dispatch
            // 才会被回调。这里在 main 线程、且不持生命周期锁，派发是安全的；漏掉这
            // 一步就会出现"文件已落盘，但 --capture 时剪贴板拿不到图"。
            Host::HostServices::dispatchEvents();
            if (captured) {
                spdlog::info("Capture saved: {}", savedPath);
            } else {
                spdlog::error("Capture failed: 没有落盘，原因见上面的 capture.failed 日志");
                exitCode = 1;
            }
        } else {
            // 入口兜底：托盘是常驻模式的默认入口，但它允许被用户禁用/加载失败。
            // 那种情况下自动打开设置页，否则下次启动既没有托盘、也找不到设置页。
            bool trayFound = false;
            bool trayLoaded = false;
            uint32_t trayState = 0;
            for (const Plugin::InstalledPlugin& plugin : runtime.manager().getInstalledPlugins()) {
                if (plugin.id == "tray.main") {
                    trayFound = true;
                    trayLoaded = plugin.loaded;
                    trayState = plugin.state;
                    break;
                }
            }
            if (!trayLoaded) {
                spdlog::info("tray.main 未加载（{}），自动发布 settings.open 作为入口兜底",
                             trayFound ? ("state=" + std::to_string(trayState)) : "未安装");
            }
            // --settings 与"托盘兜底"都只需要一次 settings.open；托盘正常时 --settings
            // 仍然照旧生效。
            if (settingsCommand || !trayLoaded) {
                if (Host::HostServices::eventBusPublish(nullptr, "settings.open", nullptr, 0) == 0) {
                    if (settingsCommand && trayLoaded) {
                        spdlog::info("Published settings.open (--settings)");
                    } else {
                        spdlog::info("Published settings.open (tray fallback)");
                    }
                } else {
                    spdlog::warn("Failed to publish settings.open");
                }
            }

            // 常驻主循环：事件分发 → 异步插件启停/配置落盘 → 阻塞等待下一条事件。
            // 退出由 app.quit 事件驱动：HostRuntime 订阅它并置 quitRequested()。
            while (!runtime.quitRequested()) {
                // 每轮先把事件总线里排队的消息分发给订阅者；回调跑在 main 线程上。
                Host::HostServices::dispatchEvents();
                // 然后执行异步的插件启停、把脏配置落盘：两者都只允许在 main 线程做。
                runtime.tick();
                while (!Host::g_eventQueue.empty()) {
                    Host::g_eventQueue.pop();
                }
                // app.quit 在上一轮 dispatch 里可能已经置了退出标志：先检查再阻塞，
                // 否则要白等一次兜底超时才退出。
                if (runtime.quitRequested()) {
                    break;
                }
                // 没有工作就阻塞等待：publish / post_to_host / requestMainThreadWork
                // 都会唤醒；1s 只是防漏唤醒的兜底，正常路径不再空转轮询。
                Host::HostServices::waitForActivity(1000);
            }
            spdlog::info("Quit requested, shutting down");
        }
    }  // 先卸载插件（deinit 里还会经宿主服务写日志），再关日志系统

    spdlog::shutdown();
    return exitCode;
}
