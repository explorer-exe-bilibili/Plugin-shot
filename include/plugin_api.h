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
//
//  abi_major：布局或语义不兼容时递增，宿主只接受 major 相同的插件。
//  abi_minor：仅追加能力时递增，宿主接受 minor <= 自己的插件。
//  只要遵守上面三条纪律，追加能力不需要动 major。
// ============================================================================
#define SHOT_ABI_MAJOR 1
#define SHOT_ABI_MINOR 1

// ============================================================================
//  元数据尾部标记
//
//  构建期由 cmake/AppendPluginMeta.cmake 把 metadata.json 贴到 DLL/SO 末尾：
//
//      <原始二进制> \n---PLUGIN_META_START---\n {json} \n---PLUGIN_META_END---\n
//
//  PE/ELF 加载器只读文件头的节表，文件尾这段额外数据被完全忽略，因此追加不影响
//  装载。元数据跟着文件走，用户复制/重命名插件也不会丢。
//  这两行是构建脚本（构建期直接读本文件的宏值）与日后宿主读取逻辑的唯一约定，
//  格式必须保持为  #define <名字> "<字面量>"。
// ============================================================================
#define PLUGIN_META_START_MARKER "---PLUGIN_META_START---"
#define PLUGIN_META_END_MARKER   "---PLUGIN_META_END---"

// ============================================================================
//  能力（interface）id
//
//  query_interface(instance, id, version) 用这些字符串询问"你有没有这个能力"：
//  返回非空 = 有（指向该能力的函数表），返回 NULL = 没有，宿主安静跳过。
//
//  新增一类能力 = 在这一段里加一个 id + 下面加一个结构体，不需要改动任何已有
//  结构体，因此也不需要改版本号。所有 id 必须登记到 document/ABI_POLICY.md。
// ============================================================================
#define SHOT_IFACE_IMAGE_PROCESSOR  "shot.image_processor"
#define SHOT_IFACE_CONFIGURABLE     "shot.configurable"
#define SHOT_IFACE_LIB_PROVIDER     "shot.lib_provider"
#define SHOT_IFACE_UI_PROVIDER      "shot.ui_provider"

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

// ptr 指向的类型里，读完 field 需要多少字节。
#define SHOT_REQUIRES(type, field) \
    ((uint32_t)(offsetof(type, field) + sizeof(((type*)0)->field)))

// 读任何字段之前先过一遍：对方的结构够长，字段才存在。
#define SHOT_HAS_FIELD(ptr, type, field) \
    ((ptr) != NULL && (ptr)->struct_size >= SHOT_REQUIRES(type, field))

// 布局锚点：任何一次改动已有字段的位置或类型都会在这里编译失败，
// 从而把"追加字段"变成一次有意识的行为。
#if defined(__cplusplus)
    #define SHOT_STATIC_ASSERT(cond) static_assert(cond, #cond)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define SHOT_STATIC_ASSERT(cond) _Static_assert(cond, #cond)
#else
    #define SHOT_STATIC_ASSERT(cond) /* 该编译器不支持静态断言 */
#endif

// ============================================================================
//  图像：处理类插件的数据契约
//
//  · pixel_format 显式声明像素布局，不再靠 channels 猜。当前只定义 BGRA8。
//  · stride 以字节计，可以为负（自底向上存储），其绝对值也可以大于
//    width * channels（每行尾部有填充）。必须按行寻址，不能假设 data 连续。
//  · 可用字节数至少为 (height-1)*|stride| + width*channels。
// ============================================================================
#define SHOT_PIXEL_FORMAT_BGRA8 1u

typedef struct ShotImage {
    uint32_t struct_size;   // = sizeof(ShotImage)
    uint32_t pixel_format;  // SHOT_PIXEL_FORMAT_*
    uint8_t* data;          // 首行第一个像素；行序由 stride 的正负决定
    int32_t  width;
    int32_t  height;
    int32_t  stride;        // 字节为单位，可负
    int32_t  channels;      // 每像素通道数；BGRA8 为 4
} ShotImage;

// ============================================================================
//  插件身份证：宿主不创建实例也能列出清单
// ============================================================================
typedef struct ShotPluginInfo {
    uint32_t    struct_size;   // = sizeof(ShotPluginInfo)
    uint32_t    abi_minor;     // 该插件实现的次版本
    const char* id;            // "author.name"，与 metadata.json 的 plugins[] 对应
    const char* name;          // 显示名
    const char* version;       // 插件自己的语义化版本，如 "1.0.0"
} ShotPluginInfo;

