#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// ============================================================================
//  Host::I18n —— 宿主侧界面语言（i18n）中心
//
//  这个类只解决宿主自己的那部分 i18n，跨插件文本由通用列表协议承担：
//    · 语言注册表：tag + 原生显示名 + fallback，表驱动（加语种只加一行）；
//    · 当前语言：core/language 配置项（auto|zh-CN|zh-TW|en-US），auto 解析
//      Windows 的 GetUserDefaultLocaleName，其它平台回退 en-US；
//    · 宿主自己的文案表：托盘命令 label、配置项 label/tooltip、ENUM 候选的原生
//      显示名，以及宿主产生、被消费方原样展示的动态文案（注册表 reason、
//      capture.failed.reason 等）；
//    · 语言表选择：给定"标签 -> 文本"的映射，按 完全匹配 -> 主语言 -> 默认
//      en-US -> 第一条可用文本 的顺序挑一条（与插件侧 shot::I18n 同一套回退链）。
//
//  为什么宿主文案不走 key：这些字符串由宿主自己生成、只有宿主知道参数，
//  消费方拿到就该直接显示（document/I18N.md §4）。反过来说，插件贡献给
//  config_list / tray_command_list 的 label 是 key，由消费方用 i18n_list 解析。
//
//  线程模型：当前语言、偏好与文案表只在宿主 main 线程读写（lists.update 的
//  set_config 与 tick 都在 main 线程）；text()/format() 是纯查表，任意线程可调。
// ============================================================================

namespace Host {

/**
 * @brief 宿主界面语言中心：语言注册表、当前语言与宿主文案表。
 *
 * 进程内单例（instance()）；当前语言由 `core/language` 配置项驱动，
 * HostConfigurable::setConfig 校验后调用 setPreference，HostRuntime::tick 负责
 * 重建 i18n_list 并通知消费方。
 */
class I18n {
public:
    /** @brief 语言注册表项。 */
    struct Language {
        const char* tag;         /**< 规范 BCP-47 标签，如 "zh-CN"；比较时大小写不敏感 */
        const char* nativeName;  /**< 原生显示名，如 "简体中文" */
        const char* fallback;    /**< 该语言缺文本时的下一个候选；空串 = 用默认语言 */
    };

    /** @brief 宿主"自己产生、被消费方原样展示"的文案（按 id 取）。 */
    struct CoreText {
        const char* id;    /**< 文本 id，如 "capture.request" / "config.language.label" */
        const char* zhCN;  /**< 简体中文 */
        const char* enUS;  /**< 英文 */
    };

    /**
     * @brief 一条宿主生成的动态文案：消息 id + 最多两个模板参数。
     *
     * 注册表 reason / capture.failed.reason 这类字符串带参数（返回码、插件 id、
     * 依赖链），必须由宿主生成：消息模板放在 I18n 的文案表里，chinese() 固定
     * 给内部日志用（日志一律中文），localized() 给界面用（当前语言）。
     */
    class Message {
    public:
        /** @brief 构造空消息（empty() 为 true）。 */
        Message() = default;
        /**
         * @brief 构造无参消息。
         * @param[in] id 消息 id（见 src/hostI18n.cpp 的文案表）。
         */
        explicit Message(const char* id);
        /**
         * @brief 构造一个参数的消息。
         * @param[in] id   消息 id。
         * @param[in] arg0 替换模板里 {0} 的文本。
         */
        Message(const char* id, std::string arg0);
        /**
         * @brief 构造两个参数的消息。
         * @param[in] id   消息 id。
         * @param[in] arg0 替换模板里 {0} 的文本。
         * @param[in] arg1 替换模板里 {1} 的文本。
         */
        Message(const char* id, std::string arg0, std::string arg1);

        /** @brief 是否为空消息（无原因）。 */
        bool empty() const noexcept { return m_id.empty(); }

        /** @brief 中文文本：内部日志与错误信息固定用它，日志不随界面语言变化。 */
        std::string chinese() const;

        /** @brief 当前界面语言的文本：给被消费方原样展示的字段用。 */
        std::string localized() const;

    private:
        std::string m_id;                 /**< 消息 id；空 = 无消息 */
        std::array<std::string, 2> m_args;/**< 模板参数 {0} / {1} */
    };

    /** @brief 取进程级单例（首次调用时构造）。 */
    static I18n& instance();

    /** @brief 取语言注册表（顺序即设置页 ENUM 的顺序）。 */
    static const std::vector<Language>& languages();

    /**
     * @brief 按标签查语言注册表项（大小写不敏感，`_` 视同 `-`）。
     * @param[in] tag 候选标签，如 "zh-cn" / "zh_CN"。
     * @return 注册表项；不认识时返回 nullptr。
     */
    static const Language* findLanguage(std::string_view tag);

    /**
     * @brief 把标签规范成 BCP-47 写法（语言小写、地区大写，`_` 换成 `-`）。
     * @param[in] tag 任意写法的标签，如 "zh_cn" / "EN-us"。
     * @return 规范标签；空输入返回空串。
     */
    static std::string normalizeTag(std::string_view tag);

    /**
     * @brief 取标签的主语言（第一个 `-` 之前的部分）。
     * @param[in] tag 规范或非规范标签。
     * @return 主语言，如 "zh-CN" -> "zh"；没有 `-` 时返回整个标签。
     */
    static std::string primaryTag(std::string_view tag);

    /** @brief 默认回退语言标签（"en-US"）。 */
    static const char* defaultLanguage();

