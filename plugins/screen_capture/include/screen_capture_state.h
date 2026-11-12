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

#include <memory>

#include "capture_settings.h"
#include "plugin_api.h"
#include "screen_capture.h"

namespace shot {
class I18n;  // 多语言封装：完整定义在 shot_plugin_helpers/shot_i18n.h，这里只存指针
}  // namespace shot

namespace screen_capture {

/**
 * @brief 插件实例状态：语言表 + 可配置项 + 抓屏能力对象。
 *
 * 宿主 create 一次、destroy 一次，只拿一个不透明指针。抓屏在流水线 worker 线程
 * 同步执行，set_config 在宿主 main 线程执行，两者只通过 CaptureSettings 内部的
 * atomic 通信。Windows 的 GDI 句柄与 X11 的 Display 都在单次抓屏内释放/关闭；
 * Linux/Wayland 实现持有门户会话与两个后台线程，由 ScreenCapture 析构时停流、
 * 结束会话并 join。
 */
struct ScreenCaptureState {
    /**
     * @brief 构造：绑定宿主服务表（只用于写日志），配置项取默认值。
     * @param[in] host 宿主服务表，可为 nullptr（老宿主没有服务表，采集照常）。
     * @note 定义在 plugin_entry.cpp 而不是这里：`std::unique_ptr<shot::I18n>` 在构造
     *       函数的异常清理路径里也要析构成员，MSVC 会因此在只有前置声明的 TU 里报
     *       C2027（"使用了未定义类型"）。
     */
    explicit ScreenCaptureState(const ShotHostServices* host) noexcept;

    /**
     * @brief 析构：释放语言表（I18n 析构会退订 lists.changed）。
     *
     * 定义在 plugin_entry.cpp 而不是这里：`std::unique_ptr<shot::I18n>` 的删除器需要
     * 完整类型，而这个内部头也被 capture_settings.cpp 包含——不能为了一个成员把所有
     * TU 都拖进 nlohmann-json。
     */
    ~ScreenCaptureState();

    std::unique_ptr<shot::I18n> i18n;  /**< 语言表：create_instance 里构造，destroy_state 里 reset。 */
    CaptureSettings settings;          /**< 配置项状态（include_cursor）。 */
    ScreenCapture capture;             /**< 抓屏能力对象；X11/Windows 不持有跨调用资源。 */
};

}  // namespace screen_capture
