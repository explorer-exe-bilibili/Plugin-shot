#include "pluginManager.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "PluginInstances/ImageProcessor.h"
#include "PluginInstances/LibProvider.h"
#include "PluginInstances/UIProvider.h"
#include "config.h"
#include "hostServices.h"
#include "version.h"  // CMake 从 include/version.h.in 生成：VERSION_MAJOR/MINOR/PATCH

namespace Plugin {

namespace {

// 宿主配置里保存禁用列表的键：值是 JSON 字符串数组（如 ["example.invert"]）。
const char kConfigKeyDisabled[] = "plugins.disabled";

// 关闭 UI 插件前投给它的事件名：与 document/SETTINGS.md §3.1 的约定一致。
const char kShutdownEventName[] = "host.shutdown";

/**
 * @brief 构造发给 UI 插件的 "host.shutdown" 事件。
 * @return 填好 struct_size 的事件；data 为空。
 */
ShotEvent makeShutdownEvent() {
    return ShotEvent{
        .struct_size = sizeof(ShotEvent),
        .name        = kShutdownEventName,
        .data        = nullptr,
        .data_len    = 0,
        .flags       = 0,
    };
}

}  // namespace

std::string PluginManager::fallbackId(const std::string& dllPath, uint32_t index) {
    const std::string filename = std::filesystem::path(dllPath).filename().string();
    return filename + "#" + std::to_string(index);
}

std::vector<std::string> PluginManager::readDisabledList() {
    std::vector<std::string> disabled;
    core::Config& config = core::Config::getInstance();
    // contains() 只查主实例：缺键时不要走 get()，否则它会回落默认实例并打日志。
    if (!config.contains(kConfigKeyDisabled)) {
        return disabled;
    }
    const std::string text = config.get(kConfigKeyDisabled).toString();
    if (text.empty()) {
        return disabled;
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(text);
        if (!parsed.is_array()) {
            spdlog::warn("{} 不是 JSON 数组，按空列表处理", kConfigKeyDisabled);
            return disabled;
        }
        for (const auto& item : parsed) {
            if (item.is_string()) {
                disabled.push_back(item.get<std::string>());
            }
        }
    } catch (const std::exception& e) {
        // 配置被手改坏不是致命错误：按"没有禁用任何插件"继续，下一次落盘会覆盖它。
        spdlog::warn("{} 解析失败（{}），按空列表处理", kConfigKeyDisabled, e.what());
        disabled.clear();
    }
    return disabled;
}

bool PluginManager::resolveModule(const std::shared_ptr<DllHandler>& dll, const ShotModule*& module,
                                  std::string& reason) {
    const auto entryFn = reinterpret_cast<const ShotModule* (*)()>(
        dll->tryGetFunction("shot_module_entry"));
    if (entryFn == nullptr) {
        reason = "没有 shot_module_entry 导出";
        return false;
    }
    const ShotModule* loaded = entryFn();
    if (loaded == nullptr) {
        reason = "shot_module_entry 返回空";
        return false;
    }
    if (!SHOT_HAS_FIELD(loaded, ShotModule, plugin_count) ||
        !SHOT_HAS_FIELD(loaded, ShotModule, query_interface)) {
        reason = "模块表太短（" + std::to_string(loaded->struct_size) + " 字节）";
        return false;
    }
    if (loaded->abi_major != SHOT_ABI_MAJOR) {
        reason = "abi major 不兼容（插件 " + std::to_string(loaded->abi_major) + "，宿主 " +
                 std::to_string(SHOT_ABI_MAJOR) + "）";
        return false;
    }
    if (loaded->create == nullptr || loaded->destroy == nullptr) {
        reason = "缺少 create/destroy";
        return false;
    }
    module = loaded;
    return true;
}

bool PluginManager::initModule(const std::string& dllPath, const ShotModule* module,
                               std::string& reason) {
    // 新插件 + 老宿主是允许的组合：只提示，不拒绝，插件自己按 abi_minor 降级。
    if (module->abi_minor > SHOT_ABI_MINOR) {
        spdlog::info("Note: {} targets abi {}.{} but host supports {}.{}; "
                     "newer capabilities will be unavailable.",
                     dllPath, module->abi_major, module->abi_minor, SHOT_ABI_MAJOR, SHOT_ABI_MINOR);
    }
    if (module->init != nullptr) {
        const int32_t rc = module->init(&Host::g_hostServices, 0);
        if (rc != 0) {
            reason = "init 拒绝了本宿主（" + std::to_string(rc) + "）";
            return false;
        }
    }
    return true;
}

bool PluginManager::fetchIdentity(const ShotModule* module, uint32_t index, ShotPluginInfo& out) {
    if (module == nullptr || module->get_info == nullptr) {
        return false;
    }
    out = ShotPluginInfo{};
    out.struct_size = sizeof(ShotPluginInfo);
    if (module->get_info(index, &out) != 0) {
        return false;
    }
    return out.id != nullptr;
}

ShotVersion PluginManager::hostCoreVersion() {
    return ShotVersion{VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH};
}

bool PluginManager::checkRequirements(const Record& record,
                                      const std::map<std::string, std::size_t>& providers,
                                      const std::vector<Record>& records,
                                      std::string& reason) {
    const ShotVersion core = hostCoreVersion();
    for (const PluginRequirement& requirement : record.meta.requireList) {
        if (requirement.isBroken()) {
            reason = "依赖语法错误：" + requirement.getRaw();
            return false;
        }
        if (requirement.getName() == "core") {
            if (!requirement.satisfies("core", core)) {
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
        const Record& provider = records[it->second];
        if (provider.info.state != SHOT_PLUGIN_STATE_LOADED) {
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

PluginManager::~PluginManager() {
    unloadAll();
}

bool PluginManager::acquireModule(const std::string& dllPath, std::shared_ptr<DllHandler>& dll,
                                  const ShotModule*& module, std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_modules.find(dllPath);
        if (it != m_modules.end()) {
            dll = it->second.dll;
            module = it->second.module;
            return true;
        }
    }

    // LoadLibrary / 校验 / module->init 都是插件代码或系统调用：必须在数据锁外做，
    // 否则插件 init 里回调宿主服务（查配置目录等）会和这把锁互相等待。
    std::shared_ptr<DllHandler> handler;
    try {
        handler = std::make_shared<DllHandler>(dllPath);
    } catch (const std::exception& e) {
        reason = std::string("DLL 加载失败（") + e.what() + "）";
        return false;
    }

    const ShotModule* loaded = nullptr;
    if (!resolveModule(handler, loaded, reason)) {
        return false;
    }
    if (!initModule(dllPath, loaded, reason)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 热加载与启动加载都在 main 线程串行执行，这里再查一次只是防御性写法。
        const auto it = m_modules.find(dllPath);
        if (it != m_modules.end()) {
            dll = it->second.dll;
            module = it->second.module;
            return true;
        }
        ModuleEntry entry;
        entry.dll = handler;
        entry.module = loaded;
        entry.instanceCount = 0;
        m_modules.emplace(dllPath, std::move(entry));
    }
    dll = std::move(handler);
    module = loaded;
    return true;
}

PluginManager::CreatedPlugin PluginManager::createPlugin(const std::shared_ptr<DllHandler>& dll,
                                                         const ShotModule* module, uint32_t index,
                                                         const std::string& id,
                                                         const std::string& name,
                                                         const std::string& version) {
    CreatedPlugin created;
    void* instance = module->create(index, &Host::g_hostServices);
    if (instance == nullptr) {
        created.reason = "create 返回空";
        return created;
    }

    void* (*const query)(void*, const char*, std::uint32_t) = module->query_interface;
    const std::string displayName = name.empty() ? ("plugin#" + std::to_string(index)) : name;

    const auto* imageProcessorOps = (query == nullptr)
        ? nullptr
        : static_cast<const ShotImageProcessorV1*>(
              query(instance, SHOT_IFACE_IMAGE_PROCESSOR, 1));
    if (imageProcessorOps != nullptr &&
        SHOT_HAS_FIELD(imageProcessorOps, ShotImageProcessorV1, process) &&
        imageProcessorOps->process != nullptr) {
        created.instance = std::make_shared<ImageProcessorInstance>(
            dll, module, instance, id, displayName, version, imageProcessorOps);
        return created;
    }

    const auto* libOps = (query == nullptr)
        ? nullptr
        : static_cast<const ShotLibProviderV1*>(query(instance, SHOT_IFACE_LIB_PROVIDER, 1));
    if (libOps != nullptr && SHOT_HAS_FIELD(libOps, ShotLibProviderV1, get_library_name) &&
        libOps->get_library_name != nullptr) {
        const char* libName = libOps->get_library_name(instance);
        if (libName != nullptr) {
            created.libName = libName;
            if (SHOT_HAS_FIELD(libOps, ShotLibProviderV1, get_library_version) &&
                libOps->get_library_version != nullptr) {
                const char* libVersion = libOps->get_library_version(instance);
                if (libVersion != nullptr) {
                    created.libVersion = libVersion;
                }
            }
            created.instance = std::make_shared<LibProvider>(
                dll, module, instance, id, displayName, version, libOps);
            return created;
        }
    }

    const auto* uiOps = (query == nullptr)
        ? nullptr
        : static_cast<const ShotUIProviderV1*>(query(instance, SHOT_IFACE_UI_PROVIDER, 1));
    if (uiOps != nullptr && SHOT_HAS_FIELD(uiOps, ShotUIProviderV1, post_to_plugin) &&
        uiOps->post_to_plugin != nullptr) {
        auto provider = std::make_shared<UIProvider>(dll, module, instance, id, displayName,
                                                     version, uiOps);
        if (!provider->init(&Host::g_hostServices)) {
            created.reason = "UIProvider::init 失败";
            return created;  // provider 析构会触发 module->destroy，不泄漏实例
        }
        created.instance = std::move(provider);
        return created;
    }

    // 认不出任何能力：必须自己把实例还给插件销毁，宿主绝不 delete 插件对象。
    module->destroy(instance);
    created.reason = "没有本宿主认识的任何能力";
    return created;
}

bool PluginManager::loadPlugins(const std::string& dir) {
    namespace fs = std::filesystem;
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        spdlog::error("Plugin directory does not exist or is not a directory: {}", dir);
        return false;
    }

    // ---- 阶段 0：读禁用列表。缺键 = 没有插件被禁用，不是错误。 ----
    const std::vector<std::string> disabled = readDisabledList();
    const auto isDisabled = [&disabled](const std::string& id) {
        return std::find(disabled.begin(), disabled.end(), id) != disabled.end();
    };

    // ---- 阶段 1：扫目录，读每个 DLL 尾部的 metadata，建记录表 ----
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

    std::vector<Record> records;
    std::map<std::string, PluginPackageMeta> packageMeta;
    for (const std::string& path : dllPaths) {
        PluginPackageMeta package;
        std::string error;
        if (!package.readFromFile(path, error)) {
            spdlog::info("Note: {} 没有可用的元数据（{}），按无依赖处理，最后加载", path, error);
            Record record;
            record.info.dllPath = path;
            record.info.id = fallbackId(path, 0);
            record.info.name = record.info.id;
            record.info.reason = "未加载";
            record.info.state = SHOT_PLUGIN_STATE_FAILED;
            record.opaque = true;
            record.placeholder = true;
            records.push_back(std::move(record));
            continue;
        }
        // 元数据顺手缓存起来：以后热加载单个包时直接复用，不用再读一遍文件尾。
        packageMeta[path] = package;
        const auto& entries = package.getMetaData();
        for (std::uint32_t i = 0; i < entries.size(); ++i) {
            Record record;
            record.info.dllPath = path;
            record.info.id = entries[i].id;
            record.info.name = entries[i].name.empty() ? entries[i].id : entries[i].name;
            record.info.version = entries[i].version;
            record.info.reason = "未加载";
            record.info.state = SHOT_PLUGIN_STATE_FAILED;
            record.index = i;
            record.hasMeta = true;
            record.meta = entries[i];
            record.runtimeName = record.info.name;
            record.runtimeVersion = record.info.version;
            records.push_back(std::move(record));
        }
    }

    // ---- 阶段 2：建"提供者 → 记录"索引（插件 id 与它 provides 的名字都算） ----
    std::map<std::string, std::size_t> providers;
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (!records[i].hasMeta) {
            continue;
        }
        const auto add = [&](const std::string& name) {
            const auto it = providers.find(name);
            if (it == providers.end()) {
                providers[name] = i;
            } else if (it->second != i) {
                spdlog::warn("提供者 {} 重复（{} 与 {}），用前者", name,
                             records[it->second].info.id, records[i].info.id);
            }
        };
        add(records[i].info.id);
        for (const std::string& provided : records[i].meta.provides) {
            add(provided);
        }
    }

    // ---- 阶段 3：拓扑排序（提供者先于依赖者）；无元数据的包排在最后 ----
    std::vector<std::size_t> order;
    std::vector<int> state(records.size(), 0);  // 0=未访问 1=访问中 2=完成
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
        if (state[index] != 0) {
            if (state[index] == 1) {
                spdlog::warn("依赖成环，涉及 {}，按发现顺序继续", records[index].info.id);
            }
            return;
        }
        state[index] = 1;
        if (records[index].hasMeta) {
            for (const PluginRequirement& requirement : records[index].meta.requireList) {
                const auto it = providers.find(requirement.getName());
                if (it != providers.end() && it->second != index) {
                    visit(it->second);
                }
            }
        }
        state[index] = 2;
        order.push_back(index);
    };
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].hasMeta) {
            visit(i);
        }
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (!records[i].hasMeta) {
            order.push_back(i);
        }
    }

    // ---- 阶段 4：按顺序加载 ----
    std::map<std::string, std::string> moduleFailures;  // 同一个包的多条记录只尝试一次

    for (const std::size_t recordIndex : order) {
        // 先处理无元数据的包：没有声明，失败开放——把模块表里每个索引都登记一遍。
        if (records[recordIndex].opaque) {
            const std::string dllPath = records[recordIndex].info.dllPath;
            std::shared_ptr<DllHandler> dll;
            const ShotModule* module = nullptr;
            std::string reason;
            if (!acquireModule(dllPath, dll, module, reason)) {
                // 整个包装不上：占位记录转成真实失败记录，让注册表能解释原因。
                records[recordIndex].placeholder = false;
                records[recordIndex].info.state = SHOT_PLUGIN_STATE_FAILED;
                records[recordIndex].info.reason = reason;
                spdlog::warn("Failed to load plugin module: {}（{}）", dllPath, reason);
                continue;
            }
            const std::uint32_t pluginCount = module->plugin_count;
            for (std::uint32_t i = 0; i < pluginCount; ++i) {
                Record created;
                created.info.dllPath = dllPath;
                created.info.id = fallbackId(dllPath, i);
                created.info.name = created.info.id;
                created.info.state = SHOT_PLUGIN_STATE_FAILED;
                created.info.reason = "未加载";
                created.index = i;
                created.opaque = true;
                ShotPluginInfo info{};
                if (fetchIdentity(module, i, info)) {
                    created.info.id = info.id;
                    created.info.name = (info.name != nullptr) ? info.name : info.id;
                    if (info.version != nullptr) {
                        created.info.version = info.version;
                    }
                    created.runtimeName = created.info.name;
                    created.runtimeVersion = created.info.version;
                }
                if (isDisabled(created.info.id)) {
                    created.info.state = SHOT_PLUGIN_STATE_DISABLED;
                    created.info.reason = "已在配置中禁用";
                    records.push_back(std::move(created));
                    continue;
                }
                CreatedPlugin plugin = createPlugin(dll, module, i, created.info.id,
                                                    created.info.name, created.info.version);
                if (plugin.instance == nullptr) {
                    created.info.state = SHOT_PLUGIN_STATE_FAILED;
                    created.info.reason = plugin.reason;
                    spdlog::warn("  - {} 跳过：{}", created.info.id, plugin.reason);
                    records.push_back(std::move(created));
                    continue;
                }
                created.info.state = SHOT_PLUGIN_STATE_LOADED;
                created.info.loaded = true;
                created.info.reason.clear();
                created.libName = plugin.libName;
                created.libVersion = plugin.libVersion;
                const std::string displayName = plugin.instance->getName();
                records.push_back(std::move(created));
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    registerInstanceLocked(dllPath, plugin.instance);
                }
                spdlog::info("  + {}", displayName);
            }
            continue;
        }

        Record& record = records[recordIndex];
        const std::string dllPath = record.info.dllPath;

        // 禁用检查（一）：按 metadata 的 id。命中就不创建实例，连 DLL 都不用装。
        if (isDisabled(record.info.id)) {
            record.info.state = SHOT_PLUGIN_STATE_DISABLED;
            record.info.reason = "已在配置中禁用";
            spdlog::info("  - {} 已禁用，跳过", record.info.id);
            continue;
        }

        const auto failedModule = moduleFailures.find(dllPath);
        if (failedModule != moduleFailures.end()) {
            record.info.state = SHOT_PLUGIN_STATE_FAILED;
            record.info.reason = failedModule->second;
            spdlog::warn("Failed to load plugin module: {}（{}）", dllPath, record.info.reason);
            continue;
        }

        std::shared_ptr<DllHandler> dll;
        const ShotModule* module = nullptr;
        std::string reason;
        if (!acquireModule(dllPath, dll, module, reason)) {
            moduleFailures[dllPath] = reason;
            record.info.state = SHOT_PLUGIN_STATE_FAILED;
            record.info.reason = reason;
            spdlog::warn("Failed to load plugin module: {}（{}）", dllPath, reason);
            continue;
        }

        // get_info 是运行期权威：两者不一致时只警告，仍按 get_info 的值走。
        ShotPluginInfo info{};
        if (fetchIdentity(module, record.index, info)) {
            if (info.id != nullptr) {
                record.info.id = info.id;
            }
            if (info.name != nullptr) {
                record.info.name = info.name;
            }
            if (info.version != nullptr) {
                record.info.version = info.version;
            }
            record.runtimeName = record.info.name;
            record.runtimeVersion = record.info.version;
            if (record.hasMeta) {
                const auto warnMismatch = [&](const char* field, const std::string& declared,
                                              const char* actual) {
                    spdlog::warn("{} #{} 元数据与 get_info 不一致：{} 声明 \"{}\"，实际 \"{}\""
                                 "（以 get_info 为准）",
                                 dllPath, record.index, field, declared, actual);
                };
                if (record.meta.id != info.id) {
                    warnMismatch("id", record.meta.id, info.id);
                }
                if (info.name != nullptr && !record.meta.name.empty() &&
                    record.meta.name != info.name) {
                    warnMismatch("name", record.meta.name, info.name);
                }
                if (info.version != nullptr && !record.meta.version.empty() &&
                    record.meta.version != info.version) {
                    warnMismatch("version", record.meta.version, info.version);
                }
            }
        }

        // 禁用检查（二）：metadata 写错时以 get_info 为准，所以运行期 id 也要查一次。
        if (isDisabled(record.info.id)) {
            record.info.state = SHOT_PLUGIN_STATE_DISABLED;
            record.info.reason = "已在配置中禁用";
            spdlog::info("  - {} 已禁用，跳过", record.info.id);
            continue;
        }

        if (!checkRequirements(record, providers, records, reason)) {
            record.info.state = SHOT_PLUGIN_STATE_FAILED;
            record.info.reason = reason;
            spdlog::warn("  - {} 跳过：{}", record.info.id, reason);
            continue;
        }

        CreatedPlugin plugin = createPlugin(dll, module, record.index, record.info.id,
                                            record.info.name, record.info.version);
        if (plugin.instance == nullptr) {
            record.info.state = SHOT_PLUGIN_STATE_FAILED;
            record.info.reason = plugin.reason;
            spdlog::warn("  - {} 跳过：{}", record.info.id, plugin.reason);
            continue;
        }
        record.libName = plugin.libName;
        record.libVersion = plugin.libVersion;
        record.info.state = SHOT_PLUGIN_STATE_LOADED;
        record.info.loaded = true;
        record.info.reason.clear();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            registerInstanceLocked(dllPath, plugin.instance);
        }
        spdlog::info("  + {}", record.info.name);
    }

    // ---- 阶段 5：每个 DLL 一行汇总 ----
    std::vector<std::pair<std::string, std::uint32_t>> moduleSummaries;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [path, entry] : m_modules) {
            moduleSummaries.emplace_back(path, static_cast<std::uint32_t>(entry.instanceCount));
        }
    }
    for (const auto& [path, count] : moduleSummaries) {
        if (count > 0) {
            spdlog::info("Successfully loaded module: {} ({} plugins, abi {}.{})", path, count,
                         SHOT_ABI_MAJOR, SHOT_ABI_MINOR);
        } else {
            spdlog::info("Module loaded but no plugin was usable: {}", path);
        }
    }
    // 整理最终记录：无元数据包的占位记录不对外暴露。
    std::vector<Record> finalRecords;
    finalRecords.reserve(records.size());
    for (Record& record : records) {
        if (!record.placeholder) {
            finalRecords.push_back(std::move(record));
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_packageMeta = std::move(packageMeta);
        m_records = std::move(finalRecords);
        m_recordIds.clear();
        for (std::size_t i = 0; i < m_records.size(); ++i) {
            const std::string& id = m_records[i].info.id;
            if (m_recordIds.find(id) != m_recordIds.end()) {
                spdlog::warn("插件 id 重复：{}，注册表只用第一条", id);
                continue;
            }
            m_recordIds[id] = i;
        }
        m_disabled = disabled;
    }
    // 第一次运行还没有这个键：写一次空数组，让配置文件从一开始就是完整的。
    persistDisabledList();
    return true;
}