    /** @brief 语言偏好 `auto` 的字符串值。 */
    static const char* autoPreference();

    /** @brief `core/language` 配置项的 key（不带 "core/" 前缀）。 */
    static const char* preferenceKey();

    /** @brief 检测 OS 界面语言：Windows 用 GetUserDefaultLocaleName，其它平台回退 en-US。 */
    static std::string detectSystemLanguage();

    /**
     * @brief 从宿主配置读 `core/language` 并生效（启动时调用一次）。
     *
     * 配置里没有这个键（首次运行）时按 auto 处理；值不合法也只回退 auto。
     * 默认值本身由配置目录回灌时写入，这里不负责写配置。
     */
    void loadPreference();

    /**
     * @brief 设置语言偏好并立刻算出当前语言。
     * @param[in] value "auto" 或注册表里的标签（大小写不敏感）。
     * @return true = 已生效；false = 不认识的值，保持原偏好。
     */
    bool setPreference(std::string_view value);

    /**
     * @brief 当前语言偏好（"auto" 或规范标签）。
     * @return 偏好字符串，生命周期与单例相同。
     */
    const std::string& preference() const noexcept { return m_preference; }

    /**
     * @brief 当前生效语言（auto 已解析成具体标签）。
     * @return 规范标签，如 "zh-CN"；生命周期与单例相同。
     */
    const std::string& language() const noexcept { return m_language; }

    /**
     * @brief 当前语言在回退链上的下一站。
     * @return 注册表 fallback（zh 系为 "zh"）；没有配置 fallback 时返回默认语言。
     */
    std::string fallbackLanguage() const;

    /**
     * @brief 取宿主文案（当前语言）。
     * @param[in] id 文本 id；不能为 nullptr。
     * @return 文本；id 不认识时原样返回 id（便于日志里看出漏登记的 key）。
     * @note 返回静态字符串，进程生命周期内有效。
     */
    const char* text(const char* id) const;

    /**
     * @brief 取宿主文案（指定语言）。
     * @param[in] language 语言标签；空串/不认识时按默认语言处理。
     * @param[in] id       文本 id；不能为 nullptr。
     * @return 文本；id 不认识时原样返回 id。
     */
    std::string textIn(std::string_view language, const char* id) const;

    /**
     * @brief 取带模板参数的宿主文案（当前语言），替换 {0} / {1}。
     * @param[in] id   文本 id。
     * @param[in] args 模板参数，超出模板占位符的部分忽略。
     * @return 替换后的文本；id 不认识时返回 id。
     */
    std::string format(const char* id, const std::vector<std::string>& args) const;

    /**
     * @brief 取带模板参数的宿主文案（指定语言），替换 {0} / {1}。
     * @param[in] language 语言标签。
     * @param[in] id       文本 id。
     * @param[in] args     模板参数。
     * @return 替换后的文本；id 不认识时返回 id。
     */
    std::string formatIn(std::string_view language, const char* id,
                         const std::vector<std::string>& args) const;

    /**
     * @brief 按当前语言从"标签 -> 文本"的映射里挑一条。
     *
     * 回退链：完全匹配 -> 主语言族（如 zh 匹配 zh-CN 表）-> 默认 en-US 族 ->
     * 第一条可用文本。i18n_list 生成时对每个 key 用它选当前语言文本。
     *
     * @param[in] texts 标签 -> 文本；标签可以是非规范写法。
     * @return 选中的文本；texts 为空时返回空串。
     */
    std::string pickText(const std::map<std::string, std::string>& texts) const;

    /**
     * @brief 同 pickText，但按指定语言挑。
     * @param[in] language 语言标签。
     * @param[in] texts    标签 -> 文本。
     * @return 选中的文本；texts 为空时返回空串。
     */
    std::string pickTextIn(std::string_view language,
                           const std::map<std::string, std::string>& texts) const;

    /** @brief 取"会被发布进 i18n_list 的"宿主文案表（托盘/配置/ENUM/owner 名）。 */
    static const std::vector<CoreText>& displayTexts();

private:
    /** @brief 单例构造：偏好与当前语言先按 auto 初始化。 */
    I18n();

    /**
     * @brief 按偏好算出当前语言。
     * @param[in] preference "auto" 或规范标签。
     * @return 规范且受支持的语言标签；auto 解析不出来时是默认语言。
     */
    static std::string resolveLanguage(std::string_view preference);

    /**
     * @brief 查一条宿主文案（显示文案表 + 动态消息表）。
     * @param[in] id 文本 id。
     * @return 表项；不认识时返回 nullptr。
     */
    static const CoreText* findText(const char* id);

    /**
     * @brief 把语言标签映射到文案表里的列（本批只有 zh-CN / en-US 两列）。
     * @param[in] language 语言标签。
     * @return "zh-CN"（zh 语言族）或 "en-US"（其它）。
     */
    static const char* textColumn(std::string_view language);

    /**
     * @brief 在"标签 -> 文本"映射里找某个语言的文本。
     * @param[in] texts 标签 -> 文本（标签可以是任意写法）。
     * @param[in] tag   目标语言标签。
     * @param[out] out  命中时写入文本。
     * @return true = 命中（完全匹配或同语言族）。
     */
    static bool findTextFor(const std::map<std::string, std::string>& texts,
                            std::string_view tag, std::string& out);

    std::string m_preference; /**< "auto" 或规范标签 */
    std::string m_language;   /**< 当前生效的规范标签 */
};

}  // namespace Host
