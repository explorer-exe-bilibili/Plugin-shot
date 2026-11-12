#include "config.h"

#include <exception>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace {

// 宿主内部配置键：插件禁用列表（JSON 字符串数组）与配置内容结构版本。
const char kConfigKeyDisabled[] = "plugins.disabled";
const char kConfigKeySchema[] = "config.schema";

// 当前配置内容的结构版本：1 = 0.1.0 初版（长截图随包分发，但不在默认禁用列表里）。
// 只表达"配置内容需要迁移到第几代"，与宿主版本号无关——补丁版本升级不该重复迁移。
constexpr int kConfigSchema = 2;

// 实验特性：默认禁用，用户在设置页勾选启用并重启宿主后生效。
const char kLongCapturePluginId[] = "long_capture.main";

/**
 * @brief 把长截图追加进 `plugins.disabled`（配置迁移到 schema 2）。
 *
 * 为什么必须迁移：已有用户的 `plugins.disabled` 里没有 `long_capture.main`，
 * 只改 setifno 的默认值救不了他们——键已存在时 setifno 不会写入。
 *
 * 解析失败（不是数组 / 不是合法 JSON）时**保持用户原值不动**：宁可这次禁不掉，
 * 也不能把用户手改的配置覆盖成默认值；schema 仍照写，避免下次启动重复迁移。
 *
 * @param[in,out] config 宿主配置单例（初始化阶段单线程调用）。
 */
void migrateLongCaptureDisabled(core::Config& config) {
    if (config.contains(kConfigKeyDisabled)) {
        const std::string text = config.get(kConfigKeyDisabled).toString();
        try {
            nlohmann::json parsed = nlohmann::json::parse(text);
            if (!parsed.is_array()) {
                spdlog::warn("{} 不是 JSON 数组，配置迁移跳过并保留原值", kConfigKeyDisabled);
            } else {
                bool found = false;
                for (const auto& item : parsed) {
                    if (item.is_string() && item.get<std::string>() == kLongCapturePluginId) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    parsed.push_back(kLongCapturePluginId);
                    config.set(kConfigKeyDisabled, core::ConfigItem(parsed.dump()));
                    spdlog::info("配置迁移：{} 已加入 {}", kLongCapturePluginId, kConfigKeyDisabled);
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("{} 解析失败（{}），配置迁移跳过并保留原值", kConfigKeyDisabled, e.what());
        }
    }
    config.set(kConfigKeySchema, core::ConfigItem(std::to_string(kConfigSchema)));
}

}  // namespace

void core::Config::setDefaultConfig() {
    // 默认值不在这里硬编码：每个配置项由它的提供者用 ShotConfigDesc::default_value 声明，
    // 宿主启动时由 HostListCatalog::applyStoredLocked 写进配置（见 document/LISTS.md）。
    // 这个钩子留给"必须在插件加载前就存在、又不属于任何提供者"的键。
    //
    // 示例处理器（反色 / 灰度）默认禁用：它们会改写每一张截图，首次运行不应该被示例改变结果。
    // 长截图（long_capture.main）是实验特性，同样默认在插件列表里禁用；用户可在设置页
    // 勾选启用，重启宿主后生效。这个键必须在插件加载前存在（PluginManager 用它决定加载
    // 哪些 DLL），所以不能等 config_list 的默认值回灌。
    setifno(kConfigKeyDisabled,
            ConfigItem(R"(["example.invert","example.grayscale","long_capture.main"])"));

    // 存量配置迁移：`config.schema` 缺失或小于当前版本时执行一次，成功后写回版本号。
    // 为什么不在 readConfigFile() 里做：读文件只负责读，默认值与迁移集中在 init 的
    // 这个钩子里，"读文件 → 补默认值 → 迁移 → 落盘"只有一个顺序，不会出现两处各自判断。
    const bool hasSchema = contains(kConfigKeySchema);
    const int schema = hasSchema ? get(kConfigKeySchema).toInt() : 0;
    if (!hasSchema || schema < kConfigSchema) {
        migrateLongCaptureDisabled(*this);
        // --capture 一次性模式不会走到 HostRuntime::tick()（配置统一落盘点），迁移必须
        // 当场落盘；否则每次启动 schema 都缺失，迁移反复执行，用户还是看不到禁用生效。
        if (!saveConfigFile()) {
            spdlog::warn("配置迁移已写入内存，但落盘失败；下次启动会重试");
        }
    }
}
