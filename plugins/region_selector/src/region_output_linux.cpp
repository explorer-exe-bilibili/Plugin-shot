// region_output_linux —— 框选结果的落盘位置（Linux/Wayland 分支）。
//
// Wayland 分支不经过宿主流水线（截图在插件内存里，裁剪完直接写盘），所以"存到哪"
// 要自己读一次 image_saver 的配置，保持与流水线保存行为一致：
//   · 读 config_list（宿主生成的只读快照，document/LISTS.md §5.1）里
//     owner = "image_saver.main"、id = "directory" 的 value；
//   · 为空 / 读不到 → 系统图片目录（GLib 的 G_USER_DIRECTORY_PICTURES，也就是
//     $XDG_PICTURES_DIR，缺省 $HOME/Pictures，与 image_saver 的 POSIX 回退一致）；
//   · 目录不存在就建（image_saver 也这么做），建不出来就报错、不写文件。
//
// 文件名固定 shot_YYYYMMDD_HHMMSS.png（任务约定）：不跟随 image_saver 的 filename
// 模板，因为本插件不是"宿主流水线的保存策略"，只保证"当前时间 + PNG"。

#include "region_output.h"

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <string>

#include <glib.h>
#include <nlohmann/json.hpp>

#include "shot_plugin_helpers.h"

namespace region_selector {
namespace {

/** @brief image_saver 的插件 id（config_list 里的 owner）。 */
constexpr char kSaverPluginId[] = "image_saver.main";

/** @brief image_saver 的输出目录配置 key。 */
constexpr char kDirectoryKey[] = "directory";

/** @brief 目录权限：用户可写、组/其它可读可执行（与 g_mkdir_with_parents 的常规用法一致）。 */
constexpr int kDirectoryMode = 0755;

/**
 * @brief 从 config_list 快照里读 image_saver 的输出目录。
 * @param[in] host 宿主服务表，可为 nullptr。
 * @return 配置值；没有列表服务 / 插件未加载 / 值为空时返回空串。
 */
std::string readSaverDirectory(const ShotHostServices* host) {
    shot::PluginLists lists(host);
    if (!lists.available()) {
        return {};
    }
    const std::string text = lists.json("config_list");
    if (text.empty()) {
        return {};
    }
    const nlohmann::json items = nlohmann::json::parse(text, nullptr, false);
    if (items.is_discarded() || !items.is_array()) {
        shot::Logger(host).warn("region_selector：config_list 不是合法 JSON 数组，改用系统图片目录");
        return {};
    }
    for (const nlohmann::json& item : items) {
        if (!item.is_object()) {
            continue;
        }
        const auto owner = item.find("owner");
        const auto id = item.find("id");
        if (owner == item.end() || !owner->is_string() ||
            owner->get<std::string>() != kSaverPluginId) {
            continue;
        }
        if (id == item.end() || !id->is_string() || id->get<std::string>() != kDirectoryKey) {
            continue;
        }
        const auto value = item.find("value");
        if (value != item.end() && value->is_string()) {
            return value->get<std::string>();
        }
    }
    return {};
}

/**
 * @brief 系统图片目录：先问 GLib（尊重 $XDG_PICTURES_DIR），再退回 $HOME/Pictures。
 * @return 目录路径；都没有时返回空串。
 */
std::string picturesDirectory() {
    const char* pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (pictures != nullptr && pictures[0] != '\0') {
        return pictures;
    }
    const char* home = g_get_home_dir();
    if (home != nullptr && home[0] != '\0') {
        return std::string(home) + "/Pictures";
    }
    return {};
}

/** @brief 拼路径：目录末尾有 `/` 时不重复加。 */
std::string joinPath(const std::string& directory, const std::string& name) {
    if (directory.empty()) {
        return name;
    }
    if (directory.back() == '/') {
        return directory + name;
    }
    return directory + "/" + name;
}

}  // namespace

std::string resolveOutputDirectory(const ShotHostServices* host) {
    const shot::Logger log(host);
    std::string directory = readSaverDirectory(host);
    const bool fromConfig = !directory.empty();
    if (!fromConfig) {
        // 配置为空是**正常情况**（image_saver 的默认值就是空 = 系统图片目录），
        // 所以这里只在 debug 里说明来源，避免每次截图都刷 WARN。
        directory = picturesDirectory();
        if (directory.empty()) {
            log.warn("region_selector：找不到可用的图片目录（配置为空且 $HOME 不可用），本次不保存");
            return {};
        }
        log.debug("region_selector：未配置 image_saver.main/directory，使用系统图片目录 " + directory);
    }
    if (g_mkdir_with_parents(directory.c_str(), kDirectoryMode) != 0) {
        log.warn("region_selector：输出目录不可创建（" + directory + "）：" + g_strerror(errno) +
                 "，本次不保存");
        return {};
    }
    if (fromConfig) {
        log.info("region_selector：输出目录用 image_saver 配置 " + directory);
    }
    return directory;
}

std::string makeOutputPngPath(const std::string& directory) {
    if (directory.empty()) {
        return {};
    }
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    if (now == static_cast<std::time_t>(-1) || localtime_r(&now, &local) == nullptr) {
        return {};
    }
    char name[64] = {};
    std::snprintf(name, sizeof(name), "shot_%04d%02d%02d_%02d%02d%02d.png", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    return joinPath(directory, name);
}

}  // namespace region_selector
