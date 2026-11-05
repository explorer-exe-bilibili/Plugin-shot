#include "PluginManager.h"
#include "PluginInstances/ImageProcessor.h"
#include "PluginInstances/LibProvider.h"
#include "PluginInstances/UIProvider.h"
#include "plugin_meta.h"
#include "hostServices.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "version.h"  // CMake 从 include/version.h.in 生成：VERSION_MAJOR/MINOR/PATCH

using namespace Plugin;

struct PluginManager::Node{
    std::string dllPath;
    std::uint32_t index = 0;
    bool hasMeta = false;   // 读不到元数据 → 失败开放：最后加载、不做依赖检查
    bool opaque = false;    // 没有元数据 → 加载时登记整个包的所有索引
    PluginMetaEntry meta;

    bool skip = false;
    std::string skipReason;

    // 运行期事实（注册后填）
    std::string runtimeName;
    std::string runtimeVersion;
    std::string libName;
    std::string libVersion;
};

struct PluginManager::PreparedDll {
    std::shared_ptr<DllHandler> dll;
    const ShotModule* module = nullptr;
    bool failed = false;
    std::string reason;
};



std::string PluginManager::nodeLabel(const Node& node)  {
    return node.hasMeta ? node.meta.id : (node.dllPath + "#" + std::to_string(node.index));
}

// core（宿主自身）的版本：来自 CMake 生成的 version.h
ShotVersion hostCoreVersion() {
    return ShotVersion{VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH};
}

// 依赖检查：core 由宿主满足，其它名字由别的插件满足（它的 id，或它 provides 的名字）。
bool PluginManager::checkRequirements(const Node& node,
                       const std::map<std::string, std::size_t>& providers,
                       const std::vector<Node>& nodes,
                       std::string& reason) {
    const ShotVersion core = hostCoreVersion();
    for (const PluginRequirement& requirement : node.meta.requireList) {
        if(requirement.isBroken()) {
            reason = "依赖语法错误：" + requirement.getRaw();
            return false;
        }
        if (requirement.getName() == "core") {
            if(!requirement.satisfies("core", core)) {
                reason = "需要 " + requirement.getRaw() + "，宿主是 " + core.to_string();
                return false;
            }
            continue;
        }

        const auto it = providers.find(requirement.getName());
        if (it == providers.end()) {
            reason = "没有提供者满足 " + requirement.getRaw();
            return false;
        }
        const Node& provider = nodes[it->second];
        if (provider.skip) {
            reason = "提供者 " + requirement.getName() + " 被跳过了";
            return false;
        }

        // 版本一律取运行期事实：lib provider 报库版本，普通插件报自己的版本。
        const std::string& provided =
            provider.libVersion.empty() ? provider.runtimeVersion : provider.libVersion;
        ShotVersion providedVersion;
        if (!providedVersion.parse(provided)) {
            reason = "提供者 " + requirement.getName() + " 没有报告版本";
            return false;
        }
        if (!providedVersion.satisfies(requirement.getOp(), requirement.getVersion())) {
            reason = "要求 " + requirement.getRaw() + "，实际提供 " + provided;
            return false;
        }
    }
    return true;
}

PluginManager::~PluginManager() { unloadAll(); }

bool PluginManager::loadPlugins(const std::string& dir) {
    namespace fs = std::filesystem;
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        std::cerr << "Plugin directory does not exist or is not a directory: " << dir << std::endl;
        return false;
    }

    // ---- 阶段 1：扫目录，读每个 DLL 尾部的 metadata，建节点表 ----
    std::vector<std::string> dllPaths;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string ext = entry.path().extension().string();
#ifdef _WIN32
        const bool isPluginFile = (ext == ".dll");
#else
        const bool isPluginFile = (ext == ".so" || ext == ".dylib");
