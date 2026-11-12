// 本插件的配置项：位移估计算法与长图高度上限。
//
// 跨边界一律用字符串（契约：只传定宽类型 + const char*），本文件负责字符串与
// 内部类型的校验转换：非法值安静回退默认、不认识的 key 返回负值，宿主升级后
// 多传一项老插件也不会坏（document/ABI_POLICY.md §5）。
//
// 触发热键已移交给 hotkey 插件（本插件只贡献 hotkey_list 条目），所以这里不再有
// hotkey 配置项，也不在插件内注册任何长期热键。
//
// label / tooltip 写的是语言表 key，不是中文：设置页按
// display(owner, key) = i18n_list[(owner, key)] ?? key 解析，文案本体在
// long_capture_i18n.cpp 里（两边的 key 拼写必须严格一致）。
//
// 返回码约定（include/plugin_api.h 的 ShotConfigurableV1）：get 返回 >= 0 是值
// 长度（不含 NUL），负数只表示 key 不认识或参数非法；set 返回 0 = 已生效。

#include "long_capture_config.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>

namespace long_capture {
namespace {

const char kKeyAlgorithm[] = "algorithm";
const char kKeyMaxHeight[] = "max_height";

// 默认值只在描述符里写一遍：构造函数也从这个常量取初始值，避免"描述符默认值"
// 与"初始值"两处漂移。默认 correlate：它最耐相似行/重复内容，见插件 README。
const char kDefaultAlgorithm[] = "correlate";
const char kDefaultMaxHeight[] = "30000";

// 高度上限：下限保证至少能放进一屏，上限防止用户手改配置把画布撑到几百 MB。
constexpr int kMinHeight = 1000;
constexpr int kMaxHeight = 60000;

// 文案 key：命名规范 config.<配置 key>.label / .tooltip，写成常量是为了让 kConfig
// 一眼能扫出"这里放的是 key 而不是中文"。
const char kLabelAlgorithm[] = "long.config.algorithm.label";
const char kTooltipAlgorithm[] = "long.config.algorithm.tooltip";
const char kLabelMaxHeight[] = "long.config.max_height.label";
const char kTooltipMaxHeight[] = "long.config.max_height.tooltip";

const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeyAlgorithm,
        .label         = kLabelAlgorithm,
        .default_value = kDefaultAlgorithm,
        // 候选值是纯技术名（hash/correlate/phase），与 stitch_estimator.h 的枚举一一
        // 对应；"auto"（多算法融合）是预留值，只在 set 里接受，不放进候选让用户选。
        .options       = "hash|correlate|phase",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = kTooltipAlgorithm,
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_INT,
        .key           = kKeyMaxHeight,
        .label         = kLabelMaxHeight,
        .default_value = kDefaultMaxHeight,
        .options       = nullptr,
        .min           = static_cast<double>(kMinHeight),
        .max           = static_cast<double>(kMaxHeight),
        .step          = 1000.0,
        .tooltip       = kTooltipMaxHeight,
    },
};

}  // namespace

LongCaptureConfig::LongCaptureConfig() {
    // 构造期间没有并发，直接按默认常量填；不走 set() 是为了让"默认值"只依赖上面
    // 那些常量，也避免构造函数里再去解析一次整数。
    m_values.algorithm = StitchAlgorithm::Correlate;
    parseStitchAlgorithm(kDefaultAlgorithm, m_values.algorithm);
    m_values.maxHeight = std::atoi(kDefaultMaxHeight);
}

std::uint32_t LongCaptureConfig::itemCount() noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t LongCaptureConfig::describe(std::uint32_t index, ShotConfigDesc* out) noexcept {
    if (out == nullptr || index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t LongCaptureConfig::get(const char* key, char* buf, std::uint32_t bufSize) const {
    if (key == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::strcmp(key, kKeyAlgorithm) == 0) {
        return copyValue(buf, bufSize, stitchAlgorithmName(m_values.algorithm));
    }
    if (std::strcmp(key, kKeyMaxHeight) == 0) {
        const std::string text = std::to_string(m_values.maxHeight);
        return copyValue(buf, bufSize, text.c_str());
    }
    return -1;  // 不认识的 key：安静忽略，宿主升级后仍可工作
}

std::int32_t LongCaptureConfig::set(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::strcmp(key, kKeyAlgorithm) == 0) {
        // ENUM 只接受描述符里的候选值；"auto" 由 parseStitchAlgorithm 接受并回落到
        // correlate（多算法融合留待后续，见 stitch_estimator.cpp 的 TODO）。
        StitchAlgorithm parsed = m_values.algorithm;
        if (!parseStitchAlgorithm(value, parsed)) {
            return -1;  // 非法取值：拒绝，保留原值，宿主可以继续工作
        }
        m_values.algorithm = parsed;
        return 0;
    }
    if (std::strcmp(key, kKeyMaxHeight) == 0) {
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end == value || end == nullptr || *end != '\0') {
            return -1;  // 不是十进制整数：拒绝，保留原值
        }
        // 越界时夹到合法范围而不是拒绝：手改配置写成 999999 时仍能工作。
        m_values.maxHeight = static_cast<int>(std::clamp<long>(parsed, kMinHeight, kMaxHeight));
        return 0;
    }
    return -1;  // 不认识的 key：安静忽略
}

LongCaptureConfig::Snapshot LongCaptureConfig::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_values;
}

std::int32_t LongCaptureConfig::copyValue(char* buf, std::uint32_t bufSize,
                                          const char* value) noexcept {
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

std::int32_t LongCaptureConfig::copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) noexcept {
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

}  // namespace long_capture
