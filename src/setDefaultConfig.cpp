#include "config.h"

void core::Config::setDefaultConfig() {
    // 默认值不在这里硬编码：每个配置项由它的提供者用 ShotConfigDesc::default_value 声明，
    // 宿主启动时由 HostConfigCatalog::apply_stored 写进配置（见 document/SETTINGS.md §2）。
    // 这个钩子留给"必须在插件加载前就存在、又不属于任何提供者"的键；目前没有这种键。
}