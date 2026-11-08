#include "hostConfigCatalog.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "PluginInstances/base.h"
#include "config.h"
#include "hostConfigurable.h"
#include "hostServices.h"
#include "pluginManager.h"

namespace {

// 进程内唯一的配置目录。服务表函数没有 ctx 可用（契约里 ctx 是 host_context），
// 所以用这个指针回指宿主；HostConfigCatalog 构造时登记、析构时注销。
// 用原子指针是因为服务函数可能在插件线程上读它，而注销发生在宿主退出时。
static std::atomic<Host::HostConfigCatalog*> activeCatalog{nullptr};

// 事件主题：一次批量提交完成的通知（负载为空），见 document/SETTINGS.md §4。
const char kConfigAppliedTopic[] = "config.applied";

/**
 * @brief kind 是否是契约里已定义的配置种类。
 * @param[in] kind SHOT_CONFIG_KIND_* 的值。
 * @return true = 1..7 之一；未知种类由调用方安静跳过。
 */
bool isKnownKind(uint32_t kind) {
    return kind >= SHOT_CONFIG_KIND_BOOL && kind <= SHOT_CONFIG_KIND_PATH;
}

}  // namespace

namespace Host {

HostConfigCatalog::HostConfigCatalog(std::mutex& lifecycleMutex)
    : m_lifecycleMutex(&lifecycleMutex) {
    activeCatalog.store(this);
}

HostConfigCatalog::~HostConfigCatalog() {
    // 只有"当前这一个"才注销：防御将来万一出现第二个实例把别人的登记清掉。
    HostConfigCatalog* expected = this;
    activeCatalog.compare_exchange_strong(expected, nullptr);
}

void HostConfigCatalog::bind(Plugin::PluginManager* manager) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_manager = manager;
}

void HostConfigCatalog::unbind() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_manager = nullptr;
    m_current.reset();
    m_previous.reset();
}

const ShotHostPluginConfigV1* HostConfigCatalog::service() {
    // 表是 constexpr 般的静态数据：函数指针只在类内部可取（回调是私有成员）。
    static const ShotHostPluginConfigV1 kService = {
        .struct_size        = sizeof(ShotHostPluginConfigV1),
        .version            = 1,
        .configurable_count = &HostConfigCatalog::configurableCount,
        .configurable_info  = &HostConfigCatalog::configurableInfo,
        .config_count       = &HostConfigCatalog::configCount,
        .config_desc        = &HostConfigCatalog::configDesc,
        .get_value          = &HostConfigCatalog::getValue,
        .set_value          = &HostConfigCatalog::setValue,
        .apply_stored       = &HostConfigCatalog::applyStored,
        .save               = &HostConfigCatalog::save,
    };
    return activeCatalog.load() != nullptr ? &kService : nullptr;
}

void HostConfigCatalog::refresh() {
    // 目录是实时枚举的：每次服务调用都从 PluginManager 取最新实例快照，没有缓存
    // 需要重建。这个入口保留下来表达"生命周期变化后目录可能变了"；以后若把配置
    // 声明缓存起来做优化，重建点就放这里，调用方（HostRuntime::tick）不用改。
}

void HostConfigCatalog::dropSnapshots() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_current.reset();
    m_previous.reset();
}

std::shared_ptr<const std::vector<HostConfigCatalog::Provider>>
HostConfigCatalog::snapshotProvidersLocked() {
    Plugin::PluginManager* manager = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        manager = m_manager;
    }

    std::vector<Provider> providers;
    providers.push_back(coreProvider());
    if (manager != nullptr) {
        // 实时快照：目录随插件启停立即变化，设置页收到 plugins.changed 后重查即可。
        for (const auto& instance : manager->getAllInstances()) {
            if (instance == nullptr) {
                continue;
            }
            void* iface = instance->queryInterface(SHOT_IFACE_CONFIGURABLE, 1);
            if (iface == nullptr) {
                continue;  // 没声明可配置：正常情况，不是错误
            }
            const auto* config = static_cast<const ShotConfigurableV1*>(iface);
            // 缺任何必备操作都当"不可配置"：老插件/坏插件不该把设置页带崩。
            if (!SHOT_HAS_FIELD(config, ShotConfigurableV1, set_config) ||
                config->get_config_count == nullptr || config->get_config_desc == nullptr ||
                config->get_config == nullptr || config->set_config == nullptr) {
                continue;
            }
            Provider provider;
            provider.id = instance->getId();
            if (provider.id.empty()) {
                continue;  // 没有 id 就无法按 id 定位，跳过
            }
            provider.name = instance->getName();
            provider.version = instance->getVersion();
            provider.instance = instance;
            provider.self = instance->nativeHandle();
            provider.config = config;
            providers.push_back(std::move(provider));
        }
    }

    auto snapshot = std::make_shared<const std::vector<Provider>>(std::move(providers));
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        // 保留上一份：服务函数刚返回的字符串指针可能还被调用方拿着，等下一次刷新
        // 才允许回收（契约要求字符串至少活到"下一次目录调用"）。
        m_previous = std::move(m_current);
        m_current = snapshot;
    }
    return snapshot;
}

