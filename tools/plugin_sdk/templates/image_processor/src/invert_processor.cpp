// 反色处理器实现：BGRA8 逐像素反色，强度 0~100 线性混合。
//
//   strength = 0   : 原图（B/G/R 不动）
//   strength = 100 : 完全反色（B/G/R 取反）
//   中间值         : value = (original * (100 - strength) + inverted * strength) / 100
//
// A 通道始终不动：图像处理插件不该改变透明信息。
//
// 两条能力规则（document/CAPTURE.md §2.2 / plugin_api/image.h）：
//   · 只能改像素，不得改 ShotImage 的 width/height/stride/data 等字段；
//   · stride 可以小于 0、也可以大于 width*4，必须按行寻址，不能当成连续块遍历。
//
// label / tooltip 写的是语言表 key，不是中文：设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析，文案本体在 kLangTables。

#include "invert_processor.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#include "plugin_entry.h"

namespace image_processor_template {
namespace {

const char kKeyEnabled[] = "enabled";
const char kKeyStrength[] = "strength";

// 默认值只在描述符里写一遍，构造函数也从这两个常量取，避免两处漂移。
const char kDefaultEnabled[] = "1";
const char kDefaultStrength[] = "100";

constexpr int kMinStrength = 0;
constexpr int kMaxStrength = 100;

const char kLabelEnabled[] = "config.enabled.label";
const char kTooltipEnabled[] = "config.enabled.tooltip";
const char kLabelStrength[] = "config.strength.label";
const char kTooltipStrength[] = "config.strength.tooltip";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyEnabled,
        .label         = kLabelEnabled,
        .default_value = kDefaultEnabled,
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = kTooltipEnabled,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_INT,
        .key           = kKeyStrength,
        .label         = kLabelStrength,
        .default_value = kDefaultStrength,
        .options       = nullptr,
        .min           = static_cast<double>(kMinStrength),
        .max           = static_cast<double>(kMaxStrength),
        .step          = 1.0,
        .tooltip       = kTooltipStrength,
    },
};

// 语言表：每语言一张单行 JSON（@meta 必填）；整行会超过 120 列，所以用相邻
// raw 字面量拼接——拼出来仍然是一行合法 JSON。各语言的 key 集必须一致。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"{{PLUGIN_NAME}}",)json"
    R"json("plugin.description":"{{PLUGIN_DESCRIPTION}}",)json"
    R"json("config.enabled.label":"启用反色",)json"
    R"json("config.enabled.tooltip":"关闭后本插件不处理任何图像",)json"
    R"json("config.strength.label":"强度",)json"
    R"json("config.strength.tooltip":"0 = 原图，100 = 完全反色"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"{{PLUGIN_NAME}}",)json"
    R"json("plugin.description":"{{PLUGIN_DESCRIPTION}}",)json"
    R"json("config.enabled.label":"Enable invert",)json"
    R"json("config.enabled.tooltip":"When off, this plugin leaves images untouched",)json"
    R"json("config.strength.label":"Strength",)json"
    R"json("config.strength.tooltip":"0 = original image, 100 = fully inverted"})json",
};

/**
 * @brief 把配置里的布尔文本解析成 bool（只把 "0" / "false" 当关）。
 * @param[in] value 配置值文本，不能为 nullptr。
 * @return true = 开；false = 关。
 */
bool parseBool(const char* value) {
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

/**
 * @brief 把十进制文本解析成整数，并夹到 [min, max]。
 * @param[in] value 配置值文本，不能为 nullptr。
 * @param[in] min   下界。
 * @param[in] max   上界。
 * @param[in] fallback 解析失败时的返回值。
 * @return 夹取后的整数。
 */
int parseClampedInt(const char* value, int min, int max, int fallback) {
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || end == nullptr || *end != '\0') {
        return fallback;
    }
    if (parsed < min) {
        return min;
    }
    if (parsed > max) {
        return max;
    }
    return static_cast<int>(parsed);
}

/**
 * @brief 按契约把字符串写进调用方缓冲（buf_size 大于长度才写入，含结尾 NUL）。
 * @param[in]  value   要写入的文本。
 * @param[out] buf     接收缓冲；可为 nullptr（只查长度）。
 * @param[in]  bufSize buf 的字节数。
 * @return >= 0 = 值的字节数（不含 NUL）；< 0 = value 为空。
 */