#endif
        if (isPluginFile) {
            dllPaths.push_back(entry.path().string());
        }
    }
    std::sort(dllPaths.begin(), dllPaths.end());  // 同样的输入得到同样的加载顺序

    std::vector<Node> nodes;
    for (const std::string& path : dllPaths) {
        PluginPackageMeta package;
        std::string error;
        if (!package.readFromFile(path, error)) {
            std::cout << "Note: " << path << " 没有可用的元数据（" << error
                      << "），按无依赖处理，最后加载" << std::endl;
            Node node;
            node.dllPath = path;
            node.opaque = true;
            nodes.push_back(std::move(node));
            continue;
        }
        for (std::uint32_t i = 0; i < package.getMetaData().size(); ++i) {
            Node node;
            node.dllPath = path;
            node.index = i;
            node.hasMeta = true;
            node.meta = package.getMetaData()[i];
            nodes.push_back(std::move(node));
        }
    }

    // ---- 阶段 2：建"提供者 → 节点"索引（插件 id 与它 provides 的名字都算） ----
    std::map<std::string, std::size_t> providers;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].hasMeta) {
            continue;
        }
        const auto add = [&](const std::string& name) {
            const auto it = providers.find(name);
            if (it == providers.end()) {
                providers[name] = i;
            } else if (it->second != i) {
                std::cerr << "Warning: 提供者 " << name << " 重复（" << nodeLabel(nodes[it->second])
                          << " 与 " << nodeLabel(nodes[i]) << "），用前者" << std::endl;
            }
        };
        add(nodes[i].meta.id);
        for (const std::string& provided : nodes[i].meta.provides) {
            add(provided);
        }
    }

    // ---- 阶段 3：拓扑排序（提供者先于依赖者）；无元数据的包排在最后 ----
    std::vector<std::size_t> order;
    std::vector<int> state(nodes.size(), 0);  // 0=未访问 1=访问中 2=完成
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
        if (state[index] != 0) {
            if (state[index] == 1) {
                std::cerr << "Warning: 依赖成环，涉及 " << nodeLabel(nodes[index])
                          << "，按发现顺序继续" << std::endl;
            }
            return;
        }
        state[index] = 1;
        if (nodes[index].hasMeta) {
            for (const PluginRequirement& requirement : nodes[index].meta.requireList) {
                const auto it = providers.find(requirement.getName());
                if (it != providers.end() && it->second != index) {
                    visit(it->second);
                }
            }
        }
        state[index] = 2;
        order.push_back(index);
    };
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].hasMeta) {
            visit(i);
        }
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].hasMeta) {
            order.push_back(i);
        }
    }

    // ---- 阶段 4：按顺序加载 ----
    std::map<std::string, PreparedDll> prepared;   // 每个 DLL 只准备一次
    std::map<std::string, std::uint32_t> loadedPerDll;

    // 载入并校验一个 DLL：唯一导出符号、长度/版本检查、模块级 init。
    std::function<bool(Node&)> prepare = [&](Node& node) -> bool {
        auto it = prepared.find(node.dllPath);
        if (it == prepared.end()) {
            PreparedDll entry;
            try {
                entry.dll = std::make_shared<DllHandler>(node.dllPath);
                m_dllCache[node.dllPath] = entry.dll;
            } catch (const std::exception& e) {
                std::cerr << "Failed to load DLL: " << node.dllPath << ", error: " << e.what()
                          << std::endl;
                entry.failed = true;
                entry.reason = "DLL 加载失败";
            }

            if (!entry.failed) {
                auto entryFn = reinterpret_cast<const ShotModule* (*)()>(
                    entry.dll->tryGetFunction("shot_module_entry"));
                const ShotModule* module = entryFn != nullptr ? entryFn() : nullptr;
                if (entryFn == nullptr) {
                    entry.failed = true;
                    entry.reason = "没有 shot_module_entry 导出";
                } else if (module == nullptr) {
                    entry.failed = true;
                    entry.reason = "shot_module_entry 返回空";
                } else if (!SHOT_HAS_FIELD(module, ShotModule, plugin_count) ||
                           !SHOT_HAS_FIELD(module, ShotModule, query_interface)) {
                    entry.failed = true;
                    entry.reason = "模块表太短（" + std::to_string(module->struct_size) + " 字节）";
                } else if (module->abi_major != SHOT_ABI_MAJOR) {
                    entry.failed = true;
                    entry.reason = "abi major 不兼容（插件 " + std::to_string(module->abi_major) +
                                   "，宿主 " + std::to_string(SHOT_ABI_MAJOR) + "）";
                } else if (module->create == nullptr || module->destroy == nullptr) {
                    entry.failed = true;
                    entry.reason = "缺少 create/destroy";
                } else {
                    if (module->abi_minor > SHOT_ABI_MINOR) {
                        std::cout << "Note: " << node.dllPath << " targets abi " << module->abi_major
                                  << "." << module->abi_minor << " but host supports "
                                  << SHOT_ABI_MAJOR << "." << SHOT_ABI_MINOR
                                  << "; newer capabilities will be unavailable." << std::endl;
                    }
                    if (module->init != nullptr) {
                        const int32_t rc = module->init(&Host::g_hostServices, 0);
                        if (rc != 0) {
                            entry.failed = true;
                            entry.reason = "init 拒绝了本宿主（" + std::to_string(rc) + "）";
                        }
                    }
                    if (!entry.failed) {
                        entry.module = module;
                        m_modules.emplace_back(entry.dll, module);
                    }
                }
            }
            it = prepared.emplace(node.dllPath, std::move(entry)).first;
        }

        if (it->second.failed) {
            node.skip = true;
            node.skipReason = it->second.reason;
            return false;
        }
        return true;
    };

    // 创建第 index 个插件，按能力登记。成功时填 node.runtimeName。
    std::function<bool(Node&, PreparedDll&)> registerOne = [&](Node& node, PreparedDll& dll) -> bool {
        const ShotModule* module = dll.module;
        void* instance = module->create(node.index, &Host::g_hostServices);
        if (instance == nullptr) {
            node.skipReason = "create 返回空";
            return false;
        }

        std::string displayName = "plugin#" + std::to_string(node.index);
        if (module->get_info != nullptr) {
            ShotPluginInfo info{};
            info.struct_size = sizeof(ShotPluginInfo);
            if (module->get_info(node.index, &info) == 0) {
                if (info.name != nullptr) {
                    displayName = info.name;
                }
                if (info.version != nullptr) {
                    node.runtimeVersion = info.version;
                }
                // 元数据只是声明，get_info 才是权威：不一致只警告，仍按 get_info 的值走。
                if (node.hasMeta) {
                    const auto warn = [&](const char* field, const std::string& declared,
                                          const char* actual) {
                        std::cerr << "Warning: " << node.dllPath << " #" << node.index
                                  << " 元数据与 get_info 不一致：" << field << " 声明 \""
                                  << declared << "\"，实际 \"" << actual
                                  << "\"（以 get_info 为准）" << std::endl;
                    };
                    if (info.id != nullptr && node.meta.id != info.id) {
                        warn("id", node.meta.id, info.id);
                    }
                    if (info.name != nullptr && !node.meta.name.empty() &&
                        node.meta.name != info.name) {
                        warn("name", node.meta.name, info.name);
                    }
                    if (info.version != nullptr && !node.meta.version.empty() &&
                        node.meta.version != info.version) {
                        warn("version", node.meta.version, info.version);
                    }
                }
            }
        }

        void* (*const query)(void*, const char*, std::uint32_t) = module->query_interface;

        const auto* imageProcessorOps = (query == nullptr)
            ? nullptr
            : static_cast<const ShotImageProcessorV1*>(
                  query(instance, SHOT_IFACE_IMAGE_PROCESSOR, 1));
        if (imageProcessorOps != nullptr && SHOT_HAS_FIELD(imageProcessorOps, ShotImageProcessorV1, process) &&
            imageProcessorOps->process != nullptr) {
            m_plugins[PluginType::ImageProcessor].push_back(std::make_shared<ImageProcessorInstance>(
                dll.dll, module, instance, imageProcessorOps, displayName));
            node.runtimeName = displayName;
            return true;
        }

        const auto* libOps = (query == nullptr)
            ? nullptr
            : static_cast<const ShotLibProviderV1*>(query(instance, SHOT_IFACE_LIB_PROVIDER, 1));
        if (libOps != nullptr && SHOT_HAS_FIELD(libOps, ShotLibProviderV1, get_library_name) &&
            libOps->get_library_name != nullptr) {
            const char* libName = libOps->get_library_name(instance);
            if (libName != nullptr) {
                node.libName = libName;
                if (SHOT_HAS_FIELD(libOps, ShotLibProviderV1, get_library_version) &&
                    libOps->get_library_version != nullptr) {
                    const char* version = libOps->get_library_version(instance);
                    if (version != nullptr) {
                        node.libVersion = version;
                    }
                }
                m_plugins[PluginType::Lib].push_back(std::make_shared<LibProvider>(
                    dll.dll, module, instance, displayName, libOps));
                node.runtimeName = displayName;
                return true;
            }
        }

        const auto* uiOps = (query == nullptr)
            ? nullptr
            : static_cast<const ShotUIProviderV1*>(query(instance, SHOT_IFACE_UI_PROVIDER, 1));
        if (uiOps != nullptr && SHOT_HAS_FIELD(uiOps, ShotUIProviderV1, post_to_plugin) &&
            uiOps->post_to_plugin != nullptr) {
            auto provider = std::make_shared<UIProvider>(dll.dll, module, instance, uiOps, displayName);
            if (!provider->init(&Host::g_hostServices)) {
                node.skipReason = "UIProvider::init 失败";
                return false;          // 由 provider 析构触发 module->destroy
            }
            m_plugins[PluginType::UIProvider].push_back(provider);
            node.runtimeName = displayName;
            return true;
        }

        module->destroy(instance);
        node.skipReason = "没有本宿主认识的任何能力";
        return false;
    };

    for (const std::size_t index : order) {
        Node& node = nodes[index];
        if (!prepare(node)) {
            std::cerr << "Failed to load plugin module: " << node.dllPath << "（" << node.skipReason
                      << "）" << std::endl;
            continue;
        }
        PreparedDll& dll = prepared[node.dllPath];

        if (node.opaque) {
            // 没有元数据：把整个包登记一遍，不做依赖检查（失败开放）。
            const std::uint32_t count = dll.module->plugin_count;
            for (std::uint32_t i = 0; i < count; ++i) {
                node.index = i;
                if (registerOne(node, dll)) {
                    std::cout << "  + " << node.runtimeName << std::endl;
                    ++loadedPerDll[node.dllPath];
                }
            }
            continue;
        }

        std::string reason;
        if (!checkRequirements(node, providers, nodes, reason)) {
            node.skip = true;
            node.skipReason = reason;
            std::cerr << "  - " << nodeLabel(node) << " 跳过：" << reason << std::endl;
            continue;
        }

        if (registerOne(node, dll)) {
            std::cout << "  + " << node.runtimeName << std::endl;
            ++loadedPerDll[node.dllPath];
        } else {
            node.skip = true;
            std::cerr << "  - " << nodeLabel(node) << " 跳过：" << node.skipReason << std::endl;
        }
    }

    // ---- 阶段 5：每个 DLL 一行汇总 ----
    for (const auto& [path, count] : loadedPerDll) {
        std::cout << "Successfully loaded module: " << path << " (" << count << " plugins, abi "
                  << SHOT_ABI_MAJOR << "." << SHOT_ABI_MINOR << ")" << std::endl;
    }
    for (const auto& [path, entry] : prepared) {
        if (!entry.failed && loadedPerDll.find(path) == loadedPerDll.end()) {
            std::cout << "Module loaded but no plugin was usable: " << path << std::endl;
        }
    }
    return true;
}

void PluginManager::unloadAll() {
    // 顺序有讲究：先让插件实例走完自己的 destroy，再 shutdown 模块，最后才放手 DLL。
    if(auto it=m_plugins.find(PluginType::UIProvider); it != m_plugins.end()){
        for(auto& plugin : it->second){
            if(auto uiProvider = std::dynamic_pointer_cast<UIProvider>(plugin)){
                uiProvider->postToPlugin(ShotEvent{sizeof(ShotEvent), "host.shutdown", nullptr, 0, 0});
                uiProvider->deinit();  // 先 deinit，后析构
            }
        }
    }
    m_plugins.clear();
    for (auto& entry : m_modules) {
        const ShotModule* module = entry.second;
        if (module != nullptr && module->shutdown != nullptr) {
            module->shutdown();
        }
    }
    m_modules.clear();
    m_dllCache.clear();
}