const HostConfigCatalog::Provider* HostConfigCatalog::findProvider(
    const std::vector<Provider>& providers, const char* id) {
    if (id == nullptr) {
        return nullptr;
    }
    for (const Provider& provider : providers) {
        if (provider.id == id) {
            return &provider;
        }
    }
    return nullptr;
}

HostConfigCatalog::Provider HostConfigCatalog::coreProvider() {
    Provider provider;
    provider.id = HostConfigurable::id();
    provider.name = HostConfigurable::name();
    provider.version = HostConfigurable::version();
    provider.self = nullptr;  // 宿主设置没有实例，能力函数忽略 self
    provider.config = HostConfigurable::service();
    return provider;
}

int32_t HostConfigCatalog::applyStoredToProvider(const Provider& provider) {
    if (provider.config == nullptr || provider.config->get_config_count == nullptr) {
        return 0;
    }
    core::Config& config = core::Config::getInstance();
    int32_t applied = 0;
    const uint32_t count = provider.config->get_config_count(provider.self);
    for (uint32_t item = 0; item < count; ++item) {
        ShotConfigDesc desc{};
        desc.struct_size = sizeof(ShotConfigDesc);
        if (provider.config->get_config_desc(provider.self, item, &desc) != 0) {
            continue;
        }
        if (desc.key == nullptr || desc.key[0] == '\0') {
            continue;
        }
        // 存盘键带提供者前缀：core/logging.level、example.invert/keep_alpha。
        const std::string configKey = provider.id + "/" + desc.key;
        std::string value;
        if (config.contains(configKey)) {
            value = config.get(configKey).toString();
        } else if (desc.default_value != nullptr) {
            // 第一次运行没有旧值：用默认值并立刻记进配置，落盘后配置文件就是完整的。
            value = desc.default_value;
            config.set(configKey, core::ConfigItem(value));
        } else {
            continue;  // 没有已存值也没有默认值：这个键不落盘、不灌值
        }
        if (provider.config->set_config(provider.self, desc.key, value.c_str()) == 0) {
            ++applied;
        }
    }
    return applied;
}

int32_t HostConfigCatalog::applyStoredLocked() {
    Plugin::PluginManager* manager = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        manager = m_manager;
    }
    if (manager == nullptr) {
        return -1;  // 还没绑定配置目录：调用方先 init
    }
    const auto snapshot = snapshotProvidersLocked();
    int32_t applied = 0;
    for (const Provider& provider : *snapshot) {
        applied += applyStoredToProvider(provider);
    }
    return applied;
}

