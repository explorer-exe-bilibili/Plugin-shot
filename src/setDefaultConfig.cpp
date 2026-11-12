#include "config.h"

void core::Config::setDefaultConfig() {
    // 默认值不在这里硬编码：每个配置项由它的提供者用 ShotConfigDesc::default_value 声明，
    // 宿主启动时由 HostListCatalog::applyStoredLocked 写进配置（见 document/LISTS.md）。
    // 这个钩子留给"必须在插件加载前就存在、又不属于任何提供者"的键。
    //
    // 示例处理器（反色 / 灰度）默认禁用：它们会改写每一张截图，首次运行不应该被示例改变结果。
    // 这个键必须在插件加载前存在（PluginManager 用它决定加载哪些 DLL），所以不能等
    // config_list 的默认值回灌；用户仍可在设置页勾选启用。
    setifno("plugins.disabled", ConfigItem(R"(["example.invert","example.grayscale"])"));
}