#pragma once

#include <stdint.h>
#include "plugin_api/ui_provider.h"

// ============================================================================
//  宿主 → 插件的服务表
// ============================================================================

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
