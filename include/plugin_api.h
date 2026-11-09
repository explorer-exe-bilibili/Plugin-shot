#pragma once

#include <stddef.h>
#include <stdint.h>

#include "plugin_api_export.h"

// ============================================================================
//  Plugin-shot 插件契约 —— 宿主与插件共用的唯一事实来源（纯 C）
//
//  这份头文件的目标：尽量不需要再改它。手段是三条：
//    1) 只导出【一个】符号 shot_module_entry，其余全部是它返回的结构体里的字段。
//       改导出符号是硬破坏；往结构体末尾追加字段不是。
//    2) 每个跨边界结构体的第一个字段都是 struct_size。任何一次读字段之前先问
//       SHOT_HAS_FIELD()：对方给的这份结构够不够长？于是"加字段"永远不必改版本。
//    3) 能力用字符串 interface id 查询，而不是枚举 + 联合体。新增一类插件 =
//       新增一个 id 加一个结构体，不动任何已有结构体。
//
//  三条纪律（违反任意一条都会真的破坏 ABI）：
//    · 结构体只能【末尾追加】字段，永不重排、永不改类型、永不删。作废的字段
//      改名成 retired_xxx 并以等宽类型占位，永远不再使用。
//    · 跨边界只用定宽类型（uint32_t / int32_t / int64_t / double），
//      不用 int / long / size_t —— 它们的宽度是平台定的。
//    · 异常绝不跨越 DLL 边界：每个 extern "C" 函数内部自行兜底。
// ============================================================================

// ============================================================================
//  版本
// ============================================================================

/**
 * @brief 契约主版本号：布局或语义不兼容时递增。
 *
 * 宿主只接受 abi_major 与本值相同的插件（不相等就整个 DLL 跳过），
 * 所以这个数字一动，已发布的插件都要重编或作废。
 *
 * @warning 递增之前先确认真的无法用"末尾追加字段 + 新结构体"解决；
 *          改动后同步更新文件末尾的布局锚点与 document/ABI_POLICY.md。
 * @note 2.0 删除了宿主服务 `shot.host.plugin_config`（配置目录），改为通用列表服务
 *       `shot.host.plugin_lists`：列表数据是 JSON，注册/写值/提交全部走 `lists.*`
 *       事件（见 document/LISTS.md）。删除已发布的服务属于破坏性变更，所以 major 涨到 2。
 */
#define SHOT_ABI_MAJOR 2

/**
 * @brief 契约次版本号：只在末尾追加能力时递增。
 *
 * 宿主接受 abi_minor <= 自己的插件，所以老宿主能加载新插件；对 minor 更大的
 * 插件只打一条提示，并且只请求自己认识的能力版本——插件必须能在老宿主上降级。
 *
 * @note 追加字段、新增 interface id 时递增；追加出来的字段老宿主永远不会调用，
 *       因此"必须执行"的语义不能靠追加字段表达。
 * @note 2.0 起，"给 UI/宿主提供一批数据"不再对应新的能力或服务：定义一个新 list id
 *       + JSON schema，由插件在运行期用 `lists.add` 注册、宿主用 `lists.changed`
 *       通知、写值/提交用 `lists.update` / `lists.commit`（见 document/LISTS.md）。
 *       因此此后新增列表类型不需要再改 ABI。
 */
#define SHOT_ABI_MINOR 0

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

// ============================================================================
//  能力（interface）id
//
//  query_interface(instance, id, version) 用这些字符串询问"你有没有这个能力"：
//  返回非空 = 有（指向该能力的函数表），返回 NULL = 没有，宿主安静跳过。
//
//  新增一类能力 = 在这一段里加一个 id + 下面加一个结构体，不需要改动任何已有
//  结构体，因此也不需要改版本号。所有 id 必须登记到 document/ABI_POLICY.md。
//  能力函数表里的 self 一律是 create() 返回的不透明指针，宿主原样回传，
//  插件不得假设它是任何具体类型。
// ============================================================================

/**
 * @brief 图像处理器能力：处理一张 ShotImage，对应结构体 ShotImageProcessorV1。
 */
#define SHOT_IFACE_IMAGE_PROCESSOR  "shot.image_processor"

/**
 * @brief 可配置能力：声明配置项并读写当前值，对应结构体 ShotConfigurableV1。
 */
#define SHOT_IFACE_CONFIGURABLE     "shot.configurable"

/**
 * @brief 运行库提供者能力：声明本包只为进程带入某个运行库，对应结构体 ShotLibProviderV1。
 */
#define SHOT_IFACE_LIB_PROVIDER     "shot.lib_provider"

/**
 * @brief UI 提供者能力：窗口/托盘由插件自己创建和管理，对应结构体 ShotUIProviderV1。
 *
 * 窗口和托盘都用这一种能力，不区分：宿主只调 init / deinit / post_to_plugin，
 * 插件内部是窗口还是托盘图标、有没有额外线程，宿主完全不知道。
 */
#define SHOT_IFACE_UI_PROVIDER      "shot.ui_provider"

/**
 * @brief 屏幕采集能力：插件把屏幕变成一张 ShotImage，对应结构体 ShotScreenCaptureV1。
 *
 * 只负责"抓"，不负责处理或保存：宿主按 capture.request 事件把采集、处理、保存
 * 串成流水线（见 document/CAPTURE.md）。版本 1 只支持整个虚拟桌面。
 */
#define SHOT_IFACE_SCREEN_CAPTURE   "shot.screen_capture"

/**
 * @brief 图像保存能力：插件把一张 ShotImage 落盘，对应结构体 ShotImageSaverV1。
 *
 * 格式、质量、目录、文件名模板都是插件自己的 shot.configurable 配置项；宿主只
 * 提供图像和一次性的覆盖参数，不硬编码任何保存策略。
 */
