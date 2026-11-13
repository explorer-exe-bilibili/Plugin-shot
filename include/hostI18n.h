#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// ============================================================================
//  Host::I18n —— 宿主侧界面语言（i18n）中心
//
//  这个类只解决宿主自己的那部分 i18n，跨插件文本由通用列表协议承担：
//    · 语言目录：内置注册表（tag + 原生显示名 + fallback，表驱动）∪ 外部语言包
//      的 `@language/` 声明（运行期由 HostListCatalog 从 i18n_source_list 投影，
//      调 setExternalLanguages 灌进来）。目录顺序 = 设置页 ENUM 的顺序；
//    · 当前语言：core/language 配置项（auto|<目录里的标签>），auto 解析 Windows
//      的 GetUserDefaultLocaleName，POSIX 依次读 LANGUAGE / LC_ALL / LC_MESSAGES / LANG，
//      都匹配不到时回退 en-US；
//    · 宿主自己的文案表：托盘命令 label、配置项 label/tooltip、ENUM 候选的原生
//      显示名，以及宿主产生、被消费方原样展示的动态文案（注册表 reason、
//      capture.failed.reason 等）；
//    · 语言表选择：给定"标签 -> 文本"的映射，按 完全匹配 -> 该语言的 fallback
//      -> 主语言族 -> 默认 en-US -> en 语言族 -> 第一条非空文本 的顺序挑一条；
//      回退链只有 selectText / pickFromTexts 这一份实现，插件侧
//      shot::I18n 必须与它完全一致（document/I18N.md §3.2）。
//
//  为什么宿主文案不走 key：这些字符串由宿主自己生成、只有宿主知道参数，
//  消费方拿到就该直接显示（document/I18N.md §4）。反过来说，插件贡献给
//  config_list / tray_command_list 的 label 是 key，由消费方用 i18n_list 解析。
//
//  线程模型：当前语言/偏好/外部语言目录只在宿主 main 线程写（set_config、tick、
//  列表重建），但查询函数可能在插件线程调用（例如采集 worker 线程生成
//  capture.failed.reason）。因此 m_preference / m_language / m_external 由同一
//  把小锁保护、查询先拷一份字符串（或目录）再锁外计算；锁内不调用任何外部代码，
//  Message::localized() 在任意线程都安全。
// ============================================================================

namespace Host {

/**
 * @brief 宿主界面语言中心：语言目录、当前语言与宿主文案表。
 *
 * 进程内单例（instance()）；当前语言由 `core/language` 配置项驱动，
 * HostConfigurable::setConfig 校验后调用 setPreference，HostRuntime::tick 负责
 * 重建 i18n_list 并通知消费方。外部语言（语言包声明）由 HostListCatalog 在每次
 * 重建 i18n_list 时通过 setExternalLanguages 换入，设置页语言下拉据此动态扩展。
 */
class I18n {
public:
    /** @brief 内置语言注册表项。 */
    struct Language {
        const char* tag;         /**< 规范 BCP-47 标签，如 "zh-CN"；比较时大小写不敏感 */
        const char* nativeName;  /**< 原生显示名，如 "简体中文" */
        const char* fallback;    /**< 该语言缺文本时的下一个候选；空串 = 用默认语言 */
    };

    /**
     * @brief 语言目录里的一条语言（内置注册表 ∪ 外部语言包声明）。
     *
     * 和 Language 的区别是字符串归本对象持有：外部语言是运行期才出现的，不能
     * 依赖进程生命周期的静态字符串。目录顺序就是设置页 ENUM 的顺序。
     */
    struct CatalogLanguage {
        std::string tag;      /**< 规范 BCP-47 标签，如 "ja-JP" */
        std::string name;     /**< 显示名：声明 name > 注册表 nativeName > 标签本身 */
        std::string fallback; /**< 缺文本时的下一站；空串 = 用默认语言 */
    };

    /** @brief 外部语言包（i18n_source_list 的 `@language/` 条目）声明的一条语言。 */
    struct ExternalLanguage {
        std::string tag;      /**< 规范 BCP-47 标签；空串的声明会被丢弃 */
        std::string name;     /**< 该语言自己的显示名；空 = 用注册表原生名/标签 */
        std::string fallback; /**< 缺文本时的下一站；空 = 默认语言 */
    };

    /** @brief 一条文案的某种语言文本。 */
    struct LanguageText {
        const char* tag;   /**< BCP-47 语言标签，如 "zh-CN"；比较时归一化 */
        const char* text;  /**< 该语言文本（UTF-8）；值必须在进程生命周期内有效 */
    };

    /**
     * @brief 宿主"自己产生、被消费方原样展示"的文案（按 id 取）。
     *
     * `texts` 是任意长度的语言列表：加语种只在这里加一项（以及各插件的语言表），
     * 不需要改宿主代码。顺序只在"回退链上所有语言都没有"时作为第一条兜底。
     */
    struct CoreText {
        const char* id;                 /**< 文本 id，如 "capture.request" / "config.language.label" */
        std::vector<LanguageText> texts;/**< 语言标签 -> 文本（至少一项） */
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

