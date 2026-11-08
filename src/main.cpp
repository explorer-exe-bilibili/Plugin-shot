#include <filesystem>
#include <string>
#include <system_error>

#include <spdlog/spdlog.h>

#include "base.h"
#include "config.h"
#include "hostLogging.h"
#include "hostRuntime.h"
#include "hostServices.h"

int main() {
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

    // 配置文件在插件加载之前读：插件加载阶段要读 plugins.disabled，配置目录
    // 在 init 里还会把已存值灌回插件。
    core::Config::getInstance().init(core::dataPath + "/config.json");

    int exitCode = 0;
    {
        Host::HostRuntime runtime;
        if (!runtime.init("./plugins")) {
            spdlog::critical("Failed to initialize host runtime from ./plugins");
            exitCode = 1;
        } else {
            // 测试阶段宿主自己发布一次 settings.open，让设置窗口随程序启动打开。
            // 没有订阅者时发布只是空操作，不影响启动。
            // TODO: 等托盘/热键插件就位后改由它发布，宿主不再代发。
            if (Host::HostServices::eventBusPublish(nullptr, "settings.open", nullptr, 0) == 0) {
                spdlog::info("Published settings.open (temporary boot trigger)");
            } else {
                spdlog::warn("Failed to publish settings.open");
            }
            // TODO: 正式截图采集（全屏 / 区域 / 窗口）还没实现：宿主现在只加载插件。
            bool quit = false;
            // TODO: 目前没有"退出"事件来源，先空转轮询；等托盘/热键插件就位后改成
            // "等事件 → 处理 → 检查退出标志"，并处理 app.quit 事件。
            while (!quit) {
                // 每轮先把事件总线里排队的消息分发给订阅者；回调跑在 main 线程上。
                Host::HostServices::dispatchEvents();
                // 然后执行异步的插件启停、把脏配置落盘：两者都只允许在 main 线程做。
                runtime.tick();
                while (!Host::g_eventQueue.empty()) {
                    Host::g_eventQueue.pop();
                }
            }
        }
    }  // 先卸载插件（deinit 里还会经宿主服务写日志），再关日志系统

    spdlog::shutdown();
    return exitCode;
}