#define SHOT_IFACE_IMAGE_SAVER      "shot.image_saver"

/**
 * @brief 宿主配置服务：向宿主 query_interface 索取 ShotHostConfigV1。
 *
 * 与上面四个不同，这个 id 由【宿主】实现、插件来问；version 传 1。
 */
#define SHOT_HOST_IFACE_CONFIG      "shot.host.config"

/**
 * @brief 宿主广播事件服务：向宿主 query_interface 索取 ShotHostBoardEventV1。
 *
 * version 传 1；与 SHOT_HOST_IFACE_CONFIG 一样，实现方是宿主而不是插件。
 */
#define SHOT_HOST_IFACE_BOARD_MSG   "shot.host.board_msg"

/**
 * @brief 宿主事件总线：向宿主 query_interface 索取 ShotHostEventBusV1。
 *
 * 进程内主题发布/订阅：插件之间、插件与宿主之间都靠它异步通信
 * （主题约定与线程模型见 document/EVENT_BUS.md）。version 传 1；
 * 与上面两个宿主服务一样，实现方是宿主而不是插件。
 */
#define SHOT_HOST_IFACE_EVENT_BUS   "shot.host.event_bus"

/**
 * @brief 宿主列表服务：向宿主 query_interface 索取 ShotHostPluginListsV1。
 *
 * 通用"插件贡献的数据列表"：宿主把 core 与各插件的贡献按 list id 聚成 JSON 快照
 * （`config_list`、`tray_command_list` 等，见 document/LISTS.md）。列表的注册、
 * 写值与提交全部通过 `lists.*` 事件完成；本服务只负责枚举与读取快照。
 * version 传 1；实现方是宿主。
 */
#define SHOT_HOST_IFACE_PLUGIN_LISTS    "shot.host.plugin_lists"

/**
 * @brief 宿主插件注册表：向宿主 query_interface 索取 ShotHostPluginRegistryV1。
 *
 * 列出已安装插件（含被禁用/加载失败的）并请求启停；禁用是插件生命周期操作，
 * 不属于配置项，所以单独开一个服务。version 传 1；实现方是宿主。
 */
#define SHOT_HOST_IFACE_PLUGIN_REGISTRY "shot.host.plugin_registry"

// TODO: 新增录屏能力：同上，新增 interface id + 结构体，契约本身不用改。

// ============================================================================
//  前向 typedef
//
//  这些结构在后面才定义，但有些能力结构体里已经用到了它们的【指针】。C 允许
//  只前向声明，所以集中放在这里 —— 放在各自定义前面会在第一次使用处报
//  "缺少类型说明符"（实测 MSVC C4430 + C2143）。
//
//  注意不能写成 "typedef struct ShotModule ShotHostServices" —— 那是给 ShotModule
//  起别名，后面再定义 struct ShotHostServices 就成了重定义（实测 MSVC C2371）。
// ============================================================================
typedef struct ShotHostServices ShotHostServices;
typedef struct ShotModule ShotModule;

// ============================================================================
//  结构体自描述与读取规则
// ============================================================================

/**
 * @brief 算出读取 type 的 field 字段所需的字节上界（offsetof + sizeof）。
 *
 * 结果与对方填的 struct_size 比较，就能判断这份结构够不够长。
 * 纯编译期常量表达式，没有运行期开销。
 */
#define SHOT_REQUIRES(type, field) \
    ((uint32_t)(offsetof(type, field) + sizeof(((type*)0)->field)))

/**
 * @brief 判断对方给的结构指针是否长到包含 field 字段。
 *
 * 读取任何跨边界字段之前先过一遍：长度不够就当字段不存在（对方是旧结构），
 * 直接读会越界。配套的函数指针字段还要再判一次空——字段存在不代表实现提供了能力。
 *
 * @warning 只比较长度，不验证指针本身是否有效，也不保证字段内容已初始化。
 */
#define SHOT_HAS_FIELD(ptr, type, field) \
    ((ptr) != NULL && (ptr)->struct_size >= SHOT_REQUIRES(type, field))

/**
 * @brief 编译期断言：条件不成立就编译失败。
 *
 * C++ 用 static_assert，C11 及以后用 _Static_assert；更老的 C 编译器上展开为空。
 * 文件末尾的布局锚点全靠它，所以构建矩阵里不能缺了 C11/C++11 之后的编译器。
 */
#if defined(__cplusplus)
    #define SHOT_STATIC_ASSERT(cond) static_assert(cond, #cond)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define SHOT_STATIC_ASSERT(cond) _Static_assert(cond, #cond)
#else
    #define SHOT_STATIC_ASSERT(cond) /* 该编译器不支持静态断言 */
#endif

// ============================================================================
//  图像：处理类插件的数据契约
// ============================================================================

/**
 * @brief 像素格式常量：8 位 4 通道 BGRA，内存顺序为 B、G、R、A。
 *
 * pixel_format 显式声明像素布局，不再靠 channels 猜；当前只定义这一种，
 * 其它值一律视为不支持。
 *
 * @note 用定宽常量而不是 C 枚举：枚举的宽度由编译器决定，不能进 ABI。
 */
#define SHOT_PIXEL_FORMAT_BGRA8 1u

