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

namespace settings_ui {
namespace {

bool g_initialized = false;  // UI 线程独占，不需要锁
bool g_available = false;
std::string g_initError;

const nfdu8filteritem_t kFileFilters[] = {
    { "所有文件 (*.*)", "*" },
};

/**
 * @brief 懒初始化对话框库；只在第一次弹窗时调用。
 * @param[out] error 初始化失败时的中文原因（含库自己给出的错误文本）。
 * @return true = 对话框库可用。
 */
bool ensureFileDialogsInitialized(std::string& error) {
    if (g_initialized) {
        error = g_available ? std::string() : g_initError;
        return g_available;
    }
    g_initialized = true;
    if (NFD_Init() == NFD_OKAY) {
        g_available = true;
        return true;
    }
    g_available = false;
    const char* libraryError = NFD_GetError();
    g_initError = "文件对话框不可用";
    if (libraryError != nullptr && libraryError[0] != '\0') {
        g_initError += std::string("：") + libraryError;
    }
    g_initError += "，请直接输入路径";
    error = g_initError;
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
    const char* libraryError = NFD_GetError();
    error = "文件对话框不可用";
    if (libraryError != nullptr && libraryError[0] != '\0') {
        error += std::string("：") + libraryError;
    }
    error += "，请直接输入路径";
    return false;
}

}  // namespace

bool pickExistingFile(std::string& path, std::string& error) {
    if (!ensureFileDialogsInitialized(error)) {
        return false;
    }
    nfdu8char_t* outPath = nullptr;
    const nfdresult_t result = NFD_OpenDialogU8(&outPath, kFileFilters,
                                                static_cast<nfdfiltersize_t>(1), nullptr);
    return finishDialog(result, outPath, path, error);
}

bool pickExistingFolder(std::string& path, std::string& error) {
    if (!ensureFileDialogsInitialized(error)) {
        return false;
    }
    nfdu8char_t* outPath = nullptr;
    const nfdresult_t result = NFD_PickFolderU8(&outPath, nullptr);
    return finishDialog(result, outPath, path, error);
}

void shutdownFileDialogs() {
    if (g_initialized && g_available) {
        NFD_Quit();
    }
    g_initialized = false;
    g_available = false;
    g_initError.clear();
}

}  // namespace settings_ui
