// 托盘的两个开关配置：notify（保存成功后弹气泡）与 notify_on_error（失败时弹气泡）。
//
// 配置项声明、字符串值解析、当前值读写都在这个文件里；ABI 回调在
// plugin_entry.cpp 只做参数转发。值一律按契约用字符串 "1" / "0" 表达，
// 缺省都是开——托盘默认应该把"截屏成功/失败"告诉用户。
//
// 线程模型：set_config 可能在宿主任意线程上被调用，托盘线程同时可能正在读，
// 所以当前值用 std::atomic<bool>，读侧不加锁也不会读到撕裂的值。

#include "tray_config.h"

#include <cstring>
#include <iterator>

namespace tray {
namespace {

// 配置键：全小写 + 下划线，发布后不能改名（它是存盘标识，见 ABI_POLICY.md §5）。
const char kKeyNotify[] = "notify";
const char kKeyNotifyOnError[] = "notify_on_error";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyNotify,
        .label         = "截屏成功后通知",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "保存完成后在托盘弹气泡；关掉后不再打扰",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyNotifyOnError,
        .label         = "截屏失败时通知",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "采集/处理/保存失败时在托盘弹气泡显示原因",
    },
};

/**
 * @brief 把配置里的布尔文本解析成 bool。
 *
 * 只把 "0" / "false" 当成关：宿主升级后可能传别的写法（"off" 之类），按
 * "不认识就当默认值（开）"处理，比把用户配置的开关意外关掉安全。
 *
 * @param[in] value 配置值文本，不能为空。
 * @return true = 开；false = 关（含 "0" / "false"）。
 * @note 无状态的纯解析函数，不属于 TrayConfig 的接口，所以放匿名命名空间（STYLE §6.7）。
 */
bool parseBool(const char* value) {
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

}  // namespace

TrayConfig::TrayConfig() = default;

std::uint32_t TrayConfig::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t TrayConfig::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t TrayConfig::getValue(const char* key, char* buf, std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyNotify) == 0) {
        return copyValue(m_notifyOnDone.load() ? "1" : "0", buf, bufSize);
    }
    if (std::strcmp(key, kKeyNotifyOnError) == 0) {
        return copyValue(m_notifyOnError.load() ? "1" : "0", buf, bufSize);
    }
    return -1;  // 不认识的 key：安全忽略（宿主升级后多传一项也不会踩这里）
}

std::int32_t TrayConfig::setValue(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyNotify) == 0) {
        m_notifyOnDone.store(parseBool(value));
        return 0;
    }
    if (std::strcmp(key, kKeyNotifyOnError) == 0) {
        m_notifyOnError.store(parseBool(value));
        return 0;
    }
    return -1;
}

bool TrayConfig::notifyOnCaptureDone() const noexcept {
    return m_notifyOnDone.load();
}

bool TrayConfig::notifyOnCaptureError() const noexcept {
    return m_notifyOnError.load();
}

std::int32_t TrayConfig::copyValue(const char* value, char* buf, std::uint32_t bufSize) {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    // 契约：buf_size 大于返回值才写入（含结尾 NUL）；装不下只报长度，不算失败。
    if (buf != nullptr && bufSize > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

std::int32_t TrayConfig::copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (out == nullptr || src == nullptr) {
        return 1;
    }
    // 老宿主的结构可能比本插件认识的短：按 struct_size 判断能写到哪，绝不越界。
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = *src;
    return 0;
}

}  // namespace tray