/**
 * @brief 一张待处理图像：8 位像素缓冲 + 显式声明的布局。
 *
 * data 不保证连续：必须按行寻址，用 stride 算行首，不能假设行尾没有填充。
 * stride 为负表示自底向上存储（data 指向最后一行）；其绝对值可以大于
 * width * channels。可用字节数至少为 (height-1)*|stride| + width*channels，
 * 越界写就是踩坏宿主内存。
 *
 * 老宿主的 ShotImage 里可能还没有 pixel_format 字段（那之前只能靠 channels 猜），
 * 读它之前先过 SHOT_HAS_FIELD。
 *
 * @note 原地处理，故意非 const：插件只能改像素，不得改结构体字段。
 * @warning 宿主与插件在同一进程、没有隔离，越界写会直接破坏宿主。
 */
typedef struct ShotImage {
    uint32_t struct_size;   /**< = sizeof(ShotImage)；构造方填，读取方先比对 */
    uint32_t pixel_format;  /**< SHOT_PIXEL_FORMAT_*；老结构里可能没有这个字段 */
    uint8_t* data;          /**< 图像首行第一个像素；行序由 stride 的正负决定，不能为 NULL */
    int32_t  width;         /**< 以像素计；图像有效时 >= 1 */
    int32_t  height;        /**< 以像素计；图像有效时 >= 1 */
    int32_t  stride;        /**< 行间距，字节为单位，可负；每行前 width*channels 字节有效，其余是填充 */
    int32_t  channels;      /**< 每像素通道数；BGRA8 为 4 */
} ShotImage;

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
//  能力：图像处理器  "shot.image_processor"
// ============================================================================

/**
 * @brief 图像处理器能力函数表，对应 SHOT_IFACE_IMAGE_PROCESSOR、版本 1。
 *
 * process 返回 0 才算处理成功；非 0 由宿主当成"这个插件没处理成功"，
 * 不要用它表达"部分成功"。
 */
typedef struct ShotImageProcessorV1 {
    uint32_t struct_size;   /**< = sizeof(ShotImageProcessorV1)；构造方填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */
    /**
     * @brief 就地处理一张图。
     * @param[in,out] image 由宿主提供；像素可改，结构体字段不可改，不能为 NULL。
     * @return 0 = 成功；非 0 = 处理失败（宿主当成"这个插件没处理成功"）。
     * @note 必须按行寻址，并尊重 pixel_format / channels。
     */
    int32_t  (*process)(void* self, ShotImage* image);
} ShotImageProcessorV1;

// ============================================================================
//  能力：运行库提供者  "shot.lib_provider"
// ============================================================================

/**
 * @brief 运行库提供者能力函数表，对应 SHOT_IFACE_LIB_PROVIDER、版本 1。
 *
 * 有些包不提供宿主可调用的算法，只负责把某个运行库带进进程（典型：OpenCV）。
 * 它用这个能力表明自己是"库/环境提供者"，而不是一个坏掉的图像处理器；
 * 宿主见到它就按 PluginType::Lib 登记，名字与版本用于界面显示和依赖校验。
 *
 * @note 两个 getter 返回的都是插件内的静态字符串，有效期到 DLL 卸载；宿主只读，
 *       不释放。追加能力时在这个结构体末尾加字段即可（abi_minor 按需递增）。
 */
typedef struct ShotLibProviderV1 {
    uint32_t struct_size;                           /**< = sizeof(ShotLibProviderV1)；构造方填 */
    uint32_t version;                               /**< 函数表版本，当前为 1 */
    const char* (*get_library_name)(void* self);    /**< 返回库名，如 "opencv"；不能为 NULL */
    const char* (*get_library_version)(void* self); /**< 返回库版本，如 "4.12.0"；不能为 NULL */
} ShotLibProviderV1;

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
    const char* options;         /**< ENUM 用："low|mid|high"；其它类型为 NULL */
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

// ============================================================================
//  能力：UI 提供者  "shot.ui_provider"
// ============================================================================

/**
 * @brief 跨边界事件：一个事件名 + 一段不透明数据，宿主与插件两个方向都用它。
 *
 * data 只在调用期间有效：接收方必须在函数返回前拷走 data_len 字节，
 * 不能把指针存起来慢慢用。name 反过来必须是编译期静态字符串。
 *
 * @note flags 目前没有定义，构造时填 0。
 */
typedef struct ShotEvent {
    uint32_t    struct_size;   /**< = sizeof(ShotEvent)；构造方填 */
    const char* name;          /**< 事件名：编译期静态字符串，活到模块卸载，不能为 NULL */
    const char* data;          /**< 不透明字节；data_len 为 0 时可为 NULL，接收方必须在返回前拷走 */
    uint32_t    data_len;      /**< data 的有效字节数；data 为 NULL 时必须为 0 */
    uint32_t    flags;         /**< 保留，填 0 */
} ShotEvent;

/**
 * @brief UI 提供者能力函数表，对应 SHOT_IFACE_UI_PROVIDER、版本 1。
 *
 * 窗口/托盘归插件自己：它开自己的线程、跑自己的消息循环，宿主只调
 * init / deinit / post_to_plugin 三个函数，完全不碰窗口句柄，也不知道插件有几个线程。
 *
 * @note 三个函数指针都可以为 NULL（= 不提供该步骤），宿主按"没有要做的"处理，
 *       不算失败。
 * @note init 返回 0 时，窗口与线程必须已就绪，宿主之后随时可能投递事件。
 * @note post_to_plugin 可能在宿主的任意线程上被调用；ev 只在调用期间有效，
 *       插件必须在返回前拷走需要的数据。
 */
typedef struct ShotUIProviderV1 {
    uint32_t struct_size;                                        /**< = sizeof(ShotUIProviderV1)；构造方填 */
    uint32_t version;                                            /**< 函数表版本，当前为 1 */
    int32_t (*init)(void* self, const ShotHostServices* host);    /**< 建窗口 + 起线程；0 = 成功 */
    int32_t (*deinit)(void* self);                                /**< 返回前必须停线程、销毁窗口；0 = 成功 */
    int32_t (*post_to_plugin)(void* self, const ShotEvent* ev);   /**< 任意线程可调；0 = 已入队 */
} ShotUIProviderV1;

