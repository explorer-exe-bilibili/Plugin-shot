#pragma once

// ============================================================================
//  region_output.h —— 框选结果的落盘位置（Linux/Wayland 分支）
//
//  Wayland 分支自己裁剪、自己写 PNG（不再经过宿主流水线），所以"存到哪"要跟
//  image_saver 插件的配置保持一致：读 config_list 里 image_saver.main 的
//  directory 项；读不到 / 为空时用系统图片目录（$XDG_PICTURES_DIR，缺省
//  $HOME/Pictures）。文件名固定 shot_YYYYMMDD_HHMMSS.png（本插件约定，不跟随
//  image_saver 的 filename 模板——它是宿主流水线的命名策略）。
//
//  只有 Linux/Wayland 分支编译本文件对应的 .cpp。
// ============================================================================

#include <string>

#include "plugin_api.h"

namespace region_selector {

/**
 * @brief 解析框选 PNG 的保存目录。
 *
 * 优先级：`config_list` 里 `owner = "image_saver.main"`、`id = "directory"` 的
 * `value`（相对路径相对宿主工作目录，与 image_saver 一致）→ 系统图片目录。
 * 目录不存在时会尝试创建（`g_mkdir_with_parents`）。
 *
 * @param[in] host 宿主服务表；可为 nullptr（此时直接用系统图片目录）。
 * @return 可写的目录路径；一个候选都没有或建不出来时返回空串（调用方放弃本次保存）。
 * @note 会在日志里记录最终选的目录，真机排查"图存哪了"时先看这一行。
 */
std::string resolveOutputDirectory(const ShotHostServices* host);

/**
 * @brief 在目录里生成 `shot_YYYYMMDD_HHMMSS.png` 的完整路径。
 *
 * @param[in] directory 已存在的目录（resolveOutputDirectory 的返回值）。
 * @return 完整路径；目录为空 / 本地时间取不到时返回空串。
 * @note 时间用本地时区的 `localtime_r`，不是 UTC；同一秒内两次保存会落到同一个
 *       文件名（覆盖前一张），这是刻意保持的固定命名约定。
 */
std::string makeOutputPngPath(const std::string& directory);

}  // namespace region_selector
