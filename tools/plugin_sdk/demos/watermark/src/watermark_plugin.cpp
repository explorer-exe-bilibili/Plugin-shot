// 时间水印 demo 的插件状态实现：配置快照 + 本地时间格式化 + 调用渲染器。
//
// 为什么时间取"处理这张图的时刻"而不是采集时刻：ShotImage 契约里没有采集时间戳，
// process 在采集完成后立即执行，本地时间与本机时钟误差都在秒级，足够水印使用。
//
// label / tooltip 写的是语言表 key，不是中文：设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析，文案本体在 kLangTables。

#include "watermark_plugin.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <string>

#include "plugin_entry.h"
#include "watermark_renderer.h"

namespace watermark_demo {
namespace {

const char kKeyEnabled[] = "enabled";
const char kKeyFormat[] = "format";
const char kKeyScale[] = "scale";
const char kKeyMargin[] = "margin";

// 默认值只在描述符里写一遍，构造函数也从这几个常量取，避免两处漂移。
const char kDefaultEnabled[] = "1";
const char kDefaultFormat[] = "%Y-%m-%d %H:%M:%S";
const char kDefaultScale[] = "2";
const char kDefaultMargin[] = "16";

constexpr int kMinScale = 1;
constexpr int kMaxScale = 4;
constexpr int kMinMargin = 0;
constexpr int kMaxMargin = 64;
// 格式串上限：128 字节的 strftime 输出缓冲足够，太长的串直接拒绝，避免误配置。
constexpr std::size_t kMaxFormatLength = 64;

const char kLabelEnabled[] = "config.enabled.label";
const char kTooltipEnabled[] = "config.enabled.tooltip";
const char kLabelFormat[] = "config.format.label";
const char kTooltipFormat[] = "config.format.tooltip";
const char kLabelScale[] = "config.scale.label";
const char kTooltipScale[] = "config.scale.tooltip";
const char kLabelMargin[] = "config.margin.label";
const char kTooltipMargin[] = "config.margin.tooltip";

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
        .kind          = SHOT_CONFIG_KIND_STRING,
        .key           = kKeyFormat,
        .label         = kLabelFormat,
        .default_value = kDefaultFormat,
        .options       = nullptr,
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipFormat,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_INT,
        .key           = kKeyScale,
        .label         = kLabelScale,
        .default_value = kDefaultScale,
        .options       = nullptr,
        .min           = static_cast<double>(kMinScale),
        .max           = static_cast<double>(kMaxScale),
        .step          = 1.0,
        .tooltip       = kTooltipScale,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_INT,
        .key           = kKeyMargin,
        .label         = kLabelMargin,
        .default_value = kDefaultMargin,
        .options       = nullptr,
        .min           = static_cast<double>(kMinMargin),
        .max           = static_cast<double>(kMaxMargin),
        .step          = 1.0,
        .tooltip       = kTooltipMargin,
    },
};

// 语言表：每语言一张单行 JSON（@meta 必填）；整行会超过 120 列，所以用相邻
// raw 字面量拼接——拼出来仍然是一行合法 JSON。各语言的 key 集必须一致。
const char* const kLangTables[] = {
    R"json({"@meta":"zh-CN",)json"
    R"json("plugin.name":"时间水印",)json"
    R"json("plugin.description":"每次截图后在图片右下角加上拍摄时间",)json"
    R"json("config.enabled.label":"启用时间水印",)json"
    R"json("config.enabled.tooltip":"关闭后不处理任何截图",)json"
    R"json("config.format.label":"时间格式",)json"
    R"json("config.format.tooltip":"strftime 格式串，如 %Y-%m-%d %H:%M:%S",)json"
    R"json("config.scale.label":"字体放大",)json"
    R"json("config.scale.tooltip":"1 = 5x7 像素小字，4 = 20x28 像素大字",)json"
    R"json("config.margin.label":"边距",)json"
    R"json("config.margin.tooltip":"水印距图片右边缘与下边缘的像素数"})json",
    R"json({"@meta":"en-US",)json"
    R"json("plugin.name":"Time watermark",)json"
    R"json("plugin.description":"Stamps the capture time on the bottom-right corner of every screenshot",)json"
    R"json("config.enabled.label":"Enable time watermark",)json"
    R"json("config.enabled.tooltip":"When off, screenshots are left untouched",)json"
    R"json("config.format.label":"Time format",)json"
    R"json("config.format.tooltip":"strftime pattern, e.g. %Y-%m-%d %H:%M:%S",)json"
    R"json("config.scale.label":"Text scale",)json"
    R"json("config.scale.tooltip":"1 = tiny 5x7 text, 4 = large 20x28 text",)json"
    R"json("config.margin.label":"Margin",)json"
    R"json("config.margin.tooltip":"Distance in pixels from the right and bottom edges"})json",
};

/**
 * @brief 把配置里的布尔文本解析成 bool（只把 "0" / "false" 当关）。
 * @param[in] value 配置值文本，不能为 nullptr。
 * @return true = 开；false = 关。
 */
