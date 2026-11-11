#pragma once

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
