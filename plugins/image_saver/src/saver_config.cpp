// 四个配置项：输出目录 / 格式 / 质量 / 文件名模板。
//
// 跨边界一律用字符串（契约：只传定宽类型 + const char*），本文件负责字符串与内部
// 类型的校验转换：非法值安静回退默认、不认识的 key 返回负值，宿主升级后多传一项
// 老插件也不会坏（document/ABI_POLICY.md §5）。
//
// label / tooltip 写的是语言表 key，不是中文：设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析，文案本体在
// image_saver.cpp 的 kLangTables 里（两边的 key 拼写必须严格一致）。
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
// 格式：内部仍用技术名 png / jpeg（编码器与快照都按它判断），跨边界（config_list、
// 配置值）统一用带命名空间的键 format.png / format.jpeg —— 它们同时是 i18n key，
// 设置页按 display(owner, 值) 解析成「PNG（无损）」这类用户文案。
const char kFormatPng[] = "png";
const char kFormatJpeg[] = "jpeg";
const char kOptionPng[] = "format.png";
const char kOptionJpeg[] = "format.jpeg";
const char kDefaultQuality[] = "90";
const char kDefaultFilename[] = "shot_%Y%m%d_%H%M%S";

constexpr int kMinQuality = 1;
constexpr int kMaxQuality = 100;

// 文案 key：命名规范 config.<配置 key>.label / .tooltip，写成常量是为了让 kConfig
// 一眼能扫出"这里放的是 key 而不是中文"。ENUM 的候选值本身也是 i18n key
// （format.png / format.jpeg），设置页直接用 display(owner, 候选值) 解析文案。
const char kLabelDirectory[] = "config.directory.label";
const char kTooltipDirectory[] = "config.directory.tooltip";
const char kLabelFormat[] = "config.format.label";
const char kTooltipFormat[] = "config.format.tooltip";
const char kLabelQuality[] = "config.quality.label";
const char kTooltipQuality[] = "config.quality.tooltip";
const char kLabelFilename[] = "config.filename.label";
const char kTooltipFilename[] = "config.filename.tooltip";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_PATH,
        .key           = kKeyDirectory,
        .label         = kLabelDirectory,
        .default_value = kDefaultDirectory,
        // PATH 借 options 声明对话框类型（document/SETTINGS.md 的 kind 表）：
        // 保存目录必须选目录，否则默认值为空时设置页会弹成"选文件"。
        .options       = "directory",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipDirectory,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeyFormat,
        .label         = kLabelFormat,
        .default_value = kOptionPng,
        .options       = "format.png|format.jpeg",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipFormat,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_INT,
        .key           = kKeyQuality,
        .label         = kLabelQuality,
        .default_value = kDefaultQuality,
        .options       = nullptr,
        .min           = static_cast<double>(kMinQuality),
        .max           = static_cast<double>(kMaxQuality),
        .step          = 1.0,
        .tooltip       = kTooltipQuality,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_STRING,
        .key           = kKeyFilename,
        .label         = kLabelFilename,
        .default_value = kDefaultFilename,
        .options       = nullptr,
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipFilename,
    },
};

}  // namespace

SaverConfig::SaverConfig() {
    // 构造期间没有并发，直接按默认常量填；不走 set() 是为了让"默认值"只依赖上面
    // 那四个常量（set() 还要做字符串比较，构造函数里没必要）。
    m_values.directory = kDefaultDirectory;
    m_values.format = kFormatPng;
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
        // 对外统一返回命名空间键（同时是 i18n key）；内部快照仍是 png / jpeg。
        return copyValue(buf, bufSize, m_values.format == kFormatJpeg ? kOptionJpeg : kOptionPng);
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
        // 同时接受新键（format.png / format.jpeg）与旧值（png / jpeg）：用户已有配置
        // 里存的还是旧值，升级后必须照常生效；写入后内部一律归一成技术名。
        const char* format = nullptr;
        if (std::strcmp(value, kOptionPng) == 0 || std::strcmp(value, kFormatPng) == 0) {
            format = kFormatPng;
        } else if (std::strcmp(value, kOptionJpeg) == 0 ||
                   std::strcmp(value, kFormatJpeg) == 0) {
            format = kFormatJpeg;
        } else {
            return -1;  // 非法取值：拒绝，保留原值，宿主可以继续工作
        }
        m_values.format = format;
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
