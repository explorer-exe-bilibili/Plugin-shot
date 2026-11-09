#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "PluginInstances/base.h"
#include "plugin_meta.h"

struct ShotModule;

namespace Plugin {

/**
 * @brief 一条已安装插件的状态（含被禁用与加载失败的插件）。
 *
 * 注册表服务（shot.host.plugin_registry）按这份记录列清单，所以"没出现"和
 * "出现了但是失败"必须能区分：失败也占一条，原因写在 reason 里。
 * 所有字符串由管理器持有，getInstalledPlugins() 返回的是拷贝。
 */
struct InstalledPlugin {
    std::string id;      /**< 插件 id，如 "example.invert"；始终非空 */
    std::string name;    /**< 界面显示名 */
    std::string version; /**< 插件版本；加载前就失败时用 metadata 的声明值，可能为空串 */
    std::string dllPath; /**< 插件 DLL/SO 路径；始终非空 */
    std::string reason;  /**< 禁用/失败原因；正常时为空串 */
    uint32_t state = 0;  /**< SHOT_PLUGIN_STATE_LOADED / DISABLED / FAILED */
    bool loaded = false; /**< 当前是否有活着的实例（state == LOADED 时为 true） */
};

/**
 * @brief 插件生命周期管理器：扫描目录、按依赖顺序加载、运行期启停、按类型索引。
 *
 * 一个 DLL 是一个插件包；管理器按 DLL 路径缓存模块句柄与 metadata，热加载单个包时
 * 复用这些缓存，不再重新读文件尾。卸载时先销毁实例、再 shutdown 模块、最后释放 DLL。
 *
 * 线程模型：
 *  - loadPlugins / setPluginEnabled / unloadAll 只允许在宿主 main 线程调用
 *    （HostRuntime 负责持生命周期锁，与事件分发同线程）；其中所有插件代码调用
 *    （create / destroy / init / get_info / query_interface / set_config）都在数据锁之外。
 *  - 查询（getInstalledPlugins / getAllInstances / isPluginEnabled / getPluginsByType）
 *    任意线程可调，返回 shared_ptr 或值拷贝的快照。
 *  - 返回的 shared_ptr 保证实例在调用方手里不会被销毁；但插件 DLL 的卸载由
 *    HostRuntime 的生命周期锁负责，调用方不能假设"拿到快照后还能无限期使用"。
 *
 * @warning 插件实例只能由插件自己的 destroy 释放，管理器绝不 delete 插件对象。
 */
class PluginManager {
    /**
     * @brief 扫描期建立、加载期补全的一条插件记录。
     *
     * InstalledPlugin 是它的对外投影；多出来的字段只在宿主内部用（拓扑排序、
     * 依赖检查、重新加载时的索引与运行期版本事实）。
     */
    struct Record {
        InstalledPlugin info;              /**< 对外状态 */
        uint32_t index = 0;                /**< create(index) 的索引 */
        bool hasMeta = false;              /**< 有可用的 metadata 声明 */
        bool opaque = false;               /**< 无 metadata：失败开放、最后加载 */
        bool placeholder = false;          /**< 无 metadata 包的占位记录，不对外暴露 */
        PluginMetaEntry meta;              /**< metadata 声明（hasMeta 时有效） */
        std::string runtimeName;           /**< get_info 报告的名字（没有则等于 info.name） */
        std::string runtimeVersion;        /**< get_info 报告的版本 */
        std::string libName;               /**< lib provider 报告的库名 */
        std::string libVersion;            /**< lib provider 报告的库版本 */
    };

    /** @brief 一个已装载的模块：DLL 句柄 + 模块表 + 当前活着的实例数。 */
    struct ModuleEntry {
        std::shared_ptr<DllHandler> dll;   /**< DLL 句柄；entry 在则库在 */
        const ShotModule* module = nullptr;/**< 指向 DLL 内静态数据 */
        std::size_t instanceCount = 0;     /**< 本包当前活着的实例数；0 时可以 shutdown */
    };

