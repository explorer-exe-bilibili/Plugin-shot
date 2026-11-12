// 前台窗口查询：客户区物理像素 + 本进程窗口的跳过规则（Win32 平台层）。
//
// 规则（与 include/window_target.h 的契约一致）：
//   1. GetForegroundWindow()；
//   2. 前台是本进程的窗口（托盘隐藏窗、设置页、上一次的 HUD）时，沿真实 Z 序
//      （GetTopWindow + GW_HWNDNEXT）找下一个非本进程、可见、未最小化、客户区非空的窗口；
//   3. 客户区 = GetClientRect() + ClientToScreen()，单位物理像素。
//
// 坐标与 DPI：宿主进程已在 main() 最早阶段声明 PER_MONITOR_AWARE_V2，这里仍对调用
// 线程再兜一次（长截图会话线程不是宿主 main 线程），保证 GetClientRect/ClientToScreen
// 返回物理像素；返回前恢复线程原来的 DPI 上下文，不把副作用留给后面的调用。
//
// 为什么跳过本进程窗口：托盘点击/热键触发时，前台窗口常常是本进程的
// 托盘隐藏窗口或
// 设置页；直接把"自己"当成截图目标会得到一张空白/设置页的长图。沿 Z 序找下一个
// 用户真正在看的窗口，是"从托盘/热键触发"这个交互下唯一符合直觉的选择。

#include "window_target.h"

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace long_capture {
namespace {

// 这些辅助函数只服务本文件的窗口筛选，与任何类无关（STYLE.md §6.7 的合法例外）。

/**
 * @brief 判断窗口是否属于当前进程。
 * @param[in] hwnd 窗口句柄。
 * @return true = 本进程的窗口。
 */
bool isOwnProcessWindow(HWND hwnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    return processId == GetCurrentProcessId();
}

/**
 * @brief 取窗口客户区在屏幕上的矩形（物理像素）。
 * @param[in]  hwnd 窗口句柄。
 * @param[out] out  成功时填好客户区。
 * @return true = 客户区有正面积。
 */
bool clientRectOnScreen(HWND hwnd, ScreenRect& out) {
    RECT client = {};
    if (GetClientRect(hwnd, &client) == FALSE) {
        return false;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return false;
    }
    POINT topLeft = {client.left, client.top};
    POINT bottomRight = {client.right, client.bottom};
    if (ClientToScreen(hwnd, &topLeft) == FALSE || ClientToScreen(hwnd, &bottomRight) == FALSE) {
        return false;
    }
    out.x = topLeft.x;
    out.y = topLeft.y;
    out.width = bottomRight.x - topLeft.x;
    out.height = bottomRight.y - topLeft.y;
    return out.valid();
}

/**
 * @brief 判断一个窗口是否可作为截图目标。
 * @param[in]  hwnd 候选窗口。
 * @param[out] out  成功时填好客户区。
 * @param[out] why  不通过时的中文原因（只对"看起来像目标但状态不对"的窗口填）。
 * @return true = 可用。
 */
bool isUsableTarget(HWND hwnd, ScreenRect& out, std::string& why) {
    if (hwnd == nullptr || IsWindow(hwnd) == FALSE) {
        return false;
    }
    if (IsWindowVisible(hwnd) == FALSE) {
        why = "前台窗口不可见（可能被隐藏或已关闭）";
        return false;
    }
    if (IsIconic(hwnd) != FALSE) {
        why = "前台窗口已最小化，请先恢复窗口再开始长截图";
        return false;
    }
    if (!clientRectOnScreen(hwnd, out)) {
        why = "前台窗口没有可截取的内容区（客户区为空）";
        return false;
    }
    return true;
}

/**
 * @brief 沿 Z 序找第一个非本进程的可用窗口。
 * @param[out] hwnd 找到的窗口。
 * @param[out] rect 客户区。
 * @return true = 找到了。
 */
bool findNextForeignWindow(HWND& hwnd, ScreenRect& rect) {
    // GetTopWindow + GW_HWNDNEXT 走的是真实 Z 序链；EnumWindows 的顺序没有承诺。
    int guard = 0;
    for (HWND current = GetTopWindow(nullptr); current != nullptr && guard < 10000;
         current = GetWindow(current, GW_HWNDNEXT), ++guard) {
        if (isOwnProcessWindow(current)) {
            continue;
        }
        ScreenRect candidate;
        std::string ignored;
        if (isUsableTarget(current, candidate, ignored)) {
            hwnd = current;
            rect = candidate;
            return true;
        }
    }
    return false;
}

/** @brief 对当前线程临时启用 PMv2 DPI 上下文；返回原上下文供恢复。 */
DPI_AWARENESS_CONTEXT enablePerMonitorDpi() {
    // 运行期调用：老系统没有这个入口时返回 nullptr，按原上下文继续
    // （宿主已声明 PMv2）。
    return SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

}  // namespace

bool WindowTarget::findForeground(TargetWindow& out, std::string& error) {
    error.clear();
    const DPI_AWARENESS_CONTEXT previous = enablePerMonitorDpi();

    bool ok = false;
    TargetWindow target;
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        error = "没有前台窗口，请先激活要截图的窗口";
    } else if (isOwnProcessWindow(foreground)) {
        // 触发来自本程序自己的窗口：沿 Z 序找下一个用户真正在看的窗口。
        HWND next = nullptr;
        ScreenRect rect;
        if (findNextForeignWindow(next, rect)) {
            target.handle = reinterpret_cast<std::uintptr_t>(next);
            target.client = rect;
            ok = true;
        } else {
            error = "前台是本程序的窗口，且没有找到其它可截图的窗口；请先激活目标窗口";
        }
    } else {
        ScreenRect rect;
        std::string why;
        if (isUsableTarget(foreground, rect, why)) {
            target.handle = reinterpret_cast<std::uintptr_t>(foreground);
            target.client = rect;
            ok = true;
        } else {
            error = why.empty() ? "前台窗口不可用，请先激活要截图的窗口" : why;
        }
    }

    if (previous != nullptr) {
        SetThreadDpiAwarenessContext(previous);
    }
    if (ok) {
        out = target;
    }
    return ok;
}

void WindowTarget::virtualDesktopOrigin(int& originX, int& originY) {
    originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
}

}  // namespace long_capture
