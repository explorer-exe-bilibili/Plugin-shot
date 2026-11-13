// 通知点击行为：用户点了成功通知之后，按配置打开图片或打开所在文件夹。
//
// 事件从哪来：隐藏占位图标的 uCallbackMessage（kMsgNotifyIconEvent，见
// notify_toast_impl.h）——Shell 在用户点击气泡时投 NIN_BALLOONUSERCLICK 到窗口过程，
// 本文件把它翻译成一次 ShellExecuteW。整条链路都在通知线程上，不跨线程。
//
// 为什么记住"最近一次的图片路径"而不是把路径塞进 Shell：NOTIFYICONDATA 没有携带
// 自定义载荷的字段，气泡的标题/正文也不能回读；所以 onCaptureDone 在发气泡时记下
// lastImagePath，点击时用它。失败通知没有图片路径，会清掉这个值，点它不做任何事。
//
// 安全约定：图片不存在只 WARN，绝不弹 MessageBox / 错误对话框——点击通知是低风险
// 动作，打断用户比什么都不做更糟。

#include "notify_toast.h"

#include <string>

#include "notify_config.h"
#include "notify_toast_impl.h"
#include "shot_plugin_helpers.h"

namespace notify {
namespace {

/** @brief 文件是否存在；空路径返回 false。 */
bool fileExists(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/**
 * @brief UTF-16 → UTF-8，只用于写日志（host.log 是 UTF-8）。
 * @param[in] text 宽字符串；空串返回空串。
 * @return UTF-8 文本；转换失败返回空串（日志里看不出路径，但不影响功能）。
 */
std::string toUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return std::string();
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes,
                        nullptr, nullptr);
    return utf8;
}

/** @brief ShellExecuteW 的返回值 > 32 才算启动成功（Win32 的古老约定）。 */
bool launched(HINSTANCE result) {
    return reinterpret_cast<INT_PTR>(result) > 32;
}

/** @brief 把 ShellExecuteW 的返回值转成可读日志文本（<= 32 是错误码）。 */
std::string launchResultText(HINSTANCE result) {
    return std::to_string(static_cast<long long>(reinterpret_cast<INT_PTR>(result)));
}

}  // namespace

void NotifyToast::Impl::onNotifyIconEvent(LPARAM lparam) {
    // 只有 NIN_BALLOONUSERCLICK 代表"用户点了通知"；其余是气泡显示/隐藏/超时之类的
    // 生命周期事件，不需要动作。lParam 低位是通知码，高位只有调用 NIM_SETVERSION
    // （NOTIFYICON_VERSION_4）时才是图标 id，本插件没调，取低位即可。
    switch (LOWORD(lparam)) {
        case NIN_BALLOONUSERCLICK:
            openLastImage();
            return;
        case NIN_BALLOONSHOW:
            return;  // 气泡已显示：无需处理，避免每次截屏多刷一行日志
        case NIN_BALLOONHIDE:
        case NIN_BALLOONTIMEOUT:
            return;  // 用户没点：不需要动作
        default:
            return;  // 隐藏图标的其它 Shell 回调（右键/悬停等）与本插件无关
    }
}

void NotifyToast::Impl::openLastImage() {
    if (lastImagePath.empty()) {
        shot::Logger(host).debug("[notify] 通知被点击，但这条通知没有图片路径（失败通知），忽略");
        return;
    }
    if (!fileExists(lastImagePath)) {
        // 图片可能已被用户或清理工具删掉：只 WARN，绝不弹错误框打断用户。
        shot::Logger(host).warn("[notify] 通知被点击，但图片已不存在：" + toUtf8(lastImagePath));
        return;
    }
    const ClickAction action =
        (config != nullptr) ? config->clickAction() : ClickAction::OpenImage;
    shot::Logger(host).info("[notify] 通知被点击：" +
                            std::string(NotifyConfig::clickActionKey(action)) + " " +
                            toUtf8(lastImagePath));
    if (action == ClickAction::OpenFolder) {
        openContainingFolder();
    } else {
        openImageFile();
    }
}

void NotifyToast::Impl::openImageFile() {
    // "open" 交给 Shell 按文件关联选择看图程序：与在资源管理器里双击的行为一致。
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", lastImagePath.c_str(), nullptr,
                                           nullptr, SW_SHOWNORMAL);
    if (launched(result)) {
        shot::Logger(host).info("[notify] 已打开图片：" + toUtf8(lastImagePath));
        return;
    }
    shot::Logger(host).warn("[notify] 打开图片失败（ShellExecuteW 返回 " + launchResultText(result) +
                            "）");
}

void NotifyToast::Impl::openContainingFolder() {
    // explorer.exe /select,"<path>"：打开父目录并选中文件。Windows 文件名不允许双引号，
    // 所以这里的引号拼接不会破坏参数（路径也不需要再做别的转义）。
    // 用 ShellExecuteW 而不是 CreateProcessW：不用自己拼命令行首进程、不用等待进程退出。
    std::wstring arguments = L"/select,\"";
    arguments += lastImagePath;
    arguments += L"\"";
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(),
                                           nullptr, SW_SHOWNORMAL);
    if (launched(result)) {
        shot::Logger(host).info("[notify] 已在资源管理器中定位图片：" + toUtf8(lastImagePath));
        return;
    }
    shot::Logger(host).warn("[notify] 打开所在文件夹失败（ShellExecuteW 返回 " +
                            launchResultText(result) + "）");
}

}  // namespace notify