    /** @brief createPlugin 的结果：成功时带包装好的实例，失败时带原因。 */
    struct CreatedPlugin {
        std::shared_ptr<PluginBase> instance; /**< 包装好的实例；失败为空 */
        std::string libName;                  /**< lib provider 的库名，其它类型为空 */
        std::string libVersion;               /**< lib provider 的库版本 */
        std::string reason;                   /**< 失败原因 */
    };

    mutable std::mutex m_mutex;  /**< 保护下面全部数据；绝不递归加锁 */
    // <PluginType, PluginInstances>
    std::unordered_map<PluginType, std::vector<std::shared_ptr<PluginBase>>> m_plugins;
    std::vector<std::shared_ptr<PluginBase>> m_instances;  /**< 全部实例，按加载顺序 */
    std::map<std::string, ModuleEntry> m_modules;          /**< DLL 路径 -> 已装载模块 */
    std::map<std::string, PluginPackageMeta> m_packageMeta;/**< DLL 路径 -> metadata 缓存 */
    std::vector<Record> m_records;                         /**< 全部已安装插件记录 */
    std::unordered_map<std::string, std::size_t> m_recordIds; /**< 插件 id -> 记录下标 */
    std::vector<std::string> m_disabled;                   /**< 被禁用的插件 id 列表 */

    /**
     * @brief 从宿主配置读禁用列表（`plugins.disabled`，JSON 字符串数组）。
     *
     * 只在启动扫描前调用一次；配置里没有这个键时返回空列表（不是错误）。
     * @return 被禁用的插件 id；顺序与配置里一致。
     */
    static std::vector<std::string> readDisabledList();

    /**
     * @brief 解析 DLL 的导出符号并做加载期校验（不调用任何插件能力）。
     *
     * 校验项：shot_module_entry 存在且非空、结构长度够、abi_major 相同、
     * create/destroy 非空。校验失败只返回原因，不抛异常。
     * @param[in]  dll    已加载的 DLL 句柄。
     * @param[out] module 成功时写入模块表指针。
     * @param[out] reason 失败原因（中文）。
     * @return true = 通过校验；false = 这个 DLL 不是可用的插件包。
     */
    static bool resolveModule(const std::shared_ptr<DllHandler>& dll, const ShotModule*& module,
                              std::string& reason);

    /**
     * @brief 调用模块级 init（插件代码，必须在数据锁外调用）。
     * @param[in]  dllPath 插件路径，仅用于日志。
     * @param[in]  module  通过 resolveModule 的模块表。
     * @param[out] reason  失败原因。
     * @return true = 模块接受本宿主；false = init 拒绝或返回非 0。
     */
    static bool initModule(const std::string& dllPath, const ShotModule* module,
                           std::string& reason);

    /**
     * @brief 调 get_info 取插件身份证（插件代码，必须在数据锁外调用）。
     *
     * 老插件可能没有 get_info：这时返回 false，调用方用 metadata 的声明值兜底。
     * @param[in]  module 模块表。
     * @param[in]  index  插件索引。
     * @param[out] out    成功时填入身份证。
     * @return true = get_info 返回 0 且给出了非空的 id。
     */
    static bool fetchIdentity(const ShotModule* module, uint32_t index, ShotPluginInfo& out);

    /** @brief core（宿主自身）的版本：来自 CMake 生成的 version.h。 */
    static ShotVersion hostCoreVersion();

    /**
     * @brief 检查一条记录的依赖是否都被已加载的提供者满足。
     * @param[in]  record    要检查的记录（hasMeta 为 true）。
     * @param[in]  providers 提供者名字（插件 id 或 provides 名）-> 记录下标。
     * @param[in]  records   全部记录；提供者的运行期版本从这里读。
     * @param[out] reason    失败原因。
     * @return true = 依赖全部满足。
     */
    static bool checkRequirements(const Record& record,
                                  const std::map<std::string, std::size_t>& providers,
                                  const std::vector<Record>& records,
                                  std::string& reason);