// ============================================================================
//  能力：屏幕采集  "shot.screen_capture"
// ============================================================================

/**
 * @brief 采集目标的几何与像素格式：宿主据此分配目标缓冲。
 *
 * v1 只描述"整个虚拟桌面"：多显示器时 width/height 是合成后的像素总尺寸，
 * origin_x/origin_y 是左上角在系统坐标系里的位置（主屏左上角不一定是 (0,0)）。
 * 区域采集、单显示器采集留给后续版本（新增字段或新结构体，老字段不动）。
 *
 * @note 字段只在 describe 成功后有效；宿主不应对失败时的内容做任何假设。
 */
typedef struct ShotCaptureInfo {
    uint32_t struct_size;   /**< = sizeof(ShotCaptureInfo)；构造方填，读取方先比对 */
    int32_t  width;         /**< 采集结果像素宽；有效时 >= 1 */
    int32_t  height;        /**< 采集结果像素高；有效时 >= 1 */
    int32_t  origin_x;      /**< 内容左上角在系统坐标里的 x；多显示器时可能为负 */
    int32_t  origin_y;      /**< 内容左上角在系统坐标里的 y；多显示器时可能为负 */
    uint32_t pixel_format;  /**< 结果的像素格式，目前必须是 SHOT_PIXEL_FORMAT_BGRA8 */
} ShotCaptureInfo;

/**
 * @brief 屏幕采集能力函数表，对应 SHOT_IFACE_SCREEN_CAPTURE、版本 1。
 *
 * 两段式：宿主先 describe 拿到尺寸，自己分配一块 BGRA8 缓冲并填好 ShotImage，
 * 再调 capture 让插件往里写像素。插件因此**从不分配图像内存**，也就没有
 * "谁分配谁释放"的跨 DLL 所有权问题；capture 返回后插件不得保存 dst 或其
 * data 指针——下一张图宿主可能换一块完全不同的内存。
 *
 * 线程模型：两个函数都只在宿主调用线程上执行，实现内部若用系统 API 需要自己
 * 保证该线程可用（Windows 上 GDI 抓屏不要求 UI 线程）。
 *
 * 返回码：0 = 成功；1 = 当前没有可采集的屏幕（无显示器/远程会话断开）；
 * 2 = 参数不合法（out/dst 为空、pixel_format 不认识、缓冲尺寸不足）；
 * 3 = 系统调用失败（实现应已写日志）。非 0 时宿主不会使用 dst 的内容。
 *
 * @note 两个函数指针都必须实现；缺一个宿主按"不支持屏幕采集"处理，跳过该插件。
 */
typedef struct ShotScreenCaptureV1 {
    uint32_t struct_size;   /**< = sizeof(ShotScreenCaptureV1)；构造方填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */
    /**
     * @brief 报告采集目标的几何与像素格式。
     * @param[in]  self 插件实例指针。
     * @param[out] out  由调用方提供并填好 struct_size；插件据此判断能写哪些字段。
     * @return 0 = 成功；非 0 见上面的返回码约定。
     */
    int32_t (*describe)(void* self, ShotCaptureInfo* out);
    /**
     * @brief 把屏幕像素写进宿主提供的图像。
     * @param[in] self 插件实例指针。
     * @param[in,out] dst 宿主填好 data/width/height/stride/channels/pixel_format 的图像；
     *                 插件只能写像素，不得改结构体字段。必须按 stride 逐行写，
     *                 stride 为负表示自底向上存储。
     * @return 0 = 成功；非 0 见上面的返回码约定。
     */
    int32_t (*capture)(void* self, ShotImage* dst);
} ShotScreenCaptureV1;

// ============================================================================
//  能力：图像保存  "shot.image_saver"
// ============================================================================

/**
 * @brief 图像保存能力函数表，对应 SHOT_IFACE_IMAGE_SAVER、版本 1。
 *
 * 宿主把处理完的图像交给它，由它决定文件格式、编码参数、目录布局——这些全部
 * 来自插件自己的 shot.configurable 配置项，宿主不硬编码任何保存策略。因此换一个
 * 保存插件（例如只写 BMP 的调试插件、上传到图床的插件）不需要改宿主。
 *
 * directory / basename 是**本次调用的覆盖参数**：为 NULL 时插件使用自己的配置/默认值；
 * 非 NULL 时以传入值为准（设置页/命令行"另存为"会用到）。basename 不含扩展名。
 *
 * path 由宿主提供：成功时插件把最终文件路径（含扩展名，NUL 结尾）写进去；长度不够
 * 时返回 2，宿主应先给 1024 字节这类够用的缓冲。路径以平台习惯编码（Windows UTF-8
 * 落盘前再转宽字符）写入，宿主只用于日志与事件负载。
 *
 * 线程模型：save 只在宿主调用线程上执行，同一实例不会被并发调用；实现无需考虑重入。
 *
 * 返回码：0 = 已落盘；1 = 参数不合法（image 为空/尺寸非法/格式不认识）；
 * 2 = path 缓冲不足；3 = 保存失败（目录不可写、编码失败等，实现应已写日志）。
 *
 * @note save 必须实现；没有它宿主按"没有保存插件"处理并发布 capture.failed。
 */