// ============================================================================
//  能力：图像处理器  "shot.image_processor"
//
//  process 的返回值：0 = 成功，非 0 = 失败（宿主把它当"这个插件没处理成功"）。
//  实现必须按行寻址，并尊重 pixel_format / channels。
// ============================================================================
typedef struct ShotImageProcessorV1 {
    uint32_t struct_size;   // = sizeof(ShotImageProcessorV1)
    uint32_t version;       // = 1
    int32_t  (*process)(void* self, ShotImage* image);
} ShotImageProcessorV1;

// ============================================================================
//  能力：运行库提供者  "shot.lib_provider"
//
//  有些包不提供任何宿主可调用的算法，只负责把某个运行库带进进程（典型：OpenCV）。
//  它用这个能力表明自己是"库/环境提供者"，而不是一个坏掉的图像处理器；
//  宿主见到它就按 PluginType::Lib 登记，名字与版本用于界面显示和依赖校验。
//
//  两个返回值都是插件内的静态字符串，有效期到 DLL 卸载；宿主只读，不释放。
//  追加能力时在这个结构体末尾加字段即可（abi_minor 按需递增）。
// ============================================================================
typedef struct ShotLibProviderV1 {
    uint32_t struct_size;                           // = sizeof(ShotLibProviderV1)
    uint32_t version;                               // = 1
    const char* (*get_library_name)(void* self);    // "opencv"
    const char* (*get_library_version)(void* self); // "4.12.0"
} ShotLibProviderV1;

// ============================================================================
//  能力：可配置  "shot.configurable"
//
//  配置项的值一律用【字符串】表达：跨边界没有宽度与对齐问题，天然可以存成
//  JSON，插件也就不用为了读配置而引入 STL。类型信息由 ShotConfigDesc 描述，
//  宿主据此生成通用界面。
//
//  get_config / set_config 遇到不认识的 key 必须安全忽略（返回负值）：
//  这样宿主升级后多传一个配置项，老插件不会出问题。
// ============================================================================
#define SHOT_CONFIG_KIND_BOOL   1u
#define SHOT_CONFIG_KIND_INT    2u
#define SHOT_CONFIG_KIND_FLOAT  3u
#define SHOT_CONFIG_KIND_STRING 4u
#define SHOT_CONFIG_KIND_ENUM   5u
#define SHOT_CONFIG_KIND_COLOR  6u
#define SHOT_CONFIG_KIND_PATH   7u

typedef struct ShotConfigDesc {
    uint32_t    struct_size;     // = sizeof(ShotConfigDesc)
    uint32_t    kind;            // SHOT_CONFIG_KIND_*
    const char* key;             // "keep_alpha"：读写与存盘的唯一标识
    const char* label;           // 界面显示名
    const char* default_value;   // 一律字符串表达："1" / "128" / "low"
    const char* options;         // ENUM 用："low|mid|high"；其它为 NULL
    double      min;             // INT / FLOAT 用
    double      max;
    double      step;
    const char* tooltip;         // 可为 NULL
} ShotConfigDesc;

typedef struct ShotConfigurableV1 {
    uint32_t struct_size;   // = sizeof(ShotConfigurableV1)
    uint32_t version;       // = 1

    // 配置项个数。
    uint32_t (*get_config_count)(void* self);

    // 第 index 项的声明；返回 0 = 成功，非 0 = index 越界。
    int32_t (*get_config_desc)(void* self, uint32_t index, ShotConfigDesc* out);

    // 读当前值。返回值 >= 0 表示值的字节数（不含结尾 NUL）；此时若 buf 非空且
    // buf_size > 返回值，值已写入 buf（含 NUL）。返回 < 0 表示 key 不认识。
    int32_t (*get_config)(void* self, const char* key, char* buf, uint32_t buf_size);

    // 写一个值。返回 0 = 已生效（或已记录，下次 process 生效）；< 0 = key 不认识
    // 或值非法。可能在运行中被调用（用户拖滑块），实现要自行保证线程安全，
    // 或声明为"下次 process 生效"。
    int32_t (*set_config)(void* self, const char* key, const char* value);
} ShotConfigurableV1;

// ============================================================================
//  能力：UI 提供者  "shot.ui_provider"
//
//  窗口/托盘归插件自己：宿主只调 init / deinit / post_to_plugin 三个函数，
//  完全不碰窗口句柄，也不知道插件有几个线程。
// ============================================================================
typedef struct ShotEvent {
    uint32_t    struct_size;   // = sizeof(ShotEvent)
    const char* name;          // 事件名：必须是编译期静态字符串，活到模块卸载
    const char* data;          // 不透明字节；接收方必须在调用返回前拷走 data_len 字节
    uint32_t    data_len;
    uint32_t    flags;         // 保留，填 0
} ShotEvent;