    /** @brief 无 metadata 包的兜底 id（文件名 + "#index"）。 */
    static std::string fallbackId(const std::string& dllPath, uint32_t index);

    /** @brief 加锁查找记录；id 不存在时返回 nullptr。 */
    Record* findRecordLocked(const std::string& id);
    /** @brief 加锁查找记录（只读版本）；id 不存在时返回 nullptr。 */
    const Record* findRecordLocked(const std::string& id) const;

    /** @brief 加锁按 id 找已加载实例；没有时返回空指针。 */
    std::shared_ptr<PluginBase> findLoadedInstanceLocked(const std::string& id) const;

    /**
     * @brief 加锁检查"有没有已加载插件依赖 target"。
     *
     * 依赖来自 metadata 的 requires：命中 target 的 id，或命中 target 的 provides 名。
     * 被依赖时不允许禁用（v1 不做级联禁用）。
     * @return 第一个依赖者的 id；没有依赖者时返回空串。
     */
    std::string findDependentLocked(const Record& target) const;

    /**
     * @brief 加锁把依赖里的 provider 名字解析成插件记录 id。
     *
     * 先按插件 id 精确匹配，再按 metadata 的 provides 名字匹配（如 "opencv"）。
     * @param[in] name requires 里的提供者名字（不含版本运算符）。
     * @return 提供者记录的 id；没有匹配时返回空串。
     */
    std::string findProviderIdLocked(const std::string& name) const;

    /**
     * @brief 启用单个插件（不做依赖级联）。
     *
     * 包 DLL 还在就只重新 create，不在就重新 LoadLibrary + 校验 + init。所有插件
     * 回调（init / create / module->destroy）都在数据锁之外执行；本函数不写禁用列表。
     *
     * @param[in]  id     插件 id。
     * @param[out] reason 失败原因；成功时清空。
     * @return true = 已加载（或本来就加载着）；false = 加载失败，记录被标成 FAILED。
     */
    bool enableOne(const std::string& id, std::string& reason);

    /**
     * @brief 禁用单个插件：依赖检查 → UI deinit → 销毁实例 → 按需卸载 DLL。
     *
     * 被已加载插件依赖时直接拒绝（v1 不做级联禁用）。插件回调全部在数据锁之外
     * 执行；本函数不写禁用列表，由调用方统一持久化。
     *
     * @param[in]  id     插件 id。
     * @param[out] reason 拒绝/失败原因；成功时清空。
     * @return true = 已禁用（或本来就被禁用）；false = 被依赖拒绝或没有这个插件。
     */
    bool disableOne(const std::string& id, std::string& reason);

    /**
     * @brief 启用级联入口：先递归启用 metadata requires 里的提供者，再启用自己。
     *
     * 跳过 core（宿主自身）；按 record.id / record.meta.provides 匹配提供者；
     * 深度优先 + visited 防环。没有 provider 记录的名字静默跳过（插件未安装）。
     *
     * @param[in]  id     要启用的插件 id。
     * @param[out] reason 失败原因；依赖启用失败时写"依赖 X 启用失败：…"。
     * @return true = 该插件及其依赖都已启用；false = 任一环节失败。
     */
    bool enableWithDependencies(const std::string& id, std::string& reason);

    /**
     * @brief enableWithDependencies 的深度优先实现。
     *
     * @param[in]     id      当前要启用的插件 id。
     * @param[in,out] visited 已访问的 id：防依赖成环，也避免同一个 provider 重复启用。
     * @param[out]    reason  失败原因。
     * @return true = 当前插件（含其依赖）已启用。
     */
    bool enableDependenciesRecursive(const std::string& id, std::vector<std::string>& visited,
                                     std::string& reason);

    /** @brief 加锁把实例从 m_instances / m_plugins 摘掉。
     *        不销毁实例：调用方的 shared_ptr 还持有它，销毁点由调用方决定。 */
    void eraseInstanceLocked(const std::shared_ptr<PluginBase>& instance);