void PluginManager::unloadAll() {
    std::vector<std::shared_ptr<PluginBase>> instances;
    std::map<std::string, ModuleEntry> modules;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        instances = std::move(m_instances);
        m_instances.clear();
        m_plugins.clear();
        modules = std::move(m_modules);
        m_modules.clear();
        m_records.clear();
        m_recordIds.clear();
    }
    // UI 插件先收 shutdown 通知（与旧的卸载顺序一致），返回前必须自己停线程、销窗口。
    for (const auto& instance : instances) {
        if (const auto uiProvider = std::dynamic_pointer_cast<UIProvider>(instance)) {
            uiProvider->postToPlugin(makeShutdownEvent());
            uiProvider->deinit();
        }
    }
    // 顺序有讲究：先让实例走完自己的 destroy，再 shutdown 模块，最后才放手 DLL。
    instances.clear();
    for (auto& [path, entry] : modules) {
        (void)path;
        if (entry.module != nullptr && entry.module->shutdown != nullptr) {
            entry.module->shutdown();
        }
    }
    modules.clear();
}

bool PluginManager::setPluginEnabled(const std::string& id, bool enabled, std::string& reason) {
    reason.clear();
    if (id.empty()) {
        reason = "插件 id 为空";
        return false;
    }

    if (!enabled) {
        // ---- 禁用：依赖检查 → deinit → 销毁实例 → 包内没实例了才卸载 DLL ----
        std::shared_ptr<PluginBase> instance;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Record* record = findRecordLocked(id);
            if (record == nullptr) {
                reason = "没有这个插件";
                return false;
            }
            if (record->info.state == SHOT_PLUGIN_STATE_DISABLED) {
                return true;  // 已经是禁用状态：幂等
            }
            if (record->info.state == SHOT_PLUGIN_STATE_LOADED) {
                // 被依赖的插件不允许单独禁用（v1 直接拒绝，不做级联禁用）。
                const std::string dependent = findDependentLocked(*record);
                if (!dependent.empty()) {
                    reason = "被 " + dependent + " 依赖";
                    return false;
                }
                instance = findLoadedInstanceLocked(id);
            }
        }

        if (instance != nullptr) {
            // 与 unloadAll 顺序一致：先投 shutdown 通知，再 deinit（插件停线程、销窗口）。
            // 这里故意在生命周期锁之外 deinit：UI 插件的窗口线程可能在等配置目录的
            // 生命周期锁，持锁等它退出会互相等待。SETTINGS.md 也约定入口 UI 插件不启停。
            if (const auto uiProvider = std::dynamic_pointer_cast<UIProvider>(instance)) {
                uiProvider->postToPlugin(makeShutdownEvent());
                if (!uiProvider->deinit()) {
                    spdlog::warn("Plugin {} deinit reported failure; unloading anyway", id);
                }
            }
        }

        std::shared_ptr<DllHandler> dllToRelease;
        const ShotModule* moduleToShutdown = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Record* record = findRecordLocked(id);
            if (instance != nullptr) {
                eraseInstanceLocked(instance);
                if (record != nullptr) {
                    detachModuleIfUnusedLocked(record->info.dllPath, dllToRelease,
                                               moduleToShutdown);
                }
            }
            if (record != nullptr) {
                record->info.state = SHOT_PLUGIN_STATE_DISABLED;
                record->info.loaded = false;
                record->info.reason.clear();
            }
            addDisabledLocked(id);
        }
        // 实例析构会调 module->destroy：必须在 shutdown 与 FreeLibrary 之前、且在锁外完成。
        instance.reset();
        if (moduleToShutdown != nullptr && moduleToShutdown->shutdown != nullptr) {
            moduleToShutdown->shutdown();
        }
        dllToRelease.reset();  // 最后一个句柄释放后 LoadLibrary 计数归零，DLL 才真的卸载

        persistDisabledList();
        spdlog::info("Plugin disabled: {}", id);
        return true;
    }

    // ---- 启用：包 DLL 还在就只重新 create；不在就重新 LoadLibrary + 校验 + init ----
    std::uint32_t index = 0;
    std::string dllPath;
    std::string name;
    std::string version;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Record* record = findRecordLocked(id);
        if (record == nullptr) {
            reason = "没有这个插件";
            return false;
        }
        if (record->info.state == SHOT_PLUGIN_STATE_LOADED) {
            return true;  // 已经是启用状态：幂等
        }
        index = record->index;
        dllPath = record->info.dllPath;
        name = record->info.name;
        version = record->info.version;
    }

    std::shared_ptr<DllHandler> dll;
    const ShotModule* module = nullptr;
    if (!acquireModule(dllPath, dll, module, reason)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        Record* record = findRecordLocked(id);
        if (record != nullptr) {
            record->info.state = SHOT_PLUGIN_STATE_FAILED;
            record->info.reason = reason;
        }
        spdlog::warn("Failed to reload plugin {}: {}", id, reason);
        return false;
    }

    CreatedPlugin plugin = createPlugin(dll, module, index, id, name, version);
    if (plugin.instance == nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        Record* record = findRecordLocked(id);
        if (record != nullptr) {
            record->info.state = SHOT_PLUGIN_STATE_FAILED;
            record->info.reason = plugin.reason;
        }
        spdlog::warn("Failed to reload plugin {}: {}", id, plugin.reason);
        reason = plugin.reason;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Record* record = findRecordLocked(id);
        if (record != nullptr) {
            record->info.state = SHOT_PLUGIN_STATE_LOADED;
            record->info.loaded = true;
            record->info.reason.clear();
            record->libName = plugin.libName;
            record->libVersion = plugin.libVersion;
        }
        removeDisabledLocked(id);
        registerInstanceLocked(dllPath, plugin.instance);
    }
    persistDisabledList();
    spdlog::info("Plugin enabled: {}", id);
    return true;
}