typedef struct ShotUIProviderV1 {
    uint32_t struct_size;                                        // = sizeof(ShotUIProviderV1)
    uint32_t version;                                            // = 1
    int32_t (*init)(void* self, const ShotHostServices* host);    // 建窗口+起线程；0 = 成功
    int32_t (*deinit)(void* self);                                // 返回前必须停线程、销毁窗口；0 = 成功
    int32_t (*post_to_plugin)(void* self, const ShotEvent* ev);   // 任意线程可调；0 = 已入队
} ShotUIProviderV1;

// ============================================================================
//  宿主 → 插件的服务表
//
//  宿主构造一张静态实例，通过 create(index, host) 传给插件，有效期覆盖插件实例
//  的整个生命周期，插件可以把它（以及其中的 host_context）一直存着。
//  宿主新增服务 = 往这张表末尾追加字段，因此也不需要改契约版本。
// ============================================================================
#define SHOT_LOG_DEBUG 0
#define SHOT_LOG_INFO  1
#define SHOT_LOG_WARN  2
#define SHOT_LOG_ERROR 3

struct ShotHostServices {
    uint32_t struct_size;   // = sizeof(ShotHostServices)
    uint32_t abi_major;     // 宿主支持的 major
    uint32_t abi_minor;     // 宿主支持的 minor —— 插件据此决定要不要降级
    uint32_t reserved0;     // 保持 8 字节对齐；必须为 0

    void* host_context;     // 宿主身份，所有服务调用都带上它

    // 长度前缀的日志；len < 0 表示 msg 以 NUL 结尾。
    void (*log)(void* ctx, int32_t level, const char* msg, int32_t len);

    // 向宿主索取一个能力：返回非空 = 宿主有这个服务，否则 NULL。
    // 插件新要一个宿主服务时走这里，连这张表都不用再长大。
    void* (*query_interface)(void* ctx, const char* iface_id, uint32_t version);

    // 向宿主投递一个事件。任意线程可调。返回 0 = 宿主已接收（它必须在返回前
    // 拷走 ev->data 的 data_len 字节）；非 0 = 没接收，插件可丢弃或稍后重试。
    int32_t (*post_to_host)(void* ctx, const ShotEvent* ev);
};

// ============================================================================
//  模块表：唯一的导出符号 shot_module_entry() 返回它
//
//  读取方（宿主）规则：先看 struct_size，再用 SHOT_HAS_FIELD 判断想用的字段在
//  不在；函数指针为 NULL 表示该插件不提供这个能力，调用前必须判空。
//
//  init / shutdown / get_info 都可以省略（写成 NULL）：
//    init     宿主读完本表后调用一次，用来做版本协商与模块级初始化；
//             返回非 0 表示"这个 DLL 无法在本宿主上工作"，宿主直接卸载它。
//    shutdown 宿主卸载本 DLL 前调用一次。
//    get_info 不创建实例就能拿到第 index 个插件的身份证。
// ============================================================================
struct ShotModule {
    uint32_t struct_size;   // = sizeof(ShotModule)
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t plugin_count;  // 本 DLL 里的插件个数，索引 0..plugin_count-1

    int32_t (*init)(const ShotHostServices* host, uint32_t flags);
    void    (*shutdown)(void);
    int32_t (*get_info)(uint32_t index, ShotPluginInfo* out);
    void*   (*create)(uint32_t index, const ShotHostServices* host);
    void    (*destroy)(void* instance);
    void*   (*query_interface)(void* instance, const char* iface_id, uint32_t version);
};

// ============================================================================
//  导出契约：一个动态库（插件包）只导出下面这【一个】C 函数。
//
//  一个 DLL 可以装多个插件，宿主靠 plugin_count + create(index) 枚举，
//  靠 query_interface 判断每个实例会什么。metadata.json 的 plugins[] 顺序
//  必须与索引一一对应。
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif

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

SHOT_STATIC_ASSERT(sizeof(ShotConfigDesc) == 72);
SHOT_STATIC_ASSERT(offsetof(ShotConfigDesc, min) == 40);

SHOT_STATIC_ASSERT(sizeof(ShotConfigurableV1) == 40);
SHOT_STATIC_ASSERT(offsetof(ShotConfigurableV1, set_config) == 32);

SHOT_STATIC_ASSERT(sizeof(ShotHostServices) == 48);
SHOT_STATIC_ASSERT(offsetof(ShotHostServices, log) == 24);
SHOT_STATIC_ASSERT(offsetof(ShotHostServices, post_to_host)==40);

SHOT_STATIC_ASSERT(sizeof(ShotModule) == 64);
SHOT_STATIC_ASSERT(offsetof(ShotModule, destroy) == 48);
SHOT_STATIC_ASSERT(offsetof(ShotModule, query_interface) == 56);
