#pragma once

// ============================================================================
//  Plugin-shot 插件契约（汇总头）
//
//  插件只写 #include "plugin_api.h"：正文按主题拆在 plugin_api/ 下的子头里，
//  这里只负责按依赖顺序 include，不放任何定义。
//
//    basics.h         三条纪律、SHOT_ABI_*、SHOT_HAS_FIELD / SHOT_REQUIRES、日志级别
//    interfaces.h     能力与宿主服务的 interface id（ABI_POLICY 登记表的代码副本）
//    image.h          图像数据契约
//    plugin_info.h    插件身份证 + 元数据尾部标记
//    <能力>.h         六个能力的函数表：image_processor / lib_provider / configurable /
//                     ui_provider / screen_capture / image_saver
//    host_services.h  宿主 → 插件的服务表
//    module.h         ShotModule 与唯一导出符号 shot_module_entry
//    abi_layout.h     布局锚点（static_assert），必须最后 include
//
//  为什么拆：单文件已经一千多行，改一个能力要滚很久；拆开以后"改契约 = 改对应
//  子头"，布局锚点集中一处仍可直接当 ABI 的机器可读副本。
//  纯 C、append-only 的规则不变（见 basics.h 开头的三条纪律）。
//
//  注意：构建期脚本按【文本】读契约（cmake/ReadContractHeader.cmake 拼汇总头 +
//  子头），所以子头里的宏名/字面量格式不要随便改，plugin_api/ 下也不要再嵌套目录。
// ============================================================================

#include "plugin_api/basics.h"
#include "plugin_api/interfaces.h"
#include "plugin_api/image.h"
#include "plugin_api/plugin_info.h"
#include "plugin_api/image_processor.h"
#include "plugin_api/lib_provider.h"
#include "plugin_api/configurable.h"
#include "plugin_api/ui_provider.h"
#include "plugin_api/screen_capture.h"
#include "plugin_api/image_saver.h"
#include "plugin_api/host_services.h"
#include "plugin_api/module.h"
#include "plugin_api/abi_layout.h"