std::int32_t copyValue(const char* value, char* buf, std::uint32_t bufSize) {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    if (buf != nullptr && bufSize > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

/**
 * @brief 把静态描述符拷进宿主提供的 ShotConfigDesc。
 * @param[in]  src 静态描述符。
 * @param[out] out 宿主输出结构（已填 struct_size）。
 * @return 0 = 成功；1 = 参数为空；2 = 宿主结构太短。
 */
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

InvertProcessor::InvertProcessor() : m_enabled(parseBool(kDefaultEnabled)),
                                     m_strength(parseClampedInt(kDefaultStrength, kMinStrength,
                                                                kMaxStrength, kMaxStrength)) {}

InvertProcessor::~InvertProcessor() {
    stop();
}

bool InvertProcessor::start(const ShotHostServices* host) {
    m_host = host;
    // 语言表要在宿主生成 config_list 快照之前发布；create() 拿不到服务表，
    // 这里是第一个能拿到 host 的生命周期路径。
    m_i18n = std::make_unique<shot::I18n>(host, kInvert.id);
    if (!m_i18n->loadTables(kLangTables, std::size(kLangTables))) {
        shot::Logger(host).warn("[{{TARGET}}] 语言表解析失败，配置项文案回退为 key");
    }
    shot::Logger(host).info("[{{TARGET}}] 反色处理器就绪（可调强度 0~100）");
    return true;
}

void InvertProcessor::stop() {
    m_i18n.reset();
}

std::int32_t InvertProcessor::process(ShotImage* image) {
    // ---- 参数校验：任何不认识的形状都返回 1，宿主只记 warn、不中断流水线 ----
    if (image == nullptr || image->data == nullptr) {
        return 1;
    }
    if (image->width < 1 || image->height < 1) {
        return 1;
    }
    // 老宿主的 ShotImage 可能没有 pixel_format 字段：先 SHOT_HAS_FIELD 再读。
    if (!SHOT_HAS_FIELD(image, ShotImage, pixel_format) ||
        image->pixel_format != SHOT_PIXEL_FORMAT_BGRA8) {
        return 1;
    }
    if (image->channels != 4) {
        return 1;
    }
    const std::int32_t rowBytes = image->width * 4;
    const std::int32_t absStride = image->stride < 0 ? -image->stride : image->stride;
    // 每行有效字节是 width*4：stride 绝对值比它小说明行与行重叠，不敢写。
    if (absStride < rowBytes) {
        return 1;
    }

    // ---- 读一次配置快照：process 在采集 worker 线程，set_config 在 main 线程 ----
    bool enabled = true;
    int strength = 100;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        enabled = m_enabled;
        strength = m_strength;
    }
    if (!enabled || strength == 0) {
        return 0;  // 关闭或强度 0：什么都不改，仍算处理成功
    }

    // ---- 逐行寻址：data 指向第 0 行；stride 为负时 data + y*stride 仍指向第 y 行 ----
    const int inverse = 100 - strength;
    for (std::int32_t y = 0; y < image->height; ++y) {
        std::uint8_t* row = image->data + static_cast<std::ptrdiff_t>(y) * image->stride;
        for (std::int32_t x = 0; x < image->width; ++x) {
            std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
            // B/G/R 三个分量取反并按强度混合；A 通道保持原值。
            for (int channel = 0; channel < 3; ++channel) {
                const int original = pixel[channel];
                const int inverted = 255 - original;
                pixel[channel] =
                    static_cast<std::uint8_t>((original * inverse + inverted * strength) / 100);
            }
        }
    }
    return 0;
}

std::uint32_t InvertProcessor::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t InvertProcessor::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t InvertProcessor::getConfig(const char* key, char* buf, std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (std::strcmp(key, kKeyEnabled) == 0) {
        return copyValue(m_enabled ? "1" : "0", buf, bufSize);
    }
    if (std::strcmp(key, kKeyStrength) == 0) {
        return copyValue(std::to_string(m_strength).c_str(), buf, bufSize);
    }
    return -1;  // 不认识的 key：安全忽略
}

std::int32_t InvertProcessor::setConfig(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (std::strcmp(key, kKeyEnabled) == 0) {
        m_enabled = parseBool(value);
        return 0;
    }
    if (std::strcmp(key, kKeyStrength) == 0) {
        m_strength = parseClampedInt(value, kMinStrength, kMaxStrength, m_strength);
        return 0;
    }
    return -1;
}

}  // namespace image_processor_template