uint32_t HostConfigCatalog::configurableCount(void* /*ctx*/) {
    HostConfigCatalog* catalog = activeCatalog.load();
    if (catalog == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(*catalog->m_lifecycleMutex);
    return static_cast<uint32_t>(catalog->snapshotProvidersLocked()->size());
}

int32_t HostConfigCatalog::configurableInfo(void* /*ctx*/, uint32_t index,
                                            ShotPluginInfo* out) {
    if (out == nullptr) {
        return 1;
    }
    if (out->struct_size < SHOT_REQUIRES(ShotPluginInfo, version)) {
        return 2;
    }
    HostConfigCatalog* catalog = activeCatalog.load();
    if (catalog == nullptr) {
        return 1;
    }
    std::lock_guard<std::mutex> lock(*catalog->m_lifecycleMutex);
    const auto snapshot = catalog->snapshotProvidersLocked();
    if (index >= snapshot->size()) {
        return 1;
    }
    const Provider& provider = (*snapshot)[index];
    out->abi_minor = SHOT_ABI_MINOR;
    out->id = provider.id.c_str();
    out->name = provider.name.c_str();
    out->version = provider.version.c_str();
    return 0;
}

uint32_t HostConfigCatalog::configCount(void* /*ctx*/, const char* providerId) {
    if (providerId == nullptr) {
        return 0;
    }
    HostConfigCatalog* catalog = activeCatalog.load();
    if (catalog == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(*catalog->m_lifecycleMutex);
    const auto snapshot = catalog->snapshotProvidersLocked();
    const Provider* provider = findProvider(*snapshot, providerId);
    if (provider == nullptr) {
        return 0;  // 不认识的 id 返回 0，不报错
    }
    return provider->config->get_config_count(provider->self);
}

int32_t HostConfigCatalog::configDesc(void* /*ctx*/, const char* providerId, uint32_t item,
                                      ShotConfigDesc* out) {
    if (providerId == nullptr || out == nullptr) {
        return 1;
    }
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    HostConfigCatalog* catalog = activeCatalog.load();
    if (catalog == nullptr) {
        return 1;
    }
    std::lock_guard<std::mutex> lock(*catalog->m_lifecycleMutex);
    const auto snapshot = catalog->snapshotProvidersLocked();
    const Provider* provider = findProvider(*snapshot, providerId);
    if (provider == nullptr) {
        return -1;  // provider 不认识
    }
    const int32_t rc = provider->config->get_config_desc(provider->self, item, out);
    if (rc != 0) {
        return rc;
    }
    // 声明是插件自己写的，宿主必须校验：设置页拿到空 key 或未知 kind 会画不出来。
    if (out->key == nullptr || out->key[0] == '\0' || !isKnownKind(out->kind)) {
        spdlog::warn("Provider {} declared an invalid config item #{}", providerId, item);
        return 1;
    }
    return 0;
}

int32_t HostConfigCatalog::getValue(void* /*ctx*/, const char* providerId, const char* key,
                                    char* buf, uint32_t bufSize) {
    if (providerId == nullptr || key == nullptr) {
        return -1;
    }
    HostConfigCatalog* catalog = activeCatalog.load();
    if (catalog == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(*catalog->m_lifecycleMutex);
    const auto snapshot = catalog->snapshotProvidersLocked();
    const Provider* provider = findProvider(*snapshot, providerId);
    if (provider == nullptr) {
        return -1;
    }
    return provider->config->get_config(provider->self, key, buf, bufSize);
}

int32_t HostConfigCatalog::setValue(void* /*ctx*/, const char* providerId, const char* key,
                                    const char* value) {
    if (providerId == nullptr || key == nullptr || value == nullptr) {
        return -1;
    }
    HostConfigCatalog* catalog = activeCatalog.load();
    if (catalog == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(*catalog->m_lifecycleMutex);
    const auto snapshot = catalog->snapshotProvidersLocked();
    const Provider* provider = findProvider(*snapshot, providerId);
    if (provider == nullptr) {
        return -1;
    }
    const int32_t rc = provider->config->set_config(provider->self, key, value);
    if (rc != 0) {
        return rc;
    }
    // 值已经推给提供者；宿主这边只记内存，落盘要再调 save()。
    const std::string configKey = std::string(providerId) + "/" + key;
    core::Config::getInstance().set(configKey, core::ConfigItem(value));
    return 0;
}

int32_t HostConfigCatalog::applyStored(void* /*ctx*/) {
    HostConfigCatalog* catalog = activeCatalog.load();
    if (catalog == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(*catalog->m_lifecycleMutex);
    return catalog->applyStoredLocked();
}

int32_t HostConfigCatalog::save(void* /*ctx*/) {
    if (activeCatalog.load() == nullptr) {
        return -1;
    }
    // 落盘不碰插件，所以不需要生命周期锁；saveConfigFile 内部自己串行化并返回
    // 真实结果。契约要求落盘失败时返回负数且不发布 config.applied。
    if (!core::Config::getInstance().saveConfigFile()) {
        return -1;
    }
    HostServices::eventBusPublish(nullptr, kConfigAppliedTopic, nullptr, 0);
    return 0;
}

}  // namespace Host