bool parseBool(const char* value) {
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

/** @brief 把十进制文本解析成整数并夹到 [min, max]。 */
int parseClampedInt(const char* value, int min, int max, int fallback) {
    if (value == nullptr) {
        return fallback;
    }
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

/** @brief 按契约把字符串写进调用方缓冲（buf_size 大于长度才写入，含结尾 NUL）。 */
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

/** @brief 把静态描述符拷进宿主提供的 ShotConfigDesc。 */
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

WatermarkPlugin::WatermarkPlugin()
    : m_enabled(parseBool(kDefaultEnabled)),
      m_format(kDefaultFormat),
      m_scale(parseClampedInt(kDefaultScale, kMinScale, kMaxScale, 2)),
      m_margin(parseClampedInt(kDefaultMargin, kMinMargin, kMaxMargin, 16)) {}

WatermarkPlugin::~WatermarkPlugin() {
    stop();
}

bool WatermarkPlugin::start(const ShotHostServices* host) {
    m_host = host;
    // 语言表要在宿主生成 config_list 快照之前发布；create() 拿不到服务表，
    // 这里是第一个能拿到 host 的生命周期路径。
    m_i18n = std::make_unique<shot::I18n>(host, kWatermark.id);
    if (!m_i18n->loadTables(kLangTables, std::size(kLangTables))) {
        shot::Logger(host).warn("[watermark] 语言表解析失败，配置项文案回退为 key");
    }
    shot::Logger(host).info("[watermark] 时间水印就绪：截图右下角会盖上拍摄时间");
    return true;
}

void WatermarkPlugin::stop() {
    m_i18n.reset();
}

std::int32_t WatermarkPlugin::process(ShotImage* image) {
    // ---- 参数校验：不认识的形状返回 1（宿主只记 warn，不中断流水线） ----
    if (image == nullptr || image->data == nullptr || image->width < 1 || image->height < 1) {
        return 1;
    }
    if (!SHOT_HAS_FIELD(image, ShotImage, pixel_format) ||
        image->pixel_format != SHOT_PIXEL_FORMAT_BGRA8 || image->channels != 4) {
        return 1;
    }

    // ---- 读配置快照：process 在采集 worker 线程，set_config 在 main 线程 ----
    bool enabled = true;
    std::string format;
    int scale = 2;
    int margin = 16;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        enabled = m_enabled;
        format = m_format;
        scale = m_scale;
        margin = m_margin;
    }
    if (!enabled) {
        return 0;
    }

    // ---- 取本地时间文本；格式配错（strftime 返回 0）时回退默认格式，不让水印消失 ----
    std::string text;
    if (!formatTimestamp(format, text) || text.empty()) {
        if (!formatTimestamp(kDefaultFormat, text)) {
            shot::Logger(m_host).debug("[watermark] 取本地时间失败，跳过本次水印");
            return 0;
        }
    }

    // ---- 绘制；图像太小放不下时只记 debug，仍算处理成功（不污染流水线统计） ----
    if (!drawTextBottomRight(image, text, scale, margin)) {
        shot::Logger(m_host).debug("[watermark] 图像太小或边距过大，跳过本次水印");
    }
    return 0;
}

bool WatermarkPlugin::formatTimestamp(const std::string& format, std::string& out) {
    if (format.empty()) {
        return false;
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    // localtime 不是线程安全的：在采集 worker 线程上必须用平台的安全版本。
    if (localtime_s(&local, &now) != 0) {
        return false;
    }
#else
    if (localtime_r(&now, &local) == nullptr) {
        return false;
    }
#endif
    char buffer[128] = {};
    const std::size_t written = std::strftime(buffer, sizeof(buffer), format.c_str(), &local);
    if (written == 0) {
        return false;  // 格式非法或结果超过缓冲，由调用方回退默认格式
    }
    out.assign(buffer, written);
    return true;
}

std::uint32_t WatermarkPlugin::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t WatermarkPlugin::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t WatermarkPlugin::getConfig(const char* key, char* buf,
                                        std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (std::strcmp(key, kKeyEnabled) == 0) {
        return copyValue(m_enabled ? "1" : "0", buf, bufSize);
    }
    if (std::strcmp(key, kKeyFormat) == 0) {
        return copyValue(m_format.c_str(), buf, bufSize);
    }
    if (std::strcmp(key, kKeyScale) == 0) {
        return copyValue(std::to_string(m_scale).c_str(), buf, bufSize);
    }
    if (std::strcmp(key, kKeyMargin) == 0) {
        return copyValue(std::to_string(m_margin).c_str(), buf, bufSize);
    }
    return -1;  // 不认识的 key：安全忽略
}

std::int32_t WatermarkPlugin::setConfig(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (std::strcmp(key, kKeyEnabled) == 0) {
        m_enabled = parseBool(value);
        return 0;
    }
    if (std::strcmp(key, kKeyFormat) == 0) {
        if (std::strlen(value) > kMaxFormatLength) {
            return -1;  // 过长的格式串没有意义，直接拒绝（设置页会显示原值）
        }
        m_format = value;
        return 0;
    }
    if (std::strcmp(key, kKeyScale) == 0) {
        m_scale = parseClampedInt(value, kMinScale, kMaxScale, m_scale);
        return 0;
    }
    if (std::strcmp(key, kKeyMargin) == 0) {
        m_margin = parseClampedInt(value, kMinMargin, kMaxMargin, m_margin);
        return 0;
    }
    return -1;
}

}  // namespace watermark_demo
