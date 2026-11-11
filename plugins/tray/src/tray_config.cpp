// 托盘的配置项：icon（自定义图标路径）。
//
// 配置项声明、字符串值读写都在这个文件里；ABI 回调在 plugin_entry.cpp 只做参数
// 转发。值一律按契约用字符串表达：icon → UTF-8 路径，空串 = 用宿主应用图标。
//
// 提示（成功/失败气泡）不归托盘管：那条职责已经整体交给独立的消息提示插件
// plugins/notify，托盘只保留图标 + 右键菜单。
//
// 线程模型：set_config 可能在宿主任意线程上被调用（init 的 apply_stored、设置页
// 应用），托盘线程同时可能正在读图标路径；所以路径用 mutex 保护，读侧拿副本。

#include "tray_config.h"

#include <cstring>
#include <iterator>

namespace tray {
namespace {

// 配置键：全小写 + 下划线，发布后不能改名（它是存盘标识，见 ABI_POLICY.md §5）。
// label / tooltip 是文案 key：设置页按 display(owner,key) 用 i18n_list 解析，
// 所以这里只写 key、不写中文（document/I18N.md）。
const char kKeyIcon[] = "icon";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_PATH,
        .key           = kKeyIcon,
        .label         = "config.icon.label",
        .default_value = "",
        // PATH 的对话框类型：设置页据此弹"选择文件"，而不是按当前值猜目录/文件。
        .options       = "file",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "config.icon.tooltip",
    },
};

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
    if (std::strcmp(key, kKeyIcon) == 0) {
        const std::string path = iconPath();  // 锁内拷一份，避免持有锁时做字符串操作
        return copyValue(path.c_str(), buf, bufSize);
    }
    return -1;  // 不认识的 key：安全忽略（宿主升级后多传一项也不会踩这里）
}

std::int32_t TrayConfig::setValue(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyIcon) == 0) {
        std::lock_guard<std::mutex> lock(m_iconMutex);
        m_iconPath = value;
        return 0;
    }
    return -1;
}

std::string TrayConfig::iconPath() const {
    std::lock_guard<std::mutex> lock(m_iconMutex);
    return m_iconPath;
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