    /** @brief 加锁登记一个已创建好的实例，并增加所属模块的实例计数。 */
    void registerInstanceLocked(const std::string& dllPath,
                                const std::shared_ptr<PluginBase>& instance);

    /**
     * @brief 加锁把模块的实例计数减一；减到 0 时把句柄摘出来交给调用方收尾。
     *
     * 调用方必须保证：实例已经销毁之后再对摘出来的 module 调 shutdown、再释放 dll。
     * @param[in]  dllPath 插件路径。
     * @param[out] dll     最后一个实例时写入 DLL 句柄；否则保持不变。
     * @param[out] module  最后一个实例时写入模块表；否则保持不变。
     */
    void detachModuleIfUnusedLocked(const std::string& dllPath,
                                    std::shared_ptr<DllHandler>& dll,
                                    const ShotModule*& module);

    /** @brief 加锁把 id 放进禁用列表（已存在时是空操作）。 */
    void addDisabledLocked(const std::string& id);
    /** @brief 加锁把 id 从禁用列表移除。 */
    void removeDisabledLocked(const std::string& id);

    /**
     * @brief 把当前禁用列表写回宿主配置（`plugins.disabled`，JSON 字符串数组）。
     *
     * 只改内存；真正落盘由 HostRuntime 在 tick 末尾统一做。写之前把列表排序，
     * 让配置文件内容稳定、便于 diff。
     */
    void persistDisabledList() const;

    /**
     * @brief 装载一个模块（LoadLibrary + 校验 + module->init），已装载时直接复用。
     *
     * 插件代码（module->init）在数据锁外调用；返回值是 DLL 句柄的共享引用，
     * 调用方拿到它就能保证"用到的函数表所在 DLL 不会被卸载"。
     * @param[in]  dllPath 插件路径。
     * @param[out] dll     成功时写入 DLL 句柄。
     * @param[out] module  成功时写入模块表。
     * @param[out] reason  失败原因。
     * @return true = 模块可用；false = 加载/校验/init 失败。
     */
    bool acquireModule(const std::string& dllPath, std::shared_ptr<DllHandler>& dll,
                       const ShotModule*& module, std::string& reason);

    /**
     * @brief 创建第 index 个插件实例并按能力包装（插件代码，必须在数据锁外调用）。
     *
     * 五种具名能力（图像处理 / 库提供者 / UI / 采集 / 保存）按函数表包装；
     * 都认不出来时：**有 metadata 声明**的插件按 PluginType::Generic 用基类包装
     * 继续加载（纯配置项、纯事件订阅插件，例如 clipboard.main），没有 metadata 的
     * 包才按"无法识别"销毁实例并返回失败——绝不泄漏实例。
     * @param[in] dll         模块的 DLL 句柄。
     * @param[in] module      模块表。
     * @param[in] index       create(index) 的索引。
     * @param[in] id          插件 id（身份，来自 get_info 或 metadata）。
     * @param[in] name        插件显示名。
     * @param[in] version     插件版本。
     * @param[in] hasMetadata 该插件是否有 metadata 声明；决定"无具名能力"时是
     *                        包装成 Generic 还是按无法识别跳过。
     * @return 包装好的实例（失败时 instance 为空，reason 有原因）。
     */
    CreatedPlugin createPlugin(const std::shared_ptr<DllHandler>& dll, const ShotModule* module,
                               uint32_t index, const std::string& id, const std::string& name,
                               const std::string& version, bool hasMetadata);

public:
    /** @brief 构造空管理器；不加载任何插件。 */
    PluginManager() = default;
    /** @brief 卸载全部插件与 DLL。 */
    ~PluginManager();

    /** @brief 禁止拷贝：管理器持有唯一的实例与模块表。 */
    PluginManager(const PluginManager&) = delete;
    /** @brief 禁止赋值：管理器持有唯一的实例与模块表。 */
    PluginManager& operator=(const PluginManager&) = delete;

