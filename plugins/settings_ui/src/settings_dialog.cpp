// settings_dialog —— 原生文件/目录选择对话框（nativefiledialog-extended）。
//
// 设置页的 PATH 配置项需要"挑一个文件/目录"。旧实现用 Win32 的 GetOpenFileNameW，
// 跨平台后改用 nativefiledialog-extended：Windows 走 IFileDialog，macOS 走 NSOpenPanel，
// Linux 走 GTK/portal，调用方只看到 UTF-8 路径。
//
// 失败策略（SETTINGS.md 的"安静降级"）：对话框库初始化失败时返回 false + 中文原因，
// 调用方把原因显示在配置项旁边，用户可以直接手输路径，不影响其它功能。
// 因此这里懒初始化：只有第一次真的要弹对话框时才 NFD_Init()，没人用就不碰它。
//
// TODO: POSIX 未验证。macOS 上 NFD（Cocoa）要求在进程主线程初始化/弹窗，而本插件的
// UI 线程是自建线程，可能需要把对话框调用转到主线程；Linux 上 NFD 依赖 GTK3/portal，
// 目标机器没有时就是本文件覆盖的"不可用 → 手输"降级路径。等有真机再验证并修正。

#include "settings_dialog.h"

#include <string>

#include <nfd.h>

#include "shot_plugin_helpers.h"

namespace settings_ui {
namespace {

bool g_initialized = false;  // UI 线程独占，不需要锁
bool g_available = false;
std::string g_initLibraryError;  // NFD_Init 失败时的库错误文本（不含本地化外壳）

/**
 * @brief 把"对话框不可用"的完整提示按当前语言拼出来。
 *
 * 库错误文本本身是第三方的（可能是英文），只做外壳本地化：不同语言的语序不同，
 * 所以拆成"不可用 / 冒号 / 原文 / 请直接输入路径"四段，由语言表决定各段文本。
 *
 * @param[in] i18n         多语言解析器；nullptr 时用中文原文。
 * @param[in] libraryError 库给出的错误文本；可为空。
 * @return 当前语言的完整提示。
 */
std::string dialogUnavailableText(const shot::I18n* i18n, const char* libraryError) {
    const auto tr = [i18n](const char* key, const char* fallback) -> const char* {
        return (i18n != nullptr) ? i18n->tr(key, fallback) : fallback;
    };
    std::string text = tr("settings_ui.dialog_unavailable", "文件对话框不可用");
    if (libraryError != nullptr && libraryError[0] != '\0') {
        text += tr("settings_ui.dialog_error_prefix", "：");
        text += libraryError;
    }
    text += tr("settings_ui.dialog_error_suffix", "，请直接输入路径");
    return text;
}

/**
 * @brief 懒初始化对话框库；只在第一次弹窗时调用。
 * @param[out] error 初始化失败时的中文原因（含库自己给出的错误文本）。
 * @return true = 对话框库可用。
 */
bool ensureFileDialogsInitialized(std::string& error) {
    if (g_initialized) {
        error = g_available ? std::string() : g_initLibraryError;
        return g_available;
    }
    g_initialized = true;
    if (NFD_Init() == NFD_OKAY) {
        g_available = true;
        return true;
    }
    g_available = false;
    const char* libraryError = NFD_GetError();
    g_initLibraryError = (libraryError != nullptr) ? libraryError : "";
    error = g_initLibraryError;
    return false;
}

/**
 * @brief 把一次 NFD 调用的结果翻译成路径/错误/取消。
 * @param[in] result NFD 返回值。
 * @param[in] outPath 成功时 NFD 分配、需要在这里释放的 UTF-8 路径。
 * @param[out] path 成功时的路径。
 * @param[out] error 失败时的原因；用户取消时清空。
 * @return true = 用户选了路径。
 */
bool finishDialog(nfdresult_t result, nfdu8char_t* outPath, std::string& path,
                  std::string& error) {
    error.clear();
    if (result == NFD_OKAY && outPath != nullptr) {
        path = outPath;
        NFD_FreePathU8(outPath);
        return true;
    }
    if (outPath != nullptr) {
        NFD_FreePathU8(outPath);
    }
    if (result == NFD_CANCEL) {
        return false;  // 用户取消不是错误：error 留空，调用方只忽略即可
    }
    return false;  // 库错误的本地化外壳由调用方补（需要它的 i18n）
}

}  // namespace

std::string localizedDialogError(const shot::I18n* i18n, const std::string& libraryError) {
    return dialogUnavailableText(i18n, libraryError.empty() ? nullptr : libraryError.c_str());
}

bool pickExistingFile(std::string& path, std::string& error, const shot::I18n* i18n) {
    std::string libraryError;
    if (!ensureFileDialogsInitialized(libraryError)) {
        error = localizedDialogError(i18n, libraryError);
        return false;
    }
    // 过滤器名字也是用户可见文案：按当前语言取（指针在三方调用期间一直有效）。
    const nfdu8filteritem_t filters[] = {
        { (i18n != nullptr) ? i18n->tr("settings_ui.dialog_all_files", "所有文件 (*.*)")
                            : "所有文件 (*.*)",
          "*" },
    };
    nfdu8char_t* outPath = nullptr;
    const nfdresult_t result =
        NFD_OpenDialogU8(&outPath, filters, static_cast<nfdfiltersize_t>(1), nullptr);
    if (finishDialog(result, outPath, path, error)) {
        return true;
    }
    if (!error.empty() || result == NFD_CANCEL) {
        return false;  // 用户取消：不是错误，调用方只忽略
    }
    error = localizedDialogError(i18n, NFD_GetError() != nullptr ? NFD_GetError() : "");
    return false;
}

bool pickExistingFolder(std::string& path, std::string& error, const shot::I18n* i18n) {
    std::string libraryError;
    if (!ensureFileDialogsInitialized(libraryError)) {
        error = localizedDialogError(i18n, libraryError);
        return false;
    }
    nfdu8char_t* outPath = nullptr;
    const nfdresult_t result = NFD_PickFolderU8(&outPath, nullptr);
    if (finishDialog(result, outPath, path, error)) {
        return true;
    }
    if (!error.empty() || result == NFD_CANCEL) {
        return false;
    }
    error = localizedDialogError(i18n, NFD_GetError() != nullptr ? NFD_GetError() : "");
    return false;
}

void shutdownFileDialogs() {
    if (g_initialized && g_available) {
        NFD_Quit();
    }
    g_initialized = false;
    g_available = false;
    g_initLibraryError.clear();
}

}  // namespace settings_ui
