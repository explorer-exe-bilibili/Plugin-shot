#pragma once

#include <string>
#include <memory>

#include "dllHandler.h"
#include "plugin_api.h"

namespace Plugin {
/**
 * @brief 插件的能力分类；宿主按它给实例建索引。
 */
enum class PluginType{
    None,            /**< 未分类/未知（仅作包装类的默认值，不会登记到索引）。 */
    Lib,             /**< 运行库提供者（shot.lib_provider）。 */
    UIProvider,      /**< 自带窗口/托盘的 UI 提供者（shot.ui_provider）。 */
    ImageProcessor,  /**< 图像处理器（shot.image_processor）。 */
    ScreenCapture,   /**< 屏幕采集（shot.screen_capture）：只写宿主分配的缓冲。 */
    ImageSaver,      /**< 图像保存（shot.image_saver）：只负责落盘。 */
    /**
     * @brief 无具名能力的通用插件：只声明配置项、或在 create 里订阅事件参与协作。
     *
     * 典型例子是 clipboard.main（只有 shot.configurable + capture.done 订阅）——
     * 它没有宿主可以直接调用的能力表，但实例、配置项与订阅都是有效的。宿主用
     * PluginBase 基类包装它（不会索引到任何能力向量里），注册表照常列 LOADED、
     * 列表目录照常用 query_interface(shot.configurable) 枚举它的配置项。
     *
     * @note 这类插件没有 init/deinit：事件订阅必须在 create 里做、在 destroy 里退订。
     */
    Generic
};
/**
 * @brief 宿主侧插件实例基类：持有模块表、插件实例指针与 DLL 句柄。
 *
 * 三样东西必须一起保管：
 *   m_module   —— 插件的自描述模块表（destroy / query_interface 都在里面）
 *   m_instance —— 插件自己分配、自己释放的不透明指针
 *   m_ops      —— 具体能力的函数表（由子类持有）
 * m_ops 与 m_module 指向的数据都在插件 DLL 里，其有效性由 m_dllHandle 保证：
 * 实例活着，DLL 就不会被卸载。
 *
 * 另外记下插件身份（id / name / version）：注册表服务要按 id 列出与启停插件，
 * 而 get_info 只在创建实例时调用一次，之后从包装对象取，不再回头问插件。
 *
 * @warning 插件实例只能由插件自己的 destroy 释放，宿主绝不 delete。
 */
class PluginBase{
    std::shared_ptr<DllHandler> m_dllHandle;
protected:
    const ShotModule* m_module = nullptr;      /**< 插件模块表；指向 DLL 内静态数据。 */
    void* m_instance = nullptr;                /**< 插件分配的不透明实例指针。 */
    std::string m_id;                          /**< 插件 id（"作者.名字"），来自 get_info 或 metadata。 */
    std::string m_name;                        /**< 插件显示名。 */
    std::string m_version;                     /**< 插件版本；加载失败时可能为空串。 */
    const PluginType m_type=PluginType::None;  /**< 能力分类。 */
public:
    /**
     * @brief 包装一个已创建的插件实例。
     * @param[in] dllHandle 插件 DLL 句柄，非空，与实例同生命周期。
     * @param[in] module 插件模块表指针，非空，指向 DLL 内静态数据。
     * @param[in] instance 插件分配的不透明实例指针。
     * @param[in] id 插件 id；get_info 没给出 id 时用 metadata 的声明值兜底。
     * @param[in] name 插件显示名。
     * @param[in] version 插件版本。
     * @param[in] type 能力分类。
     */
    PluginBase(std::shared_ptr<DllHandler> dllHandle, const ShotModule* module, void* instance,
               std::string id, std::string name, std::string version, PluginType type):
        m_dllHandle(std::move(dllHandle)), m_module(module), m_instance(instance),
        m_id(std::move(id)), m_name(std::move(name)), m_version(std::move(version)), m_type(type) {}
    /** @brief 析构时调用插件的 destroy 释放实例；不 delete 插件对象。 */
    virtual ~PluginBase(){
        if (m_module != nullptr && m_module->destroy != nullptr && m_instance != nullptr) {
            m_module->destroy(m_instance);
            m_instance = nullptr;
        }
    }
    /** @brief 禁止拷贝：实例与 DLL 句柄必须一一对应。 */
    PluginBase(const PluginBase&) = delete;
    /** @brief 禁止赋值：实例与 DLL 句柄必须一一对应。 */
    PluginBase& operator=(const PluginBase&) = delete;
    /** @brief 取插件 id（注册表服务按它查询与启停）。 */
    virtual std::string getId() const { return m_id; }
    /** @brief 取插件显示名。 */
    virtual std::string getName() const { return m_name; }
    /** @brief 取插件版本；插件没有报告版本时为空串。 */
    virtual std::string getVersion() const { return m_version; }
    /** @brief 取能力分类。 */
    virtual PluginType getType() const { return m_type; }
    /**
     * @brief 取插件实例的不透明指针。
     *
     * 调用任何能力函数表时都要把它作为 self 传入。
     * @return 插件实例指针；包装对象析构后失效。
     */
    virtual void* nativeHandle() const { return m_instance; }
    /**
     * @brief 询问可选能力，例如 SHOT_IFACE_CONFIGURABLE。
     * @param[in] ifaceId interface id，不能为空。
     * @param[in] version 请求的接口版本。
     * @return 能力函数表指针；nullptr = 插件没有这个能力（正常情况）。
     */
    virtual void* queryInterface(const char* ifaceId, uint32_t version) const {
        if (m_module == nullptr || m_module->query_interface == nullptr) {
            return nullptr;
        }
        return m_module->query_interface(m_instance, ifaceId, version);
    }
    /**
     * @brief 向插件投递事件（基类默认忽略）。
     * @param[in] ev 事件。
     * @return 基类恒返回 0（没有要投递的对象）；有 UI 的子类会重写。
     */
    virtual int32_t postToPlugin(const ShotEvent& ev) const { return 0; }
};
}  // namespace Plugin
