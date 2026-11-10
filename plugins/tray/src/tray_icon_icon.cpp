// 托盘图标的来源与刷新：宿主应用图标（默认）/ `icon` 配置的 .ico 文件 / 系统兜底。
//
// 为什么默认用宿主图标：托盘是"Plugin-shot 的入口"，应该显示整个应用的图标。
// 宿主 exe 的 app.rc 里有 `IDI_ICON1 ICON "Plugin-shot.ico"`——这是**命名资源**，
// 所以 LoadIconW 要按资源名字符串找；有的工程会写成 `1 ICON ...`（数字 ID 1），
// 这里两种都试（先名字、再数字 ID 1），避免宿主换 rc 写法后托盘突然掉图标。
// 在插件 DLL 里 GetModuleHandleW(nullptr) 拿到的是宿主 exe 模块，不是本插件 DLL。
//
// 图标来源优先级（失败逐级回退，每一步都留日志）：
//   1. `icon` 配置非空 → LoadImageW(LR_LOADFROMFILE) 从 .ico 文件加载；
//   2. 配置为空 / 文件加载失败 → 宿主 exe 的 IDI_ICON1；
//   3. 宿主图标也拿不到 → 系统默认应用图标 IDI_APPLICATION。
//
// 句柄归属：LoadIconW 返回的宿主/系统图标是共享资源，绝不能 DestroyIcon；
// LoadImageW 从文件加载出来的图标归本插件所有，换图标/删图标时必须 DestroyIcon。
// ResolvedIcon 把"句柄 / 能否销毁 / 来源"绑在一起上报，日志与刷新比较都只看它。
//
// 所有函数都只在托盘线程上调用：config.applied / 内部刷新事件先入队，由
// drainEvents 转到托盘线程后再碰 Win32。

#include "tray_icon.h"

#include <cwchar>
#include <string>
#include <utility>

#include "shot_plugin_helpers.h"
#include "tray_config.h"
#include "tray_icon_impl.h"

namespace tray {
namespace {

// 宿主 exe 的图标资源名：app.rc.in 里 `IDI_ICON1 ICON "Plugin-shot.ico"` 是命名资源。
constexpr wchar_t kHostIconResourceName[] = L"IDI_ICON1";

// 兼容宿主把 rc 写成数字 ID 的写法：`1 ICON "..."`。
constexpr int kHostIconResourceId = 1;

}  // namespace

std::string TrayIcon::Impl::iconSourceDescription() const {
    if (iconOwned) {
        return "文件 " + iconConfigPath;
    }
    if (iconHostResource) {
        return std::string("宿主应用图标(IDI_ICON1)");
    }
    return std::string("系统默认图标(IDI_APPLICATION)");
}

void TrayIcon::Impl::releaseIcon() {
    if (iconOwned && icon != nullptr) {
        // 只有 LoadImageW 从文件加载的图标归本插件所有。销毁共享图标会连累其它
        // 使用同一资源的窗口，所以这里必须看 iconOwned 而不是看句柄非空。
        DestroyIcon(icon);
    }
    icon = nullptr;
    iconOwned = false;
    iconHostResource = false;
    iconConfigPath.clear();
}

void TrayIcon::Impl::adoptIcon(const ResolvedIcon& resolved) {
    releaseIcon();
    icon = resolved.handle;
    iconOwned = resolved.owned;
    iconHostResource = resolved.hostResource;
    iconConfigPath = resolved.configPath;
}

bool TrayIcon::Impl::resolveIcon(ResolvedIcon& out) {
    out = ResolvedIcon{};

    // 1. 配置了 icon 就先用文件图标；失败只 WARN，继续回退宿主图标。
    if (config != nullptr) {
        const std::string path = config->iconPath();
        if (!path.empty()) {
            // 配置值按契约是 UTF-8；LoadImageW 要 UTF-16，所以先转宽字符。
            // 用宽字符版本而不是 LoadImageA：A 版按系统 ANSI 代码页解释路径，
            // 中文/emoji 路径会直接失败。
            const std::wstring wide = toWide(path);
            HICON fileIcon = wide.empty()
                ? nullptr
                : static_cast<HICON>(LoadImageW(nullptr, wide.c_str(), IMAGE_ICON, 0, 0,
                                                LR_LOADFROMFILE | LR_DEFAULTSIZE));
            if (fileIcon != nullptr) {
                out.handle = fileIcon;
                out.owned = true;
                out.configPath = path;
                return true;
            }
            shot::Logger(host).warn(
                "[tray] icon 配置的图标文件加载失败（不存在或不是 .ico），回退宿主应用图标；"
                "配置值: " + path);
        }
    }

    // 2. 宿主应用图标：app.rc 里是命名资源 IDI_ICON1；先按名字找，再兼容数字 ID 1。
    const HINSTANCE hostModule = GetModuleHandleW(nullptr);  // 宿主 exe 模块，不是本 DLL
    HICON hostIcon = LoadIconW(hostModule, kHostIconResourceName);
    if (hostIcon == nullptr) {
        hostIcon = LoadIconW(hostModule, MAKEINTRESOURCEW(kHostIconResourceId));
    }
    if (hostIcon != nullptr) {
        out.handle = hostIcon;
        out.hostResource = true;
        return true;
    }
    shot::Logger(host).warn("[tray] 加载宿主应用图标失败（IDI_ICON1），回退系统默认图标");

    // 3. 系统默认图标兜底：连它都没有就没法加托盘图标了。
    out.handle = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    if (out.handle == nullptr) {
        shot::Logger(host).error("[tray] 加载系统默认图标失败，托盘图标不可用");
        return false;
    }
    return true;
}

bool TrayIcon::Impl::addIcon(HWND wnd) {
    ResolvedIcon resolved;
    if (!resolveIcon(resolved)) {
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = wnd;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = trayCallbackMessage;
    data.hIcon = resolved.handle;
    // szTip 是定长缓冲；_TRUNCATE 保证超长时安全截断，不会写坏结构体。
    wcsncpy_s(data.szTip, kTooltip, _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        shot::Logger(host).error("[tray] Shell_NotifyIconW(NIM_ADD) 失败");
        if (resolved.owned) {
            DestroyIcon(resolved.handle);  // NIM_ADD 失败：刚加载的文件图标不能泄漏
        }
        return false;
    }

    // NIM_ADD 成功后才替换当前句柄：失败路径不能把旧图标弄丢。
    adoptIcon(resolved);
    iconAdded = true;
    shot::Logger(host).info("[tray] 托盘图标已添加（来源 " + iconSourceDescription() + "）");

    // 再向 Shell 查一次图标矩形：确认图标真的挂上了，也方便排障时知道它在哪。
    // 系统把图标放进通知区域溢出区时可能查不到，这不是错误，所以只当调试信息。
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = wnd;
    identifier.uID = kTrayIconId;
    RECT rect{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &rect))) {
        shot::Logger(host).debug("[tray] 托盘图标在通知区域的位置 " + std::to_string(rect.left) +
                                 "," + std::to_string(rect.top) + " " +
                                 std::to_string(rect.right - rect.left) + "x" +
                                 std::to_string(rect.bottom - rect.top));
    }
    return true;
}