typedef struct ShotImageSaverV1 {
    uint32_t struct_size;   /**< = sizeof(ShotImageSaverV1)；构造方填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */
    /**
     * @brief 把一张图像保存成文件。
     * @param[in]  self      插件实例指针。
     * @param[in]  image     待保存图像；只读，插件不得修改像素或结构体字段。
     * @param[in]  directory 覆盖输出目录（UTF-8）；NULL = 用插件配置。
     * @param[in]  basename  覆盖文件名（不含扩展名，UTF-8）；NULL = 用插件配置。
     * @param[out] path      接收最终文件路径的缓冲；不能为 NULL。
     * @param[in]  path_size path 的字节数。
     * @return 0 = 成功；非 0 见上面的返回码约定。
     */
    int32_t (*save)(void* self, const ShotImage* image, const char* directory,
                    const char* basename, char* path, uint32_t path_size);
} ShotImageSaverV1;

// ============================================================================
//  宿主 → 插件的服务表
// ============================================================================

/**
 * @brief 调试级日志：开发期细节，正式版通常被过滤。
 */
#define SHOT_LOG_DEBUG 0

/**
 * @brief 普通信息日志。
 */
#define SHOT_LOG_INFO  1

/**
 * @brief 警告日志：有异常但不影响继续运行。
 */
#define SHOT_LOG_WARN  2

/**
 * @brief 错误日志：本次操作失败。
 */
#define SHOT_LOG_ERROR 3

/**
 * @brief 宿主配置读写服务函数表，对应 SHOT_HOST_IFACE_CONFIG、版本 1。
 *
 * 插件通过 host->query_interface 取得这张表，读写的是【宿主】的配置
 * （输出格式、保存目录之类），不是插件自己的配置项。
 *
 * @note 返回值约定与 ShotConfigurableV1 一致：0 = 成功；负数 = key 不认识或参数非法。
 *       实现可能在任意插件线程上被调用。
 */
typedef struct ShotHostConfigV1 {
    uint32_t struct_size;   /**< = sizeof(ShotHostConfigV1)；宿主填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */

    /**
     * @brief 读一个宿主配置项。
     * @param[in]  ctx      原样回传 host_context。
     * @param[in]  key      配置键，不能为 NULL；不认识的键返回负数。
     * @param[out] buf      接收缓冲区，可为 NULL（此时只查长度）。
     * @param[in]  buf_size buf 的字节数；大于返回值时才写入（含结尾 NUL）。
     * @return >= 0 = 值的字节数（不含结尾 NUL）；< 0 = key 不认识或参数非法。
     */
    int32_t (*get_config)(void* ctx, const char* key, char* buf, uint32_t buf_size);

    /**
     * @brief 写一个宿主配置项。
     * @param[in] ctx   原样回传 host_context。
     * @param[in] key   配置键，不能为 NULL。
     * @param[in] value 字符串形式的新值，不能为 NULL。
     * @return 0 = 已生效；< 0 = key 不认识或值非法。
     */
    int32_t (*set_config)(void* ctx, const char* key, const char* value);

    void (*save_config)(void* ctx); /**< 保存配置到磁盘；返回前必须完成，不能只排队 */
} ShotHostConfigV1;

/**
 * @brief 宿主广播事件服务函数表，对应 SHOT_HOST_IFACE_BOARD_MSG、版本 1。
 *
 * 插件把事件广播给宿主；与 ShotHostServices::post_to_host 一样，
 * 返回 0 之前宿主必须已拷走 ev->data 的 data_len 字节。
 */
typedef struct ShotHostBoardEventV1 {
    uint32_t struct_size;   /**< = sizeof(ShotHostBoardEventV1)；宿主填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */

    int32_t (*broadcast_event)(const ShotEvent* ev); /**< 任意线程可调；0 = 已入队，非 0 = 没接收 */
} ShotHostBoardEventV1;

/**
 * @brief 宿主事件总线函数表，对应 SHOT_HOST_IFACE_EVENT_BUS、版本 1。
 *
 * 进程内的主题（topic）发布/订阅：插件 publish 一条事件，宿主在【分发线程】上
 * 依次回调所有匹配的订阅者。它是 post_to_host / broadcast_event 的"可订阅版"：
 * 前者只投给宿主，总线投给所有订阅者（包括宿主自己）。
 *
 * @note 所有函数都可能在插件的任意线程上被调用，宿主实现负责线程安全。
 * @note payload 一律深拷贝：publish 返回后调用方可以立刻释放自己的缓冲区。
 * @warning 回调里不要阻塞、不要做长时间任务：它跑在宿主分发线程上，
 *          阻塞会拖住所有插件的消息。需要耗时就把数据拷走、转到自己的线程。
 */
typedef struct ShotHostEventBusV1 {
    uint32_t struct_size;   /**< = sizeof(ShotHostEventBusV1)；宿主填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */

    /**
     * @brief 发布一条事件给所有订阅者。
     *
     * @param[in] ctx   原样回传 host_context。
     * @param[in] topic 主题，不能为 NULL 或空串；约定见 document/EVENT_BUS.md。
     * @param[in] data  不透明负载，可为 NULL（此时 len 必须为 0）。
     * @param[in] len   负载字节数，上限 64 KiB（大对象走单独的服务，不进总线）。
     * @return 0 = 已入队；-1 = 参数非法、负载超限或队列满（事件被丢弃）。
     * @note 非阻塞：入队即返回；分发顺序按发布顺序（FIFO，单队列）。
     */
    int32_t (*publish)(void* ctx, const char* topic, const void* data, uint32_t len);

    /**
     * @brief 订阅一个主题（或通配模式）。
     *
     * @param[in] ctx      原样回传 host_context。
     * @param[in] topic    主题模式：精确匹配 "capture.request"，前缀通配 "capture.*"，
     *                     或全局通配 "*"。
     * @param[in] callback 匹配时回调；不能为 NULL。
     * @param[in] user     原样回传给 callback 的上下文指针，也是退订的句柄。
     * @return 0 = 订阅成功；-1 = 参数非法。
     * @note 回调在宿主分发线程上调用；在回调里 publish 会排到下一轮，不会重入。
     * @warning 插件必须在 deinit 返回前 unsubscribe；卸载之后宿主不再回调。
     */
    int32_t (*subscribe)(void* ctx, const char* topic,
                         void (*callback)(void* user, const char* topic,
                                          const void* data, uint32_t len),
                         void* user);

    /**
     * @brief 退订某个 user 注册的全部主题。
     * @param[in] ctx  原样回传 host_context。
     * @param[in] user subscribe 时传入的 user 指针。
     * @return 0 = 至少退订了一条；-1 = user 为 NULL 或没有匹配的订阅。
     * @note 正在分发中的一轮可能已经拿到订阅快照：退订保证"不再有新回调"，
     *       不保证"当前这一轮不会有在途回调"。
     */
    int32_t (*unsubscribe)(void* ctx, void* user);
} ShotHostEventBusV1;

