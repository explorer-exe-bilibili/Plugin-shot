#pragma once

#include <stdint.h>

// ============================================================================
//  插件身份证：宿主不创建实例也能列出清单
// ============================================================================

/**
 * @brief 一个插件的身份证：不创建实例也能列出的元信息。
 *
 * 由模块表的 get_info(index, out) 填充；宿主用它显示名称、做依赖校验。
 * 字段里的字符串由插件持有（通常是静态字符串），有效期到 DLL 卸载，宿主只读、不释放。
 *
 * @note 元数据只是加载前的声明；运行时以 get_info 为准，两者不一致时宿主只警告。
 */
typedef struct ShotPluginInfo {
    uint32_t    struct_size;   /**< 调用方填 sizeof(ShotPluginInfo)；插件据此判断能写哪些字段 */
    uint32_t    abi_minor;     /**< 该插件编译时使用的 SHOT_ABI_MINOR */
    const char* id;            /**< "author.name"，与 metadata.json 的 plugins[] 对应；不能为 NULL */
    const char* name;          /**< 界面显示名；不能为 NULL */
    const char* version;       /**< 插件自己的语义化版本，如 "1.0.0"；不能为 NULL */
} ShotPluginInfo;

// ============================================================================
//  元数据尾部标记
// ============================================================================

/**
 * @brief 插件元数据 JSON 块的起始标记。
 *
 * 构建期 cmake/AppendPluginMeta.cmake 在 DLL/SO 的原始二进制之后依次追加：
 * 换行、PLUGIN_META_START_MARKER 这一行、换行、metadata.json、换行、
 * PLUGIN_META_END_MARKER 这一行、换行。
 *
 * PE/ELF 加载器只读文件头的节表，文件尾这段额外数据被完全忽略，因此追加不影响
 * 装载；元数据跟着文件走，用户复制/重命名插件也不会丢。
 *
 * @warning 格式必须保持为  #define 宏名 "字面量"：构建脚本在构建期直接读本行
 *          取宏值（不编译这个头文件），改格式会让打包脚本读不到。
 */
#define PLUGIN_META_START_MARKER "---PLUGIN_META_START---"

/**
 * @brief 插件元数据 JSON 块的结束标记。
 *
 * 宿主从文件尾部往前找最后一对标记；同一个 DLL 可能被追加过多次，只有最后一份有效。
 *
 * @warning 与 PLUGIN_META_START_MARKER 必须成对出现，字面量由构建脚本原样写入。
 */
#define PLUGIN_META_END_MARKER   "---PLUGIN_META_END---"
