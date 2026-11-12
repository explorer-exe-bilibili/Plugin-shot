// window_target_wayland —— Wayland 版的"候选窗口"查询（悬停吸附在 Wayland 上关闭）。
//
// 为什么是空实现，而不是"以后再说"：
//   · Wayland 明确不允许客户端枚举其它应用的顶层窗口（没有 X11 的 QueryTree，
//     也没有 Win32 的 EnumWindows 等价物），所以"悬停到哪个窗口"这件事在 Wayland 上
//     根本拿不到数据；
//   · 覆盖层自己的窗口也没有可传递的原生句柄（region_geometry_wayland.cpp 里
//     nativeWindowHandle() 恒为 0），交互层因此会直接跳过枚举。
//
// 这两个函数只用于满足共享代码的链接需求（region_window.cpp 里仍保留 Win32 版的
// 调用点），运行时不会被真正调用。X11 分支由后续任务补：X11 可以用 XQueryTree +
// _NET_CLIENT_LIST 做同样的枚举，到那时把本文件换成真正的实现即可。

#include "window_target.h"

namespace region_selector {

std::vector<WindowTarget> listWindowTargets(std::uintptr_t excludeHwnd) {
    (void)excludeHwnd;
    // Wayland：没有全局窗口枚举，候选集合恒为空（悬停吸附关闭）。
    return {};
}

bool findWindowAt(int x, int y, std::uintptr_t excludeHwnd, WindowTarget& out) {
    (void)x;
    (void)y;
    (void)excludeHwnd;
    (void)out;
    // 与 listWindowTargets 一致：Wayland 上永远命中不到窗口，单击按"取消"处理。
    return false;
}

}  // namespace region_selector
