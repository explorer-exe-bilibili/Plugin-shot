// tray_command_list → 菜单数据模型：JSON 解析、条目校验、排序、本地兜底。
//
// 为什么把解析单独成文件：菜单内容是宿主列表服务的运行期数据（core 预置三项 +
// 各插件 lists.add 的命令），托盘只是渲染器。解析规则集中在这里，托盘窗口代码
// 只处理 HMENU，两边可以分别验证——本文件不依赖 <windows.h>，用假 JSON 就能
// 离线验证排序 / 分隔线 / 兜底（tray_icon_menu.cpp 才是 Win32 那一半）。
//
// 条目 schema 与字段语义见 plugins/tray/README.md「菜单数据」一节。

#include "tray_menu.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "shot_plugin_helpers.h"

namespace tray {
namespace {

// 条目 order 的缺省值：与宿主 lists.add 的约定一致，缺省排在所有显式 order 之后。
constexpr int kDefaultOrder = 1000;

// 本地兜底条目：列表服务不可用时唯一还能做的事——退出宿主，避免托盘变成只能看。
constexpr char kFallbackId[] = "tray.fallback.quit";
constexpr char kFallbackLabel[] = "退出";
constexpr char kFallbackEvent[] = "app.quit";

// 下面四个字段读取函数只服务本文件的解析循环、与 TrayMenu 的状态无关，所以放
// 匿名命名空间（STYLE.md §6.7 允许"确实与任何类无关、且只在本 TU 用的具名函数"）。
// 一律做类型检查而不是 get<T>()：宿主可能转发任意插件写的坏 JSON，抛异常会让
// 托盘线程 terminate，坏条目的正确归宿是被跳过并记一条 WARN。

/** @brief 取字符串字段；字段缺省或类型不对时返回空串。 */
std::string stringField(const nlohmann::json& entry, const char* key) {
    const auto it = entry.find(key);
    if (it == entry.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

/** @brief 取布尔字段；字段缺省或类型不对时返回 fallbackValue。 */
bool boolField(const nlohmann::json& entry, const char* key, bool fallbackValue) {
    const auto it = entry.find(key);
    if (it == entry.end() || !it->is_boolean()) {
        return fallbackValue;
    }
    return it->get<bool>();
}

/** @brief 取 order；字段缺省或不是数字时按宿主约定 kDefaultOrder。 */
int intField(const nlohmann::json& entry, const char* key, int fallbackValue) {
    const auto it = entry.find(key);
    if (it == entry.end() || !it->is_number()) {
        return fallbackValue;
    }
    return static_cast<int>(it->get<double>());
}

/**
 * @brief 取 data 负载：约定是字符串，但宿主按"插件写什么就存什么"转发条目，
 *        所以对象 / 数组 / 数字等非字符串负载序列化成 JSON 文本再发布。
 */
std::string dataField(const nlohmann::json& entry) {
    const auto it = entry.find("data");
    if (it == entry.end() || it->is_null()) {
        return {};
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    // error_handler=replace：data 里混进非法 UTF-8 时也安静替换，不抛异常。
    return it->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace

void TrayMenu::installFallback(const ShotHostServices* host, const char* reason) {
    TrayCommandItem item;
    item.id = kFallbackId;
    item.label = kFallbackLabel;
    item.event = kFallbackEvent;
    item.order = 0;  // 只有它一项，order 只用于语义完整
    m_items.clear();
    m_items.push_back(std::move(item));
    m_fallback = true;
    // 兜底说明为什么菜单退化成一个「退出」：排障时靠这条日志区分"宿主没给列表"
    // 和"列表数据坏了"。
    shot::Logger(host).warn(std::string("[tray] ") + reason +
                            "，菜单只保留本地兜底「退出」(app.quit)");
}

std::size_t TrayMenu::load(const ShotHostServices* host) {
    // 列表服务可能不存在（旧宿主 / 宿主还没注册这个列表）：json() 会安静返回空串，
    // 下面按"列表不可用"走本地兜底，不会把托盘拖垮。
    return loadFromJson(host, shot::PluginLists(host).json(kTrayCommandListId));
}

std::size_t TrayMenu::loadFromJson(const ShotHostServices* host, const std::string& jsonText) {
    m_items.clear();
    m_fallback = false;
    shot::Logger log(host);
    if (jsonText.empty()) {
        installFallback(host, "tray_command_list 不可用（宿主未提供列表服务或列表未注册）");
        return m_items.size();
    }
    const nlohmann::json doc =
        nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_array()) {
        installFallback(host, "tray_command_list 不是合法的 JSON 数组");
        return m_items.size();
    }

    std::size_t skipped = 0;
    for (const nlohmann::json& entry : doc) {
        if (!entry.is_object()) {
            ++skipped;
            continue;
        }
        TrayCommandItem item;
        item.id = stringField(entry, "id");
        item.label = stringField(entry, "label");
        item.event = stringField(entry, "event");
        item.data = dataField(entry);
        item.order = intField(entry, "order", kDefaultOrder);
        item.separator = boolField(entry, "separator", false);
        item.isDefault = boolField(entry, "default", false);
        if (item.separator) {
            // 分隔线不需要 label / event：order 决定它落在哪两个命令之间。
            m_items.push_back(std::move(item));
            continue;
        }
        if (item.label.empty()) {
            item.label = item.id;  // label 缺省时退回 id，至少让用户知道点的是什么
        }
        if (item.label.empty() || item.event.empty()) {
            // 命令缺文字或缺事件：点了也没有意义，跳过而不是渲染一个死菜单项。
            ++skipped;
            log.warn("[tray] tray_command_list 有条目缺 label/id 或 event，已跳过");
            continue;
        }
        m_items.push_back(std::move(item));
    }

    // 排序放在最后：分隔线也参与排序，它的 order 决定它出现在哪两项之间。
    // 稳定排序保证同 order 的条目保持宿主给出的顺序。
    std::stable_sort(m_items.begin(), m_items.end(),
                     [](const TrayCommandItem& lhs, const TrayCommandItem& rhs) {
                         return lhs.order < rhs.order;
                     });

    if (m_items.empty()) {
        installFallback(host, "tray_command_list 里没有可用条目");
        return m_items.size();
    }

    std::size_t commands = 0;
    std::size_t separators = 0;
    for (const TrayCommandItem& item : m_items) {
        (item.separator ? separators : commands) += 1;
    }
    log.info("[tray] 菜单数据就绪：tray_command_list 里 " + std::to_string(commands) +
             " 个命令、" + std::to_string(separators) + " 条分隔线" +
             (skipped > 0 ? "（跳过 " + std::to_string(skipped) + " 个坏条目）" : ""));
    return m_items.size();
}

}  // namespace tray
