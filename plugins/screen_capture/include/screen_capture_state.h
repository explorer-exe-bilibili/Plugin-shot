#pragma once

// ============================================================================
//  screen_capture_state.h —— 内部头：插件实例状态
//
//  **不是插件对外接口**：只被本插件的 plugin_entry.cpp 与 capture_settings.cpp 包含。
//
//  为什么单独成文件：抓屏回调（plugin_entry.cpp）与配置回调（capture_settings.cpp）
//  必须操作同一份实例状态——配置写的是"下一次抓屏要不要画光标"，两个 .cpp 各定义
//  一份状态会让 set_config 写不到抓屏读的那份（STYLE.md §6.8：跨 .cpp 共享的内部
//  类型进独立内部头）。
// ============================================================================

#include "capture_settings.h"
#include "plugin_api.h"
#include "screen_capture.h"

namespace screen_capture {

/**
 * @brief 插件实例状态：可配置项 + 抓屏能力对象。
 *
 * 宿主 create 一次、destroy 一次，只拿一个不透明指针。抓屏在流水线 worker 线程
 * 同步执行，set_config 在宿主 main 线程执行，两者只通过 CaptureSettings 内部的
 * atomic 通信；ScreenCapture 自身没有跨调用状态，GDI 句柄都在单次抓屏内释放。
 */
struct ScreenCaptureState {
    /**
     * @brief 构造：绑定宿主服务表（只用于写日志），配置项取默认值。
     * @param[in] host 宿主服务表，可为 nullptr（老宿主没有服务表，采集照常）。
     */
    explicit ScreenCaptureState(const ShotHostServices* host) noexcept : capture(host) {}

    CaptureSettings settings;  /**< 配置项状态（include_cursor）。 */
    ScreenCapture capture;     /**< 抓屏能力对象；不持有 GDI 句柄。 */
};

}  // namespace screen_capture
