#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "pluginManager.h"
#include "PluginInstances/ImageProcessor.h"
#include "PluginInstances/LibProvider.h"
#include "PluginInstances/UIProvider.h"

#include "hostServices.h"
namespace {

void printPixel(const char* tag, const ShotImage& image) {
    const std::uint8_t* p = image.data;
    std::cout << tag << " B=" << static_cast<int>(p[0])
              << " G=" << static_cast<int>(p[1])
              << " R=" << static_cast<int>(p[2])
              << " A=" << static_cast<int>(p[3]) << std::endl;
}

// 把插件声明的配置项列出来。
// 注意这里宿主完全没有硬编码任何配置项名字：它只是遍历插件给的 ShotConfigDesc。
void listConfig(Plugin::ImageProcessorInstance& processor) {
    const auto* cfg = static_cast<const ShotConfigurableV1*>(
        processor.queryInterface(SHOT_IFACE_CONFIGURABLE, 1));
    if (cfg == nullptr || !SHOT_HAS_FIELD(cfg, ShotConfigurableV1, get_config)) {
        std::cout << "    (no shot.configurable)" << std::endl;
        return;
    }

    void* self = processor.nativeHandle();
    const std::uint32_t count = cfg->get_config_count ? cfg->get_config_count(self) : 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        ShotConfigDesc desc{};
        desc.struct_size = sizeof(ShotConfigDesc);
        if (cfg->get_config_desc == nullptr || cfg->get_config_desc(self, i, &desc) != 0 ||
            desc.key == nullptr) {
            continue;
        }
        char value[128] = {};
        const std::int32_t len = cfg->get_config(self, desc.key, value, sizeof(value));
        std::cout << "    " << desc.key << " = " << (len >= 0 ? value : "<error>")
                  << "    (label: " << (desc.label ? desc.label : "-")
                  << ", default: " << (desc.default_value ? desc.default_value : "-")
                  << ")" << std::endl;
    }
}

// 演示宿主改一个配置项。不认识的 key 必须被插件安全忽略 —— 这里也顺带验证这一点。
void setConfig(Plugin::ImageProcessorInstance& processor, const char* key, const char* value) {
    const auto* cfg = static_cast<const ShotConfigurableV1*>(
        processor.queryInterface(SHOT_IFACE_CONFIGURABLE, 1));
    if (cfg == nullptr || !SHOT_HAS_FIELD(cfg, ShotConfigurableV1, set_config) ||
        cfg->set_config == nullptr) {
        return;
    }
    const std::int32_t rc = cfg->set_config(processor.nativeHandle(), key, value);
    std::cout << "  set " << processor.getName() << ": " << key << "=" << value
              << (rc == 0 ? "  (applied)" : "  (ignored: unknown key)") << std::endl;
}

}  // namespace

int main() {
    std::cout << "Starting plugin manager..." << std::endl;
    Plugin::PluginManager manager;

    const std::string pluginDir = "./plugins";
    if (!manager.loadPlugins(pluginDir)) {
        std::cerr << "Failed to load plugins from directory: " << pluginDir << std::endl;
        return 1;
    }

    auto processors =
        manager.getPluginsByType<Plugin::ImageProcessorInstance>(Plugin::PluginType::ImageProcessor);
    std::cout << "Loaded " << processors.size() << " image processor plugins." << std::endl;

    // 运行库提供者：不提供算法，只负责把某个库带进本进程（见 shot.lib_provider）。
    auto libProviders = manager.getPluginsByType<Plugin::LibProvider>(Plugin::PluginType::Lib);
    std::cout << "Loaded " << libProviders.size() << " lib provider plugins." << std::endl;
    for (auto& lib : libProviders) {
        const std::string version = lib->getLibraryVersion();
        std::cout << "  ~ " << lib->getName()
                  << (version.empty() ? "" : " " + version) << std::endl;
    }

    for (auto& processor : processors) {
        std::cout << "  config of " << processor->getName() << std::endl;
        listConfig(*processor);
    }

    // 改两个配置项：invert 不再保留 alpha；grayscale 只混 50% 的灰。
    // 每个插件都会收到两个 key，不认识的那个应被安全忽略。
    std::cout << "Applying config..." << std::endl;
    for (auto& processor : processors) {
        setConfig(*processor, "keep_alpha", "0");
        setConfig(*processor, "strength", "50");
    }

    const int w = 2, h = 2;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 255;  // B
        pixels[i + 1] = 0;    // G
        pixels[i + 2] = 0;    // R
        pixels[i + 3] = 255;  // A
    }

    ShotImage image{};
    image.struct_size = sizeof(ShotImage);
    image.pixel_format = SHOT_PIXEL_FORMAT_BGRA8;
    image.data = pixels.data();
    image.width = w;
    image.height = h;
    image.stride = w * 4;
    image.channels = 4;

    printPixel("Before processing:", image);
    for (auto& processor : processors) {
        if (!processor->process(image)) {
            std::cerr << "  " << processor->getName() << " reported failure" << std::endl;
        }
    }
    printPixel("After processing: ", image);
    auto uiProviders = manager.getPluginsByType<Plugin::UIProvider>(Plugin::PluginType::UIProvider);
    std::cout << "Loaded " << uiProviders.size() << " ui provider plugins." << std::endl;
    if (!uiProviders.empty()) {
        const char payload[] = "{\"dark_mode\":true}";
        ShotEvent ev{
            .struct_size = sizeof(ShotEvent),
            .name        = "setting.load",
            .data        = payload,
            .data_len    = sizeof(payload) - 1,
            .flags       = 0,
        };
        uiProviders[0]->postToPlugin(ev);
    }

    return 0;
}