bool PluginManager::isPluginEnabled(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const Record* record = findRecordLocked(id);
    if (record == nullptr) {
        return false;
    }
    return record->info.state != SHOT_PLUGIN_STATE_DISABLED;
}

std::vector<std::shared_ptr<PluginBase>> PluginManager::getAllInstances() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_instances;
}

std::vector<InstalledPlugin> PluginManager::getInstalledPlugins() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<InstalledPlugin> result;
    result.reserve(m_records.size());
    for (const Record& record : m_records) {
        result.push_back(record.info);
    }
    return result;
}

PluginManager::Record* PluginManager::findRecordLocked(const std::string& id) {
    const auto it = m_recordIds.find(id);
    if (it == m_recordIds.end()) {
        return nullptr;
    }
    return &m_records[it->second];
}

const PluginManager::Record* PluginManager::findRecordLocked(const std::string& id) const {
    const auto it = m_recordIds.find(id);
    if (it == m_recordIds.end()) {
        return nullptr;
    }
    return &m_records[it->second];
}

std::shared_ptr<PluginBase> PluginManager::findLoadedInstanceLocked(
    const std::string& id) const {
    for (const auto& instance : m_instances) {
        if (instance != nullptr && instance->getId() == id) {
            return instance;
        }
    }
    return nullptr;
}

