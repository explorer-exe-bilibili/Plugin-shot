// 消息提示插件的配置项声明与值读写。
//
// 配置项声明、字符串值解析、当前值读写都在这个文件里；ABI 回调在
// plugin_entry.cpp 只做参数转发。值一律按契约用字符串表达：
//   · enabled / show_on_success / show_on_error / sound → "1" / "0"
//   · title         → 标题模板（UTF-8），空串 = 用默认多语言标题
//   · click_action  → "click.open_image" / "click.open_folder"（同时是 i18n key）
//
// 线程模型：set_config 可能在宿主任意线程上被调用（init 的 apply_stored、设置页
// 应用），通知线程同时可能正在读开关；四个布尔与枚举用 std::atomic 保存，读侧永远
// 拿到完整值、不需要加锁。标题是字符串，用互斥量保护，读侧拿一份拷贝。
//
// 兼容性：click_action 的旧值（open_image / open_folder）继续接受，get 一律返回
// 新键（document/ABI_POLICY.md §5：配置值只追加，不改已有语义）。

#include "notify_config.h"

#include <cstring>
#include <iterator>

namespace notify {
namespace {

// 配置键：全小写 + 下划线，发布后不能改名（它是存盘标识，见 ABI_POLICY.md §5）。
const char kKeyEnabled[] = "enabled";
const char kKeyShowOnSuccess[] = "show_on_success";
const char kKeyShowOnError[] = "show_on_error";
const char kKeySound[] = "sound";
const char kKeyTitle[] = "title";
const char kKeyClickAction[] = "click_action";

// ENUM 候选值同时是 i18n key（设置页按 display(owner, 候选值) 解析）：
// 候选值用命名空间前缀，避免与其它枚举的 "open_image" 之类撞名。
const char kClickOpenImage[] = "click.open_image";
const char kClickOpenFolder[] = "click.open_folder";

// 旧版本存下的技术值：升级后用户配置里可能还是它们，必须照常生效。
const char kLegacyOpenImage[] = "open_image";
const char kLegacyOpenFolder[] = "open_folder";

// label / tooltip 是 i18n key，不是最终显示文本：宿主把 key 原样放进 config_list，
// 设置页用 i18n_list 解析成当前语言（display(owner, key) ?? key）。对应文案在
// plugin_entry.cpp 的 kLangTables 里（config.<配置键>.label / .tooltip），两边必须
// 一起改；查不到时设置页显示 key 本身，便于发现漏写。
const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyEnabled,
        .label         = "config.enabled.label",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "config.enabled.tooltip",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyShowOnSuccess,
        .label         = "config.show_on_success.label",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "config.show_on_success.tooltip",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyShowOnError,
        .label         = "config.show_on_error.label",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "config.show_on_error.tooltip",
    },
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeySound,
        .label         = "config.sound.label",
        .default_value = "1",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 1.0,
        .step          = 1.0,
        .tooltip       = "config.sound.tooltip",
    },
    {
        // STRING：标题模板。默认空串 = 用 toast.success_title / toast.failed_title
        // 这两条多语言默认标题；非空时支持 {path} / {width} / {height} 占位符
        // （展开见 notify_title.cpp，只做朴素字符串替换，不引入模板引擎）。
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_STRING,
        .key           = kKeyTitle,
        .label         = "config.title.label",
        .default_value = "",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "config.title.tooltip",
    },
    {
        // ENUM：点击成功通知时做什么。候选值即 i18n key，设置页显示成
        // 「打开图片 / 打开所在文件夹」。
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_ENUM,
        .key           = kKeyClickAction,
        .label         = "config.click_action.label",
        .default_value = kClickOpenImage,
        .options       = "click.open_image|click.open_folder",
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "config.click_action.tooltip",
    },
};

/**
 * @brief 把配置里的布尔文本解析成 bool。
 *
 * 只把 "0" / "false" 当成关：宿主升级后可能传别的写法，按"不认识就当默认值（开）"
 * 处理，比把用户配置的开关意外关掉安全（与 tray / clipboard 的约定一致）。
 *
 * @param[in] value 配置值文本，不能为空。
 * @return true = 开；false = 关（含 "0" / "false"）。
 * @note 无状态的纯解析函数，不属于 NotifyConfig 的接口，所以放匿名命名空间（STYLE §6.7）。
 */
bool parseBool(const char* value) {
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

/**
 * @brief 把配置里的点击行为文本解析成 ClickAction。
 *
 * 同时接受新键（click.open_image / click.open_folder）与旧值（open_image /
 * open_folder）：用户配置里存的可能是旧值，升级后必须照常生效。
 *
 * @param[in]  value 配置值文本，不能为空。
 * @param[out] out   解析结果；仅返回 true 时写入。
 * @return true = 认识这个值；false = 非法取值（调用方保留原值）。
 */
bool parseClickAction(const char* value, ClickAction* out) {
    if (std::strcmp(value, kClickOpenImage) == 0 || std::strcmp(value, kLegacyOpenImage) == 0) {
        *out = ClickAction::OpenImage;
        return true;
    }
    if (std::strcmp(value, kClickOpenFolder) == 0 || std::strcmp(value, kLegacyOpenFolder) == 0) {
        *out = ClickAction::OpenFolder;
        return true;
    }
    return false;
}

}  // namespace