    /** @brief 取内置语言注册表（不含外部语言包声明；顺序即内置语言的 ENUM 顺序）。 */
    static const std::vector<Language>& languages();

    /**
     * @brief 语言目录：内置注册表在前，外部语言包声明按 tag 排序在后。
     *
     * 设置页语言下拉、`i18n_list` 的 `@meta`/语言显示名都从这里取；目录内容随
     * 语言包加载变化，catalogVersion() 是它的失效信号。
     *
     * @return 目录的拷贝（内部状态由锁保护，不返回引用）。
     */
    std::vector<CatalogLanguage> catalog() const;

    /**
     * @brief 用语言包声明的语言替换目录里的"外部部分"（幂等，锁外计算锁内交换）。
     *
     * 重复调用同样的声明不会改变目录、也不会动版本号；空 tag 的声明被丢弃。
     * 目录真的变化时同时会把"启动时因未知而搁置的语言偏好"补认成当前语言——
     * 外部语言包在 loadPreference 之后才加载，不补认的话用户选了语言包语言，
     * 重启后会永久退回 auto。
     *
     * @param[in] languages 外部声明；传空表示当前没有语言包声明。
     * @return true = 目录变了（catalogVersion() 随之 +1）。
     */
    bool setExternalLanguages(std::vector<ExternalLanguage> languages);

    /**
     * @brief 目录版本号：setExternalLanguages 真正改变目录时 +1，用于失效缓存。
     * @return 单调递增的版本号；进程启动时为 0。
     */
    std::uint64_t catalogVersion() const;

    /**
     * @brief 目录里是否有这个标签（内置或外部声明，大小写/`_` 不敏感）。
     * @param[in] tag 候选标签。
     * @return true = 在 catalog() 里能找到同标签。
     */
    static bool hasLanguage(std::string_view tag);

    /**
     * @brief 按标签查语言注册表项（大小写不敏感，`_` 视同 `-`）。
     * @param[in] tag 候选标签，如 "zh-cn" / "zh_CN"。
     * @return 注册表项；不认识时返回 nullptr。
     * @note 只看内置注册表；要判断"目录里有没有"（含语言包）请用 hasLanguage。
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

    /**
     * @brief 检测 OS 界面语言并映射到目录里的标签。
     *
     * Windows 用 GetUserDefaultLocaleName；POSIX 读 LANGUAGE（冒号分隔的优先级列表）、
     * LC_ALL / LC_MESSAGES / LANG，去掉 .编码 与 @修饰符、把下划线换成连字符；
     * 系统标签不在目录里时按主语言族匹配，
     * 仍没有才回退 en-US。其它平台本期不做探测、直接返回默认语言。
     * @return 目录里的规范标签或默认语言。
     */
    static std::string detectSystemLanguage();

    /**
     * @brief 从宿主配置读 `core/language` 并生效（启动时调用一次）。
     *
     * 配置里没有这个键（首次运行）时按 auto 处理；值不合法只回退 auto，并把该值
     * 记成"待认领"——外部语言包的语言要等插件加载后才声明，不记下来的话用户选了
     * 语言包语言，重启后偏好会永久退回 auto。
     * 默认值本身由配置目录回灌时写入，这里不负责写配置。
     */
    void loadPreference();

    /**
     * @brief 设置语言偏好并立刻算出当前语言。
     * @param[in] value "auto" 或目录里的标签（catalog()，大小写不敏感）。
     * @return true = 已生效；false = 不在目录里的值，保持原偏好。
     * @note 外部语言包的语言要等 setExternalLanguages 之后才在目录里。
     */
    bool setPreference(std::string_view value);

    /**
     * @brief 当前语言偏好（"auto" 或规范标签）。
     * @return 偏好字符串的拷贝（内部状态由锁保护，不返回引用）。
     */
    std::string preference() const;

    /**
     * @brief 当前生效语言（auto 已解析成具体标签）。
     * @return 规范标签的拷贝，如 "zh-CN"。
     */
    std::string language() const;

    /**
     * @brief 当前语言在回退链上的下一站。
     * @return 目录声明的 fallback（zh 系为 "zh"）；没有配置 fallback 时返回默认语言。
     */
    std::string fallbackLanguage() const;

    /**
     * @brief 指定语言在回退链上的下一站（声明 fallback > 注册表 fallback > 默认语言）。
     * @param[in] language 语言标签；空串/不在目录里时返回默认语言。
     * @return 规范标签；可能是一个主语言（如 zh-CN 的 "zh"）。
     */
    static std::string fallbackTag(std::string_view language);

