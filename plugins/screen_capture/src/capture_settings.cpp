// include_cursor 配置项：声明 + shot.configurable 的 get/set 回调。
//
// 这里只放"配置"这一个关注点：配置描述符、值的串/布尔转换与线程安全状态都在
// CaptureSettings（capture_settings.h）；抓屏本身在 gdi_desktop_capture.cpp，
// ABI 函数表在 plugin_entry.cpp 里组装。
//
// label / tooltip 写的是语言表 key，不是中文：设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析，文案本体在
// plugin_entry.cpp 的 kLangTables 里（两边的 key 拼写必须严格一致）。
//
// 返回码按 ShotConfigurableV1 的契约：get 返回 >= 0 是值的字节数（不含 NUL），
// 负数只表示 key 不认识或参数非法；set 返回 0 = 已生效。不认识的 key 必须安全
// 忽略——宿主升级后多灌一项配置时，老插件不能因此坏掉。

#include "capture_settings.h"

#include <cstring>
#include <iterator>

#include "plugin_entry.h"
#include "screen_capture_state.h"

namespace screen_capture {
namespace {

const char kKeyIncludeCursor[] = "include_cursor";
const char kKeySource[] = "source";

// 文案 key：命名规范 config.<配置 key>.label / config.<配置 key>.tooltip。
// 写成常量是为了让 kConfig 一眼能看出这里放的是 key 而不是最终显示文本。
const char kLabelIncludeCursor[] = "config.include_cursor.label";
const char kTooltipIncludeCursor[] = "config.include_cursor.tooltip";
const char kLabelSource[] = "config.source.label";
const char kTooltipSource[] = "config.source.tooltip";

// source 的取值（ENUM）：Wayland 门户一次只共享一个输出或一个窗口。
const char kSourceMonitor[] = "monitor";
const char kSourceWindow[] = "window";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyIncludeCursor,
        .label         = kLabelIncludeCursor,
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = kTooltipIncludeCursor,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeySource,
        .label         = kLabelSource,
        .default_value = kSourceMonitor,
        .options       = "monitor|window",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipSource,
    },
};

// ---- 下面两个只服务于配置项读写，与任何类无关，放匿名命名空间即可 ----

// 按契约把字符串写进宿主缓冲区，并报告值的字节数（不含结尾 NUL）。
// buf 为空或装不下时只报告长度、不写：宿主据此扩容后重试，不算失败。
std::int32_t copyValue(char* buf, std::uint32_t bufSize, const char* value) {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    if (buf != nullptr && bufSize > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

// 把插件侧的静态配置声明拷进宿主给的 ShotConfigDesc。
// 宿主结构可能比本实现老：装不下 tooltip 就报 2，绝不写到调用方内存之外。
std::int32_t copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (out == nullptr || src == nullptr) {
        return 1;
    }
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = *src;
    return 0;
}

}  // namespace

std::uint32_t capture_config_count(void* /*self*/) {
    // 配置项数量与实例无关：宿主枚举配置目录时不保证带实例，所以这里不看 self。
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t capture_config_desc(void* /*self*/, std::uint32_t index, ShotConfigDesc* out) {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t capture_config_get(void* self, const char* key, char* buf, std::uint32_t bufSize) {
    if (key == nullptr) {
        return -1;
    }
    const auto* state = static_cast<const ScreenCaptureState*>(state_of(self));
    if (std::strcmp(key, kKeyIncludeCursor) == 0) {
        // 没有实例时按默认值回答（true）：宿主可能只查长度，不应该因此失败。
        const bool enabled = (state == nullptr) ? true : state->settings.includeCursor();
        return copyValue(buf, bufSize, enabled ? "1" : "0");
    }
    if (std::strcmp(key, kKeySource) == 0) {
        // 没有实例时按默认值回答（monitor）。
        const bool window = (state == nullptr) ? false : state->settings.captureWindow();
        return copyValue(buf, bufSize, window ? kSourceWindow : kSourceMonitor);
    }
    return -1;  // 不认识的 key：安全忽略，宿主升级也不会踩到这里
}

std::int32_t capture_config_set(void* self, const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    auto* state = static_cast<ScreenCaptureState*>(state_of(self));
    if (state == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyIncludeCursor) == 0) {
        // 布尔值只接受契约约定的四种写法（与 shot::Config::getBool 的读法对称）：
        // 把 "yes"/"2" 静默当成 true/false 会让手改配置的人以为写错了也没事。
        if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
            std::strcmp(value, "TRUE") == 0) {
            state->settings.setIncludeCursor(true);
            return 0;
        }
        if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
            std::strcmp(value, "FALSE") == 0) {
            state->settings.setIncludeCursor(false);
            return 0;
        }
        return -1;  // 非法取值：拒绝，保留原值，宿主可以继续工作
    }
    if (std::strcmp(key, kKeySource) == 0) {
        // ENUM 只接受描述符里声明的两个值；大小写也按声明精确匹配。
        if (std::strcmp(value, kSourceMonitor) == 0) {
            state->settings.setCaptureWindow(false);
            return 0;
        }
        if (std::strcmp(value, kSourceWindow) == 0) {
            state->settings.setCaptureWindow(true);
            return 0;
        }
        return -1;
    }
    return -1;  // 不认识的 key：安静忽略，宿主升级后仍可工作
}

}  // namespace screen_capture