std::string PluginManager::findDependentLocked(const Record& target) const {
    // 目标"提供给进程的名字"= 它自己的 id + metadata provides 里的名字（如 opencv）。
    std::vector<std::string> provided;
    provided.push_back(target.info.id);
    for (const std::string& name : target.meta.provides) {
        provided.push_back(name);
    }
    for (const auto& instance : m_instances) {
        if (instance == nullptr) {
            continue;
        }
        const auto it = m_recordIds.find(instance->getId());
        if (it == m_recordIds.end()) {
            continue;
        }
        const Record& dependent = m_records[it->second];
        for (const PluginRequirement& requirement : dependent.meta.requireList) {
            for (const std::string& name : provided) {
                if (requirement.getName() == name) {
                    return dependent.info.id;
                }
            }
        }
    }
    return {};
}

void PluginManager::eraseInstanceLocked(const std::shared_ptr<PluginBase>& instance) {
    if (instance == nullptr) {
        return;
    }
    m_instances.erase(std::remove(m_instances.begin(), m_instances.end(), instance),
                      m_instances.end());
    const auto it = m_plugins.find(instance->getType());
    if (it != m_plugins.end()) {
        auto& list = it->second;
        list.erase(std::remove(list.begin(), list.end(), instance), list.end());
    }
}