// ============================================================================
//  宿主服务：插件注册表  "shot.host.plugin_registry"
// ============================================================================

/**
 * @brief 插件状态常量：已加载，实例存在、能力可用。
 */
#define SHOT_PLUGIN_STATE_LOADED   1u

/**
 * @brief 插件状态常量：已禁用，本次不创建实例（禁用列表已持久化，重启后仍禁用）。
 */
#define SHOT_PLUGIN_STATE_DISABLED 2u

/**
 * @brief 插件状态常量：加载失败，宿主扫描阶段跳过了它（reason 里是原因）。
 */
#define SHOT_PLUGIN_STATE_FAILED   3u

/**
 * @brief 一条已安装插件的状态：注册表服务用它列出插件清单。
 *
 * 以【插件】而不是 DLL 为单位：一个包里两个插件就是两条记录。
 * 字符串由宿主快照持有，下一次调用任意注册表函数后可能失效，调用方必须立即拷走。
 */
typedef struct ShotPluginStatus {
    uint32_t    struct_size;   /**< = sizeof(ShotPluginStatus)；调用方填 */
    const char* id;            /**< 插件 id，如 "example.invert"；不能为 NULL */
    const char* name;          /**< 界面显示名；不能为 NULL */
    const char* version;       /**< 插件版本；加载前就失败时用 metadata 的声明值，可能为空串 */
    const char* path;          /**< 插件 DLL/SO 的完整路径；不能为 NULL */
    uint32_t    state;         /**< SHOT_PLUGIN_STATE_* */
    const char* reason;        /**< 禁用/失败原因；没有原因时为空串，不为 NULL */
} ShotPluginStatus;

/**
 * @brief 宿主插件注册表函数表，对应 SHOT_HOST_IFACE_PLUGIN_REGISTRY、版本 1。
 *
 * 启停请求是【异步】的：set_enabled 只登记变更并立刻返回。宿主的 main 线程在
 * 下一轮做真正的生命周期操作——销毁/创建插件实例、卸载/重新加载 DLL、回灌已存
 * 配置——完成后发布 `plugins.changed`（主题与负载格式见 document/SETTINGS.md）。
 *
 * @note 所有函数都可能在任意插件线程上被调用，宿主实现负责线程安全。
 * @note 被其他已加载插件依赖（metadata 的 requires）的插件不接受禁用：宿主拒绝
 *       该变更，并在 `plugins.changed` 的负载里给出原因，调用方照此提示用户。
 */
typedef struct ShotHostPluginRegistryV1 {
    uint32_t struct_size;   /**< = sizeof(ShotHostPluginRegistryV1)；宿主填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */

    /**
     * @brief 已安装插件（含被禁用与加载失败的）个数。
     * @param[in] ctx 原样回传 host_context。
     * @return 记录条数；没有插件时为 0。
     */
    uint32_t (*installed_count)(void* ctx);

    /**
     * @brief 取第 index 条插件状态。
     * @param[in]  ctx   原样回传 host_context。
     * @param[in]  index 0 .. installed_count()-1。
     * @param[out] out   调用方填好 struct_size；字符串只在本次调用内有效。
     * @return 0 = 成功；1 = index 越界或参数不合法；2 = out 结构太短。
     */
    int32_t (*installed_info)(void* ctx, uint32_t index, ShotPluginStatus* out);

    /**
     * @brief 查询插件当前是否启用。
     * @return 1 = 启用；0 = 禁用；-1 = 没有这个 id。
     */
    int32_t (*is_enabled)(void* ctx, const char* plugin_id);

    /**
     * @brief 请求启用/禁用插件（异步，登记后立即返回）。
     * @param[in] ctx       原样回传 host_context。
     * @param[in] plugin_id 插件 id；不能为 NULL。
     * @param[in] enabled   非 0 = 启用，0 = 禁用。
     * @return 0 = 已登记；-1 = 参数非法或没有这个 id；-2 = 宿主不接受该变更（预留）。
     * @note 同一个 id 重复登记以最后一次为准；一批变更处理完后发布 `plugins.changed`。
     * @warning 不要在这之后立刻假设插件已经卸载：真正执行在宿主 main 线程的下一轮。
     */
    int32_t (*set_enabled)(void* ctx, const char* plugin_id, int32_t enabled);
} ShotHostPluginRegistryV1;

// ============================================================================
//  宿主服务：列表  "shot.host.plugin_lists"
// ============================================================================