void TrayIcon::Impl::removeIcon() {
    if (iconAdded) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        data.uID = kTrayIconId;
        if (!Shell_NotifyIconW(NIM_DELETE, &data)) {
            // 删不掉只能用日志留痕：窗口马上要销毁，Shell 最终也会清理这个图标。
            shot::Logger(host).warn("[tray] Shell_NotifyIconW(NIM_DELETE) 失败");
        }
        iconAdded = false;
    }
    // 文件加载的图标必须在这里释放（共享图标不会进 iconOwned 分支）；
    // 线程退出后进程仍可能继续跑（禁用→重新启用），泄漏会累积。
    releaseIcon();
}

void TrayIcon::Impl::refreshIcon() {
    if (!iconAdded || hwnd == nullptr) {
        // 图标还没挂上（启动中 / 已删除）：addIcon 时会自己按最新配置解析，这里不用忙。
        return;
    }
    ResolvedIcon resolved;
    if (!resolveIcon(resolved)) {
        return;  // 连宿主图标都拿不到：保留当前图标，别把托盘弄没
    }

    // 来源没变就不碰 Shell：config.applied 可能因为别的插件的配置触发，而且
    // NIM_MODIFY 会让图标闪一下。比较 (来源种类, 文件路径) 四元组是否一致。
    const bool sameSource = (resolved.configPath == iconConfigPath) &&
                            (resolved.hostResource == iconHostResource) &&
                            (resolved.owned == iconOwned);
    if (sameSource && icon != nullptr) {
        if (resolved.owned) {
            DestroyIcon(resolved.handle);  // 同一文件重复加载出来的句柄要释放
        }
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON;
    data.hIcon = resolved.handle;
    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
        shot::Logger(host).warn("[tray] 切换托盘图标失败（Shell_NotifyIconW NIM_MODIFY 返回 FALSE）");
        if (resolved.owned) {
            DestroyIcon(resolved.handle);  // 没切过去：新句柄释放，旧图标继续用
        }
        return;
    }

    adoptIcon(resolved);
    shot::Logger(host).info("[tray] 托盘图标已切换为 " + iconSourceDescription());
}

}  // namespace tray
