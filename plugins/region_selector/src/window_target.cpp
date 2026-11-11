// window_target —— 顶层窗口枚举、Z 序、DWM 扩展帧边界与命中测试。
//
// 为什么不用 Win32 的逐点窗口命中 API（关键，不要"优化"回去）：
//   覆盖层是一个盖满整块显示器、带 GLFW_FLOATING 的置顶窗口，屏幕上的每个像素都
//   属于它自己。按坐标找窗口的 API 只会返回覆盖层窗口，永远看不到下面的真实窗口。
//   唯一可行的做法是自己 EnumWindows 枚举顶层窗口，跳过不该被选中的窗口，再按
//   Z 序做矩形命中测试。
//
// 为什么用 DWMWA_EXTENDED_FRAME_BOUNDS：
//   Win10/11 的普通窗口在 DWM 里有一圈肉眼不可见的调整边框（阴影/拖拽热区），
//   GetWindowRect 会把这圈算进去。直接拿它当截图区域，图四周会多出几像素的桌面
//   或相邻窗口。DWM 的扩展帧边界才是用户实际看到的窗口轮廓。DWM 调用失败
//   （合成被关闭、属性不支持等）时退回 GetWindowRect，保证还能用。
//
// 为什么坐标是物理像素：
//   宿主 main() 在任何窗口/采集之前把进程声明成 PER_MONITOR_AWARE_V2，所以
//   GetCursorPos / GetWindowRect / DwmGetWindowAttribute 返回的都是物理像素，
//   与 screen_capture 的 describe、region 的坐标系一致。这里不做任何 DPI 换算。
//
// 线程：纯查询，不依赖窗口线程；但当前只由覆盖层 UI 线程调用，内部没有共享状态。

#include "window_target.h"

#include <algorithm>
#include <climits>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dwmapi.h>

namespace region_selector {
namespace {

/** @brief Z 序链的最大步数：防止异常情况下 GetWindow 链成环把渲染线程卡死。 */
constexpr int kMaxZOrderWindows = 10000;

/**
 * @brief 窗口是否被 DWM 标记为 cloaked（不可见）。
 *
 * Win10 的 UWP 挂起窗口、其它虚拟桌面的窗口 IsWindowVisible 仍为 TRUE，但用户
 * 根本看不到；不排除的话它们会盖住真实目标、命中一块"幽灵"矩形。
 * DWM 查询失败按"没被 cloaked"处理：宁可多列一个窗口，也不要漏掉真实窗口。
 */
bool isDwmCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    const HRESULT result =
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, static_cast<DWORD>(sizeof(cloaked)));
    return SUCCEEDED(result) && cloaked != 0;
}

/**
 * @brief 取窗口在屏幕上的可见矩形（物理像素）。
 *
 * 优先 DWM 扩展帧边界（去掉不可见边框）；失败退回 GetWindowRect。
 */
bool frameBounds(HWND hwnd, RECT& out) {
    RECT extended = {};
    const HRESULT result = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &extended,
                                                 static_cast<DWORD>(sizeof(extended)));
    if (SUCCEEDED(result)) {
        out = extended;
        return true;
    }
    return GetWindowRect(hwnd, &out) != FALSE;
}

/**
 * @brief 把矩形裁剪到虚拟桌面内；裁剪后为空返回 false。
 *
 * 横跨多屏 / 部分移出屏幕的窗口不能整块返回：region 交给流水线后要按虚拟桌面
 * 裁剪，边界外的部分本来就不存在，提前裁掉能让绘制和命中保持一致。
 */
