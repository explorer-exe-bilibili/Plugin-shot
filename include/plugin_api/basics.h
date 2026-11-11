#pragma once

#include <stddef.h>
#include <stdint.h>

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