    /**
     * @brief 扫描目录并按依赖顺序加载插件。
     *
     * 五个阶段与旧行为一致：扫目录读 metadata → 建提供者索引 → 拓扑排序 →
     * 按顺序加载 → 每个 DLL 一行汇总。区别是现在会读 `plugins.disabled`：
     * 命中的插件不创建实例、记为 SHOT_PLUGIN_STATE_DISABLED；每个插件（含被禁用
     * 与加载失败的）都会留下一条记录供注册表枚举。
     *
     * @param[in] dir 插件目录；不存在或不是目录时返回 false。
     * @return true = 扫描完成（个别插件失败只跳过并记录）；false = 目录不可用。
     */
    bool loadPlugins(const std::string& dir);

    /** @brief 卸载全部已加载插件：销毁实例、shutdown 模块、释放 DLL。 */
    void unloadAll();

    /**
     * @brief 运行期启用/禁用一个插件（热卸载 / 重新加载）。
     *
     * 只允许在宿主 main 线程调用。**启用会做依赖级联**：先按 metadata 的 requires
     * 递归把提供者（如 opencv.provider）启用成功，再启用目标插件；任一依赖失败则
     * 整条失败，reason 里写明是哪个依赖失败。**禁用不做级联**：被已加载插件依赖时
     * 直接拒绝。
     *
     * 禁用顺序：依赖检查 → UI 插件 post "host.shutdown" + deinit → 销毁实例 →
     * 包内没有实例了才 shutdown 模块并释放 DLL。启用走反方向：包 DLL 还在就只
     * 重新 create，不在就重新 LoadLibrary + 校验 + init。
     *
     * @param[in]  id      插件 id。
     * @param[in]  enabled true = 启用（含级联启用依赖），false = 禁用。
     * @param[out] reason  失败原因；成功时清空。
     * @return true = 状态已切换（或本来就是目标状态）；false = 被拒绝/加载失败。
     * @note 级联/禁用结果会写回宿主配置的 `plugins.disabled`（内存），落盘由
     *       HostRuntime 在 tick 末尾统一做。
     */
    bool setPluginEnabled(const std::string& id, bool enabled, std::string& reason);

    /**
     * @brief 查询插件当前是否启用（不在禁用列表里）。
     * @param[in] id 插件 id。
     * @return true = 启用（可能仍因加载失败而没有实例，用 getInstalledPlugins 的
     *         state 区分）；false = 被禁用或没有这个 id。
     */
    bool isPluginEnabled(const std::string& id) const;

    /**
     * @brief 取全部已加载实例的快照（按加载顺序）。
     *
     * 返回的 shared_ptr 保证调用期间实例不被销毁；调用方不应把它们存起来跨
     * 卸载周期使用。
     * @return 实例快照；没有插件时为空。
     */
    std::vector<std::shared_ptr<PluginBase>> getAllInstances() const;

    /**
     * @brief 取全部已安装插件记录的快照（含被禁用与加载失败的）。
     * @return 记录拷贝，顺序与扫描/加载顺序一致。
     */
    std::vector<InstalledPlugin> getInstalledPlugins() const;

    /**
     * @brief 按插件类型取出全部实例并向下转型。
     *
     * @param[in] type 插件类型，如 PluginType::ImageProcessor。
     * @return 转换成功的实例列表；没有匹配时返回空 vector。
     */
    template<typename T>
    std::vector<std::shared_ptr<T>> getPluginsByType(PluginType type) const {
        std::vector<std::shared_ptr<T>> result;
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_plugins.find(type);
        if (it != m_plugins.end()) {
            for (const auto& plugin : it->second) {
                if (auto casted = std::dynamic_pointer_cast<T>(plugin)) {
                    result.push_back(std::move(casted));
                }
            }
        }
        return result;
    }
};

}  // namespace Plugin