NotifyConfig::NotifyConfig() = default;

std::uint32_t NotifyConfig::configCount() const noexcept {
    return static_cast<std::uint32_t>(std::size(kConfig));
}

std::int32_t NotifyConfig::configDesc(std::uint32_t index, ShotConfigDesc* out) const {
    if (index >= std::size(kConfig)) {
        return 1;
    }
    return copyDesc(&kConfig[index], out);
}

std::int32_t NotifyConfig::getValue(const char* key, char* buf, std::uint32_t buf_size) const {
    if (key == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        return copyValue(m_enabled.load() ? "1" : "0", buf, buf_size);
    }
    if (std::strcmp(key, kKeyShowOnSuccess) == 0) {
        return copyValue(m_showOnSuccess.load() ? "1" : "0", buf, buf_size);
    }
    if (std::strcmp(key, kKeyShowOnError) == 0) {
        return copyValue(m_showOnError.load() ? "1" : "0", buf, buf_size);
    }
    if (std::strcmp(key, kKeySound) == 0) {
        return copyValue(m_sound.load() ? "1" : "0", buf, buf_size);
    }
    if (std::strcmp(key, kKeyTitle) == 0) {
        // 先拷一份再写宿主缓冲区：锁外调用 copyValue，避免长寿锁 + 让 value 指针
        // 指向一份稳定的本地拷贝（m_title 可能被设置页线程同时改写）。
        const std::string copy = title();
        return copyValue(copy.c_str(), buf, buf_size);
    }
    if (std::strcmp(key, kKeyClickAction) == 0) {
        return copyValue(clickActionKey(clickAction()), buf, buf_size);
    }
    return -1;  // 不认识的 key：安全忽略（宿主升级后多传一项也不会踩这里）
}

std::int32_t NotifyConfig::setValue(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        return -1;
    }
    if (std::strcmp(key, kKeyEnabled) == 0) {
        m_enabled.store(parseBool(value));
        return 0;
    }
    if (std::strcmp(key, kKeyShowOnSuccess) == 0) {
        m_showOnSuccess.store(parseBool(value));
        return 0;
    }
    if (std::strcmp(key, kKeyShowOnError) == 0) {
        m_showOnError.store(parseBool(value));
        return 0;
    }
    if (std::strcmp(key, kKeySound) == 0) {
        m_sound.store(parseBool(value));
        return 0;
    }
    if (std::strcmp(key, kKeyTitle) == 0) {
        // 空串是合法值（= 恢复默认多语言标题），不做"空串等价于默认值"的替换，
        // 否则用户永远清不掉自定义标题。
        {
            std::lock_guard<std::mutex> lock(m_titleMutex);
            m_title = value;
        }
        return 0;
    }
    if (std::strcmp(key, kKeyClickAction) == 0) {
        ClickAction action = ClickAction::OpenImage;
        if (!parseClickAction(value, &action)) {
            return -1;  // 非法取值：拒绝，保留原值，宿主可以继续工作
        }
        m_clickAction.store(static_cast<int>(action));
        return 0;
    }
    return -1;
}

bool NotifyConfig::enabled() const noexcept {
    return m_enabled.load();
}

bool NotifyConfig::showOnSuccess() const noexcept {
    return m_showOnSuccess.load();
}

bool NotifyConfig::showOnError() const noexcept {
    return m_showOnError.load();
}

bool NotifyConfig::soundEnabled() const noexcept {
    return m_sound.load();
}

std::string NotifyConfig::title() const {
    std::lock_guard<std::mutex> lock(m_titleMutex);
    return m_title;
}

ClickAction NotifyConfig::clickAction() const noexcept {
    return static_cast<ClickAction>(m_clickAction.load());
}

const char* NotifyConfig::clickActionKey(ClickAction action) noexcept {
    return (action == ClickAction::OpenFolder) ? kClickOpenFolder : kClickOpenImage;
}

std::int32_t NotifyConfig::copyValue(const char* value, char* buf, std::uint32_t buf_size) {
    if (value == nullptr) {
        return -1;
    }
    const std::size_t len = std::strlen(value);
    // 契约：buf_size 大于返回值才写入（含结尾 NUL）；装不下只报长度，不算失败。
    if (buf != nullptr && buf_size > len) {
        std::memcpy(buf, value, len + 1);
    }
    return static_cast<std::int32_t>(len);
}

std::int32_t NotifyConfig::copyDesc(const ShotConfigDesc* src, ShotConfigDesc* out) {
    if (src == nullptr || out == nullptr) {
        return 1;
    }
    // 老宿主的结构可能比本插件认识的短：整段拷贝前必须确认它装得下完整声明，
    // 否则 *out = *src 会写到宿主缓冲区之外（与 tray / clipboard 的约定一致）。
    if (out->struct_size < SHOT_REQUIRES(ShotConfigDesc, tooltip)) {
        return 2;
    }
    *out = *src;
    return 0;
}

}  // namespace notify
