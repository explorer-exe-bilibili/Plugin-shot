#pragma once

#include <stdint.h>
#include "plugin_api/basics.h"

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
