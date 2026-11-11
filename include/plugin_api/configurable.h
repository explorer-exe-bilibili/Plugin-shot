#pragma once

#include <stdint.h>

// ============================================================================
//  能力：可配置  "shot.configurable"
// ============================================================================

/**
 * @brief 布尔配置项；值用字符串 "0"/"1" 表达。
 */
#define SHOT_CONFIG_KIND_BOOL   1u

/**
 * @brief 整数配置项；值用十进制字符串，如 "128"。
 */
#define SHOT_CONFIG_KIND_INT    2u

/**
 * @brief 浮点配置项；值用十进制字符串，如 "0.5"。
 */
#define SHOT_CONFIG_KIND_FLOAT  3u

/**
 * @brief 任意文本配置项；值原样保存。
 */
#define SHOT_CONFIG_KIND_STRING 4u

/**
 * @brief 枚举配置项；候选列表放在 ShotConfigDesc::options，用 | 分隔。
 */
#define SHOT_CONFIG_KIND_ENUM   5u

/**
 * @brief 颜色配置项；值用字符串表达，交给宿主的取色控件渲染。
 */
#define SHOT_CONFIG_KIND_COLOR  6u

/**
 * @brief 文件/目录路径配置项；值用字符串表达。
 */
#define SHOT_CONFIG_KIND_PATH   7u

/**
 * @brief 一个配置项的声明：宿主据此生成界面、存盘、灌默认值。
 *
 * 类型信息只在这里描述；值本身一律用【字符串】表达（default_value 与
 * get/set_config 的 value 都是）：跨边界没有宽度与对齐问题，天然可以存成 JSON，
 * 插件也不必为了读配置而引入 STL。
 *
 * @note 字符串字段都由插件持有（通常是静态字符串），宿主只读；范围类字段只对
 *       INT / FLOAT 有意义，其它类型忽略。
 */
typedef struct ShotConfigDesc {
    uint32_t    struct_size;     /**< 调用方填 sizeof(ShotConfigDesc)；插件据此判断能写哪些字段 */
    uint32_t    kind;            /**< SHOT_CONFIG_KIND_* */
    const char* key;             /**< "keep_alpha"：读写与存盘的唯一标识，发布后不能改名 */
    const char* label;           /**< 界面显示名；不能为 NULL */
    const char* default_value;   /**< 默认值，一律字符串表达："1" / "128" / "low"；可为 NULL（无默认值） */
    const char* options;         /**< ENUM 用 "low|mid|high"；PATH 用 "directory"/"file" 指定对话框类型 */
    double      min;             /**< INT / FLOAT 用；取值范围下界 */
    double      max;             /**< INT / FLOAT 用；取值范围上界 */
    double      step;            /**< INT / FLOAT 用；界面步长 */
    const char* tooltip;         /**< 鼠标悬停提示；可为 NULL */
} ShotConfigDesc;

/**
 * @brief 可配置能力函数表，对应 SHOT_IFACE_CONFIGURABLE、版本 1。
 *
 * 插件自己声明配置项（数量 + 每项描述），宿主据此生成界面、保存文件、灌值，
 * 宿主不硬编码任何配置项名字。get_config / set_config 遇到不认识的 key 必须
 * 安全忽略（返回负值）：这样宿主升级后多传一个配置项，老插件不会出问题。
 *
 * @note 返回码：0 = 成功，1 = 参数/索引不合法，2 = 对方结构太短，负数 = key 不认识。
 * @note set_config 可能在运行中被任意线程调用（用户拖滑块），实现要么自行保证线程
 *       安全，要么声明为"下次 process 生效"。
 */
typedef struct ShotConfigurableV1 {
    uint32_t struct_size;   /**< = sizeof(ShotConfigurableV1)；构造方填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */

    /**
     * @brief 返回配置项个数。
     * @return 配置项个数；0 = 这个插件没有配置项。
     */
    uint32_t (*get_config_count)(void* self);

    /**
     * @brief 声明第 index 个配置项。
     * @param[in]  index 从 0 开始，必须小于 get_config_count 的返回值。
     * @param[out] out   由调用方提供并填好 struct_size；插件据此判断能写哪些字段。
     * @return 0 = 成功；1 = index 越界/参数不合法；2 = out 结构太短。
     */
    int32_t (*get_config_desc)(void* self, uint32_t index, ShotConfigDesc* out);

    /**
     * @brief 读一个配置项的当前值。
     * @param[in]  key      必须与 get_config_desc 声明过的 key 一致，不能为 NULL。
     * @param[out] buf      接收缓冲区，可为 NULL（此时只查长度）。
     * @param[in]  buf_size buf 的字节数；大于返回值时才写入（含结尾 NUL）。
     * @return >= 0 = 值的字节数（不含结尾 NUL）；< 0 = key 不认识或参数非法。
     */
    int32_t (*get_config)(void* self, const char* key, char* buf, uint32_t buf_size);

    /**
     * @brief 写一个配置项；可能在运行中被调用。
     * @param[in] key   必须与 get_config_desc 声明过的 key 一致，不能为 NULL。
     * @param[in] value 字符串形式的新值，不能为 NULL。
     * @return 0 = 已生效（或已记录，下次 process 生效）；< 0 = key 不认识或值非法。
     */
    int32_t (*set_config)(void* self, const char* key, const char* value);
} ShotConfigurableV1;
