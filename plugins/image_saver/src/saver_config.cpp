// 四个配置项：输出目录 / 格式 / 质量 / 文件名模板。
//
// 跨边界一律用字符串（契约：只传定宽类型 + const char*），本文件负责字符串与内部
// 类型的校验转换：非法值安静回退默认、不认识的 key 返回负值，宿主升级后多传一项
// 老插件也不会坏（document/ABI_POLICY.md §5）。
//
// 返回码约定（include/plugin_api.h 的 ShotConfigurableV1）：get 返回 >= 0 是值长度
// （不含 NUL），负数只表示 key 不认识或参数非法；set 返回 0 = 已生效。

#include "saver_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>

namespace image_saver {
namespace {

const char kKeyDirectory[] = "directory";
const char kKeyFormat[] = "format";
const char kKeyQuality[] = "quality";
const char kKeyFilename[] = "filename";

// 默认值只在描述符里写一遍：构造函数也从这几个常量取初始值，避免"描述符默认值"
// 与"初始值"两处漂移。
const char kDefaultDirectory[] = "";  // 空 = 系统图片目录
const char kDefaultFormat[] = "png";
const char kDefaultQuality[] = "90";
const char kDefaultFilename[] = "shot_%Y%m%d_%H%M%S";

constexpr int kMinQuality = 1;
constexpr int kMaxQuality = 100;

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_PATH,
        .key           = kKeyDirectory,
        .label         = "保存目录",
        .default_value = kDefaultDirectory,
        // PATH 借 options 声明对话框类型（document/SETTINGS.md 的 kind 表）：
        // 保存目录必须选目录，否则默认值为空时设置页会弹成"选文件"。
        .options       = "directory",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "留空使用系统图片目录（Windows: %USERPROFILE%\\Pictures）",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeyFormat,
        .label         = "保存格式",
        .default_value = kDefaultFormat,
        .options       = "png|jpeg",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "PNG 无损；JPEG 有损、文件更小（质量见下一项）",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_INT,
        .key           = kKeyQuality,
        .label         = "JPEG 质量",
        .default_value = kDefaultQuality,
        .options       = nullptr,
        .min           = static_cast<double>(kMinQuality),
        .max           = static_cast<double>(kMaxQuality),
        .step          = 1.0,
        .tooltip       = "1 = 最小文件，100 = 最好画质；PNG 忽略此项",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_STRING,
        .key           = kKeyFilename,
        .label         = "文件名模板",
        .default_value = kDefaultFilename,
        .options       = nullptr,
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "不含扩展名；支持 %Y %m %d %H %M %S，如 shot_%Y%m%d_%H%M%S",
    },
};

}  // namespace

SaverConfig::SaverConfig() {
    // 构造期间没有并发，直接按默认常量填；不走 set() 是为了让"默认值"只依赖上面
    // 那四个常量（set() 还要做字符串比较，构造函数里没必要）。
    m_values.directory = kDefaultDirectory;
    m_values.format = kDefaultFormat;
    m_values.quality = std::atoi(kDefaultQuality);
    m_values.filename = kDefaultFilename;
}

std::uint32_t SaverConfig::itemCount() noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t SaverConfig::describe(std::uint32_t index, ShotConfigDesc* out) noexcept {
    if (out == nullptr || index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t SaverConfig::get(const char* key, char* buf, std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::strcmp(key, kKeyDirectory) == 0) {
        return copyValue(buf, bufSize, m_values.directory.c_str());
    }
    if (std::strcmp(key, kKeyFormat) == 0) {
        return copyValue(buf, bufSize, m_values.format.c_str());
    }
    if (std::strcmp(key, kKeyQuality) == 0) {
        char text[16];
        const int written = std::snprintf(text, sizeof(text), "%d", m_values.quality);
        if (written <= 0) {
            return -1;
        }
        return copyValue(buf, bufSize, text);
    }
    if (std::strcmp(key, kKeyFilename) == 0) {
        return copyValue(buf, bufSize, m_values.filename.c_str());
    }
    return -1;  // 不认识的 key：安静忽略，宿主升级后仍可工作
}

std::int32_t SaverConfig::set(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::strcmp(key, kKeyDirectory) == 0) {
        // 目录允许空串（= 系统图片目录），也允许相对路径（保存时相对宿主当前目录）。
        m_values.directory = value;
        return 0;
    }
    if (std::strcmp(key, kKeyFormat) == 0) {
        if (std::strcmp(value, "png") != 0 && std::strcmp(value, "jpeg") != 0) {
            return -1;  // 非法取值：拒绝，保留原值，宿主可以继续工作
        }
        m_values.format = value;
        return 0;
    }
    if (std::strcmp(key, kKeyQuality) == 0) {
        // 整串必须是十进制数字；越界时夹到 [1,100]，而不是拒绝——用户手改配置写成
        // 999/0 时仍然能保存，只是按合法质量编码。
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end == value || end == nullptr || *end != '\0') {
            return -1;
        }
        if (parsed < kMinQuality) {
            m_values.quality = kMinQuality;
        } else if (parsed > kMaxQuality) {
            m_values.quality = kMaxQuality;
        } else {
            m_values.quality = static_cast<int>(parsed);
        }
        return 0;
    }
    if (std::strcmp(key, kKeyFilename) == 0) {
        // 清空模板等于恢复默认模板：否则保存时还得再判断一次空串。
        m_values.filename = (value[0] == '\0') ? kDefaultFilename : value;
        return 0;
    }
    return -1;  // 不认识的 key：安静忽略
}

SaverConfig::Snapshot SaverConfig::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_values;
}

std::int32_t SaverConfig::copyValue(char* buf, std::uint32_t bufSize, const char* value) noexcept {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    // int32 装不下就没法按契约报告长度；配置值不该有这么大，防御性拒绝。
    if (len > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return -1;
    }
    // 契约：buf 装得下（含 NUL）才写；装不下只报告长度，宿主据此扩容后重试。
    if (buf != nullptr && bufSize > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

std::int32_t SaverConfig::copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) noexcept {
    if (src == nullptr || out == nullptr) {
        return 1;
    }
    // 宿主的结构可能比本插件认识的还短：装不下 tooltip 就报 2，绝不写到宿主内存之外。
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = *src;
    return 0;
}

}  // namespace image_saver