/**
 * @brief 宿主列表服务函数表，对应 SHOT_HOST_IFACE_PLUGIN_LISTS、版本 1。
 *
 * 宿主把"插件贡献的数据"按 list id 聚成 JSON 快照，这里只提供只读枚举。
 * 列表的注册、写值、提交都不在这里——它们是 `lists.add` / `lists.remove` /
 * `lists.update` / `lists.commit` 事件（见 document/LISTS.md），由宿主 main 线程
 * 统一处理，所以写路径天然是单线程的。
 *
 * 已知 list id：`config_list`（设置页配置项）、`tray_command_list`（托盘菜单项），
 * 后续新增的列表只需登记 id 与 JSON schema，不需要改本结构体。
 *
 * 线程模型：所有函数可在任意插件线程调用；宿主内部用快照 + 互斥保护，返回的
 * JSON 是调用方自己缓冲里的副本，与列表后续变化无关。
 *
 * @note list_json 用两段式读取：buf 传 NULL、buf_size 传 0 时返回所需字节数
 *       （不含结尾 NUL），再分配缓冲读取；返回负数表示 list id 不认识或缓冲不足。
 */
typedef struct ShotHostPluginListsV1 {
    uint32_t struct_size;   /**< = sizeof(ShotHostPluginListsV1)；宿主填 */
    uint32_t version;       /**< 函数表版本，当前为 1 */

    /**
     * @brief 当前已注册的列表个数。
     * @return 列表个数；宿主没有可读列表时为 0。
     */
    uint32_t (*list_count)(void* ctx);

    /**
     * @brief 取第 index 个列表的 id。
     * @param[in]  index     0 .. list_count()-1。
     * @param[out] buf       接收缓冲；可为 NULL（此时只查长度）。
     * @param[in]  buf_size  buf 的字节数；必须大于 id 长度（给 NUL 留位）。
     * @return >= 0 = id 的字节数（不含 NUL）；负数 = 越界/缓冲不足/参数非法。
     */
    int32_t (*list_id)(void* ctx, uint32_t index, char* buf, uint32_t buf_size);

    /**
     * @brief 取一个列表的 JSON 快照（UTF-8，所需长度不含结尾 NUL）。
     *
     * JSON 形状由 list id 决定，schema 见 document/LISTS.md。宿主内部已解析成
     * 结构保存，序列化结果有缓存，调用方按需读取即可，不要每帧轮询。
     *
     * @param[in]  list_id  列表 id；NULL 或未注册时返回负数。
     * @param[out] buf      接收缓冲；可为 NULL（此时只查长度）。
     * @param[in]  buf_size buf 的字节数；必须大于返回长度（给 NUL 留位）。
     * @return >= 0 = JSON 字节数；负数 = list id 不认识/缓冲不足/参数非法。
     */
    int32_t (*list_json)(void* ctx, const char* list_id, char* buf, uint32_t buf_size);
} ShotHostPluginListsV1;

/**
 * @brief 宿主 → 插件的服务表：插件拿它回调宿主的一切。
 *
 * 宿主构造一张静态实例（全局唯一、线程安全），通过 create(index, host) 交给插件，
 * 有效期覆盖插件实例的整个生命周期，插件可以把指针（以及 host_context）一直存着。
 * 宿主新增服务 = 往这张表末尾追加字段，因此也不需要改契约版本。
 *
 * @note 所有函数都可能在插件的任意线程上被调用，线程安全由宿主实现负责。
 * @warning 插件不得修改这张表，也不得释放它；它属于宿主。
 */
struct ShotHostServices {
    uint32_t struct_size;   /**< = sizeof(ShotHostServices)；读取前先比对 */
    uint32_t abi_major;     /**< 宿主支持的 SHOT_ABI_MAJOR，插件据此判断是否兼容 */
    uint32_t abi_minor;     /**< 宿主支持的 SHOT_ABI_MINOR，插件据此决定要不要降级 */
    uint32_t reserved0;     /**< 保持 8 字节对齐；必须为 0 */

    void* host_context;     /**< 宿主身份，所有服务调用都要原样带上；可为 NULL */

    /**
     * @brief 打一条日志，由宿主决定写到控制台还是文件。
     * @param[in] ctx   原样回传 host_context。
     * @param[in] level SHOT_LOG_*；越界值由宿主兜底。
     * @param[in] msg   日志正文，可为 NULL（此时忽略本次调用）。
     * @param[in] len   msg 的字节数；< 0 表示 msg 以 NUL 结尾，按 strlen 处理。
     * @note 任意线程可调。
     */
    void (*log)(void* ctx, int32_t level, const char* msg, int32_t len);

    /**
     * @brief 向宿主索取一个服务/能力，按字符串 id 查询。
     *
     * 插件要新的宿主服务时走这里，连这张表都不用再长大。
     *
     * @param[in] ctx      原样回传 host_context。
     * @param[in] iface_id 宿主服务 id，如 SHOT_HOST_IFACE_CONFIG，不能为 NULL。
     * @param[in] version  插件认识的服务版本，当前传 1。
     * @return 非 NULL = 指向宿主服务表的指针；NULL = 宿主没这个服务，插件安静跳过。
     * @note 返回的指针在插件实例生命周期内有效，插件只读、不得释放。
     */
    void* (*query_interface)(void* ctx, const char* iface_id, uint32_t version);

    /**
     * @brief 把事件投递给宿主。
     * @param[in] ctx 原样回传 host_context。
     * @param[in] ev  事件，不能为 NULL；只在调用期间有效。
     * @return 0 = 宿主已接收（它必须在返回前拷走 ev->data 的 data_len 字节）；
     *         非 0 = 没接收，插件可丢弃或稍后重试。
     * @note 任意线程可调。
     */
    int32_t (*post_to_host)(void* ctx, const ShotEvent* ev);
};



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