    /**
     * @brief 取宿主文案（当前语言）。
     * @param[in] id 文本 id；不能为 nullptr。
     * @return 文本：认识的 id 返回静态表里的字符串（进程生命周期内有效），
     *         不认识的 id 原样返回调用方传入的指针。
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
     * 回退链见 selectText；i18n_list 生成时对每个 key 用它选当前语言文本。
     *
     * @param[in] texts 标签 -> 文本；标签可以是非规范写法。
     * @return 选中的文本；texts 为空时返回空串。
     */
    std::string pickText(const std::map<std::string, std::string>& texts) const;

    /**
     * @brief 同 pickText，但按指定语言挑。
     * @param[in] language 语言标签；空串按当前语言处理。
     * @param[in] texts    标签 -> 文本。
     * @return 选中的文本；texts 为空时返回空串。
     */
    std::string pickTextIn(std::string_view language,
                           const std::map<std::string, std::string>& texts) const;

    /**
     * @brief 按统一回退链从"标签 -> 文本"里选一条（宿主与插件 helper 同一套规则）。
     *
     * 顺序：完全匹配 -> 该语言的 fallback -> 主语言族（zh-TW 命中 zh-CN）->
     * 默认语言 en-US -> en 语言族 -> 第一条非空文本。空串视为缺失，
     * 一条可用文本都没有时返回空串（调用方决定是否显示 key 原文）。
     *
     * @param[in] texts       标签 -> 文本；标签可以是非规范写法。
     * @param[in] language    目标语言；空串按默认语言处理。
     * @param[in] fallbackTag 该语言的 fallback；空串表示没有配置（直接走默认语言）。
     * @return 选中的文本；一条可用文本都没有时返回空串。
     */
    static std::string selectText(const std::map<std::string, std::string>& texts,
                                  std::string_view language, std::string_view fallbackTag);

    /** @brief 取"会被发布进 i18n_list 的"宿主文案表（托盘/配置/ENUM/owner 名）。 */
    static const std::vector<CoreText>& displayTexts();

private:
    /** @brief 单例构造：只置 auto + 默认语言（真正的偏好在 loadPreference 里生效）。 */
    I18n();

    /**
     * @brief 按偏好算出当前语言（auto 走系统探测；其它值必须在目录里）。
     * @param[in] preference "auto" 或规范标签。
     * @return 规范且受支持的语言标签；解析不出来时是默认语言。
     */
    static std::string resolveLanguage(std::string_view preference);

    /**
     * @brief 查一条宿主文案（显示文案表 + 动态消息表）。
     * @param[in] id 文本 id。
     * @return 表项；不认识时返回 nullptr。
     */
    static const CoreText* findText(const char* id);

    /**
     * @brief 按统一回退链从一条文案的语言列表里选文本。
     *
     * 与 selectText 共用同一条链（完全匹配/fallback/主语言族/默认语言/en 族/
     * 第一条非空），区别只是返回指向静态表的指针、避免每条 core 文案都拷一次。
     *
     * @param[in] texts       语言列表；空列表返回 nullptr。
     * @param[in] language    目标语言标签。
     * @param[in] fallbackTag 该语言的 fallback；空串表示没有配置。
     * @return 选中的文本；一个都没有时 nullptr。
     */
    static const char* pickFromTexts(const std::vector<LanguageText>& texts,
                                     std::string_view language, std::string_view fallbackTag);

    /**
     * @brief 记住一个当前目录还认不出的语言偏好（loadPreference 用）。
     *
     * 外部语言包在插件加载后才声明自己的语言，启动时读到的 ja-JP 之类标签暂时
     * 不在目录里。记下来，等 setExternalLanguages 把目录补全后自动认领。
     * @param[in] value 配置里存的语言标签（可以是任意写法）。
     */
    void rememberUnresolvedPreference(std::string_view value);

    /**
     * @brief 目录变化后补认搁置的语言偏好（在锁外取目录、锁内赋值）。
     *
     * 认领成功后当前语言立即切换；HostRuntime::tick 的语言变化检测会顺带重建
     * i18n_list 与注册表快照，消费方无需额外事件。
     */
    void adoptUnresolvedPreference();

    /**
     * @brief 取当前语言的拷贝（持锁）。
     *
     * 查询函数先取一次拷贝再查静态文案表：避免插件线程读 m_language 时与 main
     * 线程的 setPreference 形成 std::string 数据竞争。
     * @return 当前生效语言的规范标签；单例构造后非空。
     */
    std::string currentLanguage() const;

    mutable std::mutex m_mutex;      /**< 保护 m_preference / m_language / m_external（读多写少） */
    std::string m_preference;        /**< "auto" 或规范标签 */
    std::string m_language;          /**< 当前生效的规范标签 */
    std::string m_unresolvedPreference; /**< 目录补全后要补认的偏好；空 = 没有 */
    std::vector<ExternalLanguage> m_external; /**< 语言包声明（规范标签、按 tag 排序） */
    std::uint64_t m_catalogVersion = 0; /**< 目录版本号；m_external 真变时 +1 */
};

}  // namespace Host