void PluginManager::registerInstanceLocked(const std::string& dllPath,
                                           const std::shared_ptr<PluginBase>& instance) {
    if (instance == nullptr) {
        return;
    }
    m_instances.push_back(instance);
    m_plugins[instance->getType()].push_back(instance);
    const auto it = m_modules.find(dllPath);
    if (it != m_modules.end()) {
        ++it->second.instanceCount;
    }
}

void PluginManager::detachModuleIfUnusedLocked(const std::string& dllPath,
                                               std::shared_ptr<DllHandler>& dll,
                                               const ShotModule*& module) {
    const auto it = m_modules.find(dllPath);
    if (it == m_modules.end()) {
        return;
    }
    if (it->second.instanceCount > 0) {
        --it->second.instanceCount;
    }
    if (it->second.instanceCount > 0) {
        return;  // 同包还有别的实例活着：DLL 继续留着
    }
    dll = it->second.dll;
    module = it->second.module;
    m_modules.erase(it);
}

void PluginManager::addDisabledLocked(const std::string& id) {
    if (std::find(m_disabled.begin(), m_disabled.end(), id) == m_disabled.end()) {
        m_disabled.push_back(id);
    }
}

void PluginManager::removeDisabledLocked(const std::string& id) {
    m_disabled.erase(std::remove(m_disabled.begin(), m_disabled.end(), id), m_disabled.end());
}

void PluginManager::persistDisabledList() const {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ids = m_disabled;
    }
    // 排序后再写：同一组禁用项永远得到同样的 JSON，配置文件的 diff 才干净。
    std::sort(ids.begin(), ids.end());
    nlohmann::json array = nlohmann::json::array();
    for (const std::string& id : ids) {
        array.push_back(id);
    }
    core::Config::getInstance().set(kConfigKeyDisabled, core::ConfigItem(array.dump()));
}

}  // namespace Plugin