// ============================================================================
//  布局锚点：下面的数字是 ABI。任何一处变动都必须是有意识的（改这里 + 想清楚
//  要不要递增 SHOT_ABI_MAJOR），并且同步更新 document/ABI_POLICY.md。
// ============================================================================
SHOT_STATIC_ASSERT(sizeof(ShotImage) == 32);
SHOT_STATIC_ASSERT(offsetof(ShotImage, pixel_format) == 4);
SHOT_STATIC_ASSERT(offsetof(ShotImage, data) == 8);
SHOT_STATIC_ASSERT(offsetof(ShotImage, channels) == 28);

SHOT_STATIC_ASSERT(sizeof(ShotEvent) == 32);
SHOT_STATIC_ASSERT(offsetof(ShotEvent, name) == 8);
SHOT_STATIC_ASSERT(offsetof(ShotEvent, data) == 16);
SHOT_STATIC_ASSERT(offsetof(ShotEvent, data_len) == 24);

SHOT_STATIC_ASSERT(sizeof(ShotPluginInfo) == 32);
SHOT_STATIC_ASSERT(offsetof(ShotPluginInfo, id) == 8);

SHOT_STATIC_ASSERT(sizeof(ShotImageProcessorV1) == 16);
SHOT_STATIC_ASSERT(offsetof(ShotImageProcessorV1, process) == 8);

SHOT_STATIC_ASSERT(sizeof(ShotLibProviderV1) == 24);
SHOT_STATIC_ASSERT(offsetof(ShotLibProviderV1, get_library_version) == 16);

SHOT_STATIC_ASSERT(sizeof(ShotUIProviderV1) == 32);
SHOT_STATIC_ASSERT(offsetof(ShotUIProviderV1, post_to_plugin) == 24);

SHOT_STATIC_ASSERT(sizeof(ShotCaptureInfo) == 24);
SHOT_STATIC_ASSERT(offsetof(ShotCaptureInfo, width) == 4);
SHOT_STATIC_ASSERT(offsetof(ShotCaptureInfo, origin_x) == 12);
SHOT_STATIC_ASSERT(offsetof(ShotCaptureInfo, pixel_format) == 20);

SHOT_STATIC_ASSERT(sizeof(ShotScreenCaptureV1) == 24);
SHOT_STATIC_ASSERT(offsetof(ShotScreenCaptureV1, describe) == 8);
SHOT_STATIC_ASSERT(offsetof(ShotScreenCaptureV1, capture) == 16);

SHOT_STATIC_ASSERT(sizeof(ShotImageSaverV1) == 16);
SHOT_STATIC_ASSERT(offsetof(ShotImageSaverV1, save) == 8);

SHOT_STATIC_ASSERT(sizeof(ShotConfigDesc) == 72);
SHOT_STATIC_ASSERT(offsetof(ShotConfigDesc, min) == 40);

SHOT_STATIC_ASSERT(sizeof(ShotConfigurableV1) == 40);
SHOT_STATIC_ASSERT(offsetof(ShotConfigurableV1, set_config) == 32);

SHOT_STATIC_ASSERT(sizeof(ShotHostConfigV1) == 32);
SHOT_STATIC_ASSERT(offsetof(ShotHostConfigV1, get_config) == 8);
SHOT_STATIC_ASSERT(offsetof(ShotHostConfigV1, set_config) == 16);
SHOT_STATIC_ASSERT(offsetof(ShotHostConfigV1, save_config) == 24);

SHOT_STATIC_ASSERT(sizeof(ShotHostBoardEventV1) == 16);
SHOT_STATIC_ASSERT(offsetof(ShotHostBoardEventV1, broadcast_event) == 8);

SHOT_STATIC_ASSERT(sizeof(ShotHostEventBusV1) == 32);
SHOT_STATIC_ASSERT(offsetof(ShotHostEventBusV1, publish) == 8);
SHOT_STATIC_ASSERT(offsetof(ShotHostEventBusV1, subscribe) == 16);
SHOT_STATIC_ASSERT(offsetof(ShotHostEventBusV1, unsubscribe) == 24);

SHOT_STATIC_ASSERT(sizeof(ShotPluginStatus) == 56);
SHOT_STATIC_ASSERT(offsetof(ShotPluginStatus, id) == 8);
SHOT_STATIC_ASSERT(offsetof(ShotPluginStatus, state) == 40);
SHOT_STATIC_ASSERT(offsetof(ShotPluginStatus, reason) == 48);

SHOT_STATIC_ASSERT(sizeof(ShotHostPluginRegistryV1) == 40);
SHOT_STATIC_ASSERT(offsetof(ShotHostPluginRegistryV1, installed_count) == 8);
SHOT_STATIC_ASSERT(offsetof(ShotHostPluginRegistryV1, set_enabled) == 32);

SHOT_STATIC_ASSERT(sizeof(ShotHostPluginListsV1) == 32);
SHOT_STATIC_ASSERT(offsetof(ShotHostPluginListsV1, list_count) == 8);
SHOT_STATIC_ASSERT(offsetof(ShotHostPluginListsV1, list_id) == 16);
SHOT_STATIC_ASSERT(offsetof(ShotHostPluginListsV1, list_json) == 24);

SHOT_STATIC_ASSERT(sizeof(ShotHostServices) == 48);
SHOT_STATIC_ASSERT(offsetof(ShotHostServices, log) == 24);
SHOT_STATIC_ASSERT(offsetof(ShotHostServices, post_to_host)==40);

SHOT_STATIC_ASSERT(sizeof(ShotModule) == 64);
SHOT_STATIC_ASSERT(offsetof(ShotModule, destroy) == 48);
SHOT_STATIC_ASSERT(offsetof(ShotModule, query_interface) == 56);
