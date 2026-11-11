#pragma once

#include <stdint.h>
#include "plugin_api/basics.h"
#include "plugin_api/plugin_info.h"
#include "plugin_api_export.h"

// ============================================================================
//  模块表：唯一的导出符号 shot_module_entry() 返回它
// ============================================================================

/**
 * @brief 一个插件包的模块表，也是 shot_module_entry() 的返回类型。
 *
 * 一个 DLL 可以装多个插件：宿主靠 plugin_count 枚举索引 0..plugin_count-1，
 * 对每个索引调用 get_info / create / destroy，靠 query_interface 判断每个实例会什么；
 * metadata.json 的 plugins[] 顺序必须与索引一一对应。
 *
 * 读取方（宿主）规则：先看 struct_size，再用 SHOT_HAS_FIELD 判断想用的字段在不在；
 * 函数指针为 NULL 表示该插件不提供这个能力，调用前必须判空。
 *
 * @note create / destroy 必填；init / shutdown / get_info / query_interface 可省略（写 NULL）。
 *       模块表应由插件以静态存储返回，内容在 DLL 卸载前保持不变。
 */
struct ShotModule {
    uint32_t struct_size;   /**< = sizeof(ShotModule)；读取前先比对 */
    uint32_t abi_major;     /**< 插件编译时的 SHOT_ABI_MAJOR；与宿主不等就整包跳过 */
    uint32_t abi_minor;     /**< 插件编译时的 SHOT_ABI_MINOR；比宿主大时只提示，插件需降级运行 */
    uint32_t plugin_count;  /**< 本 DLL 里的插件个数，索引 0..plugin_count-1；可为 0 */

    /**
     * @brief 模块级初始化；宿主读完本表、创建任何实例之前调用一次。
     * @param[in] host  宿主服务表，有效期覆盖整个模块生命周期，插件可长期保存；
     *                  老宿主可能传 NULL，插件必须容忍。
     * @param[in] flags 预留，当前恒为 0；插件必须忽略自己不认识的位。
     * @return 0 = 接受本宿主；非 0 = 本 DLL 无法在这个宿主上工作，宿主直接卸载它。
     * @note 可为 NULL（= 没有要做的初始化）。
     */
    int32_t (*init)(const ShotHostServices* host, uint32_t flags);

    /**
     * @brief 模块级收尾；宿主卸载本 DLL 前调用一次。
     * @note 调用点在所有实例 destroy 之后、FreeLibrary 之前；可为 NULL。
     */
    void    (*shutdown)(void);

    /**
     * @brief 取第 index 个插件的身份证；宿主不创建实例也能列清单。
     * @param[in]  index 0..plugin_count-1；越界返回非 0。
     * @param[out] out   由调用方提供并填好 struct_size，插件据此判断能写哪些字段。
     * @return 0 = 成功；非 0 = index 越界或参数不合法。
     * @note 可为 NULL（= 不提供身份证，宿主用兜底名字显示）。
     */
    int32_t (*get_info)(uint32_t index, ShotPluginInfo* out);

    /**
     * @brief 创建第 index 个插件的实例，返回插件自己分配的不透明指针。
     * @param[in] index 0..plugin_count-1，必须是本模块认识的索引。
     * @param[in] host  宿主服务表；插件可以把指针和 host_context 一直存着。
     * @return 非 NULL = 实例句柄，之后所有能力调用的 self 都是它；
     *         NULL = 创建失败，宿主跳过该插件。
     * @note 必填；实例只能由本模块的 destroy 释放，宿主绝不 delete。
     */
    void*   (*create)(uint32_t index, const ShotHostServices* host);

    /**
     * @brief 销毁 create 返回的实例；返回前必须释放该实例持有的全部资源。
     * @param[in] instance create 的返回值；宿主不会传 NULL。
     * @note 必填；宿主保证之后不再调用该实例的任何能力。
     */
    void    (*destroy)(void* instance);

    /**
     * @brief 询问实例是否提供某个能力，命中返回对应的函数表。
     * @param[in] instance create 的返回值。
     * @param[in] iface_id 能力 id，如 SHOT_IFACE_IMAGE_PROCESSOR，不能为 NULL。
     * @param[in] version  调用方认识的能力版本（宿主当前传 1）；插件应返回 <= 该版本的表。
     * @return 非 NULL = 该能力的函数表；NULL = 没有这个能力，宿主安静跳过。
     * @note 可为 NULL（= 所有实例都没有可选能力）；返回的指针在实例存活期间有效。
     */
    void*   (*query_interface)(void* instance, const char* iface_id, uint32_t version);
};

// ============================================================================
//  导出契约：一个动态库（插件包）只导出下面这【一个】C 函数。
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief 取插件包的模块表；整个动态库唯一的导出符号。
     *
     * 一个 DLL 可以装多个插件，宿主靠 module->plugin_count + create(index) 枚举。
     * 宿主用 GetProcAddress / dlsym 在运行期解析本符号，因此插件侧必须导出它。
     *
     * @return 指向插件内静态存储的模块表，在 DLL 卸载前一直有效；不得返回 NULL。
     * @note 插件编译时不要定义 SCREENSHOT_HOST_EXPORTS，否则本符号不会被导出。
     * @warning 本符号改名或改签名是 major 级 ABI 破坏；不要返回栈上的临时对象。
     */
    SCREENSHOT_API const ShotModule* shot_module_entry(void);

#ifdef __cplusplus
}
#endif