bool clipToVirtualDesktop(RECT& rect) {
    const RECT desktop = {
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
    RECT clipped = {};
    if (IntersectRect(&clipped, &rect, &desktop) == FALSE) {
        return false;
    }
    rect = clipped;
    return true;
}

/**
 * @brief 判断一个 HWND 是否可以作为框选目标。
 *
 * 排除清单（每一条的原因见对应的行内注释）：
 *   · 覆盖层自己：它是全屏置顶窗口，不排除会永远命中自己；
 *   · 不可见 / 最小化 / 零尺寸：用户看不到，选中它没有意义；
 *   · DWM cloaked：UWP 挂起窗口、其它虚拟桌面窗口（IsWindowVisible 仍为真）；
 *   · WS_EX_TOOLWINDOW：工具窗口、浮动提示、输入法候选框，不是用户想截的窗口。
 */
bool isCandidateWindow(HWND hwnd, std::uintptr_t excludeHwnd, RECT& rect) {
    if (hwnd == reinterpret_cast<HWND>(excludeHwnd)) {
        // 覆盖层自己（由调用方传入 HWND）。必须排除，否则命中测试永远选中自己。
        return false;
    }
    // 隐藏窗口没有可见内容；最小化窗口的 GetWindowRect 是恢复前的离谱坐标。
    if (IsWindowVisible(hwnd) == FALSE || IsIconic(hwnd) != FALSE) {
        return false;
    }
    // 工具窗口 / 提示 / 输入法候选：不在正常 Alt-Tab 列表里，用户也不会想截它们。
    const LONG_PTR extendedStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }
    if (isDwmCloaked(hwnd)) {
        return false;
    }
    if (!frameBounds(hwnd, rect)) {
        return false;
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return false;  // 零尺寸（含某些刚创建/正在销毁的窗口）
    }
    return clipToVirtualDesktop(rect);
}

/** @brief EnumWindows 回调的上下文：排除句柄 + 收集结果。 */
struct EnumContext {
    std::uintptr_t excludeHwnd = 0;
    std::vector<WindowTarget> targets;
};

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<EnumContext*>(param);
    if (context == nullptr) {
        return FALSE;
    }
    RECT rect = {};
    if (!isCandidateWindow(hwnd, context->excludeHwnd, rect)) {
        return TRUE;  // 继续枚举
    }
    WindowTarget target;
    target.hwnd = reinterpret_cast<std::uintptr_t>(hwnd);
    target.x = rect.left;
    target.y = rect.top;
    target.width = rect.right - rect.left;
    target.height = rect.bottom - rect.top;
    context->targets.push_back(target);
    return TRUE;
}

/**
 * @brief 建立"HWND → Z 序下标（0 = 最上层）"表。
 *
 * 不要假设 EnumWindows 的枚举顺序就是 Z 序：MSDN 没有承诺，不同 Windows 版本
 * 可能不同。GetTopWindow + GW_HWNDNEXT 走的是真正的 Z 序链，这里用它给候选窗口
 * 排一个确定性的顺序，命中测试才能稳定地"取最上面那个"。
 */
std::unordered_map<HWND, int> buildZOrder() {
    std::unordered_map<HWND, int> rank;
    int index = 0;
    for (HWND hwnd = GetTopWindow(nullptr); hwnd != nullptr && index < kMaxZOrderWindows;
         hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        rank.emplace(hwnd, index);
        ++index;
    }
    return rank;
}

}  // namespace

std::vector<WindowTarget> listWindowTargets(std::uintptr_t excludeHwnd) {
    EnumContext context;
    context.excludeHwnd = excludeHwnd;
    EnumWindows(&collectWindow, reinterpret_cast<LPARAM>(&context));
    if (context.targets.size() <= 1) {
        return context.targets;
    }

    const std::unordered_map<HWND, int> rank = buildZOrder();
    std::stable_sort(context.targets.begin(), context.targets.end(),
                     [&rank](const WindowTarget& left, const WindowTarget& right) {
                         const auto leftRank =
                             rank.find(reinterpret_cast<HWND>(left.hwnd));
                         const auto rightRank =
                             rank.find(reinterpret_cast<HWND>(right.hwnd));
                         const int leftIndex =
                             (leftRank != rank.end()) ? leftRank->second : INT_MAX;
                         const int rightIndex =
                             (rightRank != rank.end()) ? rightRank->second : INT_MAX;
                         return leftIndex < rightIndex;
                     });
    return context.targets;
}

bool findWindowAt(int x, int y, std::uintptr_t excludeHwnd, WindowTarget& out) {
    const std::vector<WindowTarget> targets = listWindowTargets(excludeHwnd);
    for (const WindowTarget& target : targets) {
        if (x >= target.x && x < target.x + target.width && y >= target.y &&
            y < target.y + target.height) {
            out = target;
            return true;  // 列表已按 Z 序排好，第一个包含点的就是最上层窗口
        }
    }
    return false;
}

}  // namespace region_selector
