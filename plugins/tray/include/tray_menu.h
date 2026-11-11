#pragma once

// ============================================================================
//  include/tray_menu.h —— 托盘菜单的数据模型（tray_command_list）
//
//  托盘不再硬编码任何命令：右键菜单的内容全部来自宿主列表服务里的
//  tray_command_list（宿主预置 core 三项，插件运行期还可以 lists.add 加命令）。
//  本类只负责"JSON 快照 → 可渲染条目"：校验、排序、本地兜底；HMENU 与
//  TrackPopupMenu 在 tray_icon_menu.cpp——数据和窗口分开后，解析逻辑不依赖
//  Win32，可以脱离托盘线程用假 JSON 单独验证。
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

#include "plugin_api.h"

namespace shot {
class I18n;
}  // namespace shot

namespace tray {

/** @brief tray_command_list 的列表 id（宿主登记的只读 JSON 列表，见 document/LISTS.md）。 */
inline constexpr char kTrayCommandListId[] = "tray_command_list";

/**
 * @brief 一个可渲染的托盘菜单条目。
 *
 * 对应 tray_command_list 数组里的一项
 * `{"id":"core.capture","label":"截屏","event":"capture.request","data":"",
 *   "order":10,"separator":false,"default":false}`；字段语义见
 * plugins/tray/README.md 的「菜单数据」一节。
 *
 * 字段一律是 UTF-8 文本：Win32 的宽字符在渲染时再转（tray_icon_menu.cpp），
 * 数据层不依赖 <windows.h>。
 */
struct TrayCommandItem {
    std::string id;         /**< 稳定标识：宿主按 (owner,id) upsert；托盘只用它写日志。 */
    std::string owner;      /**< 贡献方 id；label 是文案 key 时用它查 i18n_list，separator 可为空。 */
    std::string label;      /**< 菜单文字或文案 key；separator 条目可为空。 */
    std::string event;      /**< 点击时发布的事件主题；separator 条目可为空。 */
    std::string data;       /**< 事件负载（字符串）；空串 = 无负载。 */
    int order = 1000;       /**< 排序键，升序；缺省按宿主约定 1000（排在最后）。 */
    bool separator = false; /**< true = 渲染成分隔线（不可点击）。 */
    bool isDefault = false; /**< true = 左键单击托盘图标时执行它（无则回退 capture.request）。 */
};

/**
 * @brief 托盘菜单的只读数据模型：解析 tray_command_list、校验、排序、本地兜底。
 *
 * 解析规则（宿主 / 插件约定）：
 *   - 只接受 JSON 数组；条目必须是对象，`event` 非空才可点击；
 *   - `order` 升序稳定排序（同 order 保持宿主给出的顺序），缺省 1000；
 *   - `separator:true` 渲染成分隔线，`label` / `event` 可以缺省；
 *   - `label` 为空时退回 `id`，两者都空或命令缺 `event` 的条目跳过并记 WARN；
 *   - 列表服务不可用 / 列表为空 / JSON 非法时装入本地兜底：只有一项「退出」
 *     （app.quit），保证用户永远有办法从托盘退出宿主。
 *
 * 本类不是线程安全的：只在托盘线程上、或离线自测里使用。
 */
class TrayMenu {
public:
    /**
     * @brief 从宿主列表服务读取 tray_command_list 快照，重建模型。
     * @param[in] host 宿主服务表；nullptr 或列表服务不可用时按"列表不可用"装本地兜底。
     * @param[in] i18n 多语言解析器；nullptr 时 label 原样保留（老宿主/降级路径）。
     * @return 模型里的条目数（兜底时固定为 1）。
     * @note 两段式只读快照由 shot::PluginLists 封装，任意线程可调；返回的是本地副本。
     */
    std::size_t load(const ShotHostServices* host, const shot::I18n* i18n);

    /**
     * @brief 用一段 JSON 文本重建模型（生产路径由 load 调用；离线自测也走这里）。
     * @param[in] host     宿主服务表，只用于写日志；可为 nullptr（日志静默丢弃）。
     * @param[in] jsonText 与 list_json("tray_command_list") 同形状的 UTF-8 JSON 文本。
     * @param[in] i18n     多语言解析器；nullptr 时 label 原样保留。
     * @return 模型里的条目数（兜底时固定为 1）。
     */
    std::size_t loadFromJson(const ShotHostServices* host, const std::string& jsonText,
                             const shot::I18n* i18n);

    /** @brief 当前条目：已按 order 升序，separator 条目也在其中。 */
    const std::vector<TrayCommandItem>& items() const noexcept { return m_items; }

    /** @brief 当前模型是否来自本地兜底（列表不可用 / JSON 非法 / 列表为空）。 */
    bool isFallback() const noexcept { return m_fallback; }

private:
    /**
     * @brief 装入本地兜底：只有一个「退出」（app.quit）菜单项。
     * @param[in] host   宿主服务表，只用于写日志；可为 nullptr。
     * @param[in] reason 走兜底的原因（中文短语），写进 WARN 日志方便排障。
     */
    void installFallback(const ShotHostServices* host, const char* reason,
                         const shot::I18n* i18n);

    std::vector<TrayCommandItem> m_items; /**< 已解析、已排序的条目。 */
    bool m_fallback = false;              /**< m_items 来自本地兜底而不是宿主列表。 */
};

}  // namespace tray
