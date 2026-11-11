#pragma once

#include <stddef.h>
#include <stdint.h>
#include "plugin_api/basics.h"
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
