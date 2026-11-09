#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

#include <spdlog/spdlog.h>

#include "base.h"
#include "config.h"
#include "hostLogging.h"
#include "hostRuntime.h"
#include "hostServices.h"

namespace {

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
#ifdef _WIN32
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
            // --capture：一次性截屏，成功时打印最终路径，失败返回非 0。
            // 这条路径直接调流水线，不经过事件总线（与 capture.request 的回调等价）。
            std::string savedPath;
            if (runtime.capturePipeline().captureOnce(&savedPath)) {
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

            // 常驻主循环：事件分发 → 异步插件启停/配置落盘 → 让出 CPU。
            // 退出由 app.quit 事件驱动：HostRuntime 订阅它并置 quitRequested()。
            while (!runtime.quitRequested()) {
                // 每轮先把事件总线里排队的消息分发给订阅者；回调跑在 main 线程上。
                Host::HostServices::dispatchEvents();
                // 然后执行异步的插件启停、把脏配置落盘：两者都只允许在 main 线程做。
                runtime.tick();
                while (!Host::g_eventQueue.empty()) {
                    Host::g_eventQueue.pop();
                }
                // TODO: 主循环仍是 16ms 轮询；事件总线支持阻塞等待后改成真正的事件等待。
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            spdlog::info("Quit requested, shutting down");
        }
    }  // 先卸载插件（deinit 里还会经宿主服务写日志），再关日志系统

    spdlog::shutdown();
    return exitCode;
}
