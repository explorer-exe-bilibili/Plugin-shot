// window_target_wayland —— Wayland 版的"窗口扫描"（悬停吸附在 Wayland 上关闭）。
//
// 为什么是空实现，而不是"以后再说"：
//   · Wayland 明确不允许客户端枚举其它应用的顶层窗口（没有 X11 的 QueryTree，
//     也没有 Win32 的 EnumWindows 等价物），所以"悬停到哪个窗口"这件事在 Wayland 上
//     根本拿不到数据；任务栏/面板同理（Wayland 里就是 shell 的 surface，不可见）。
//   · 覆盖层自己的窗口也没有可传递的原生句柄（region_geometry_wayland.cpp 里
//     nativeWindowHandle() 恒为 0），交互层因此会直接跳过枚举。
//
// 这个函数只用于满足共享代码（region_overlay.cpp）的链接需求，运行时不会被真正调用。
// X11 与 Windows 的实现分别在 window_target_x11.cpp / window_target.cpp。

#include "window_target.h"

namespace region_selector {

WindowScan scanWindowTargets(std::uintptr_t excludeHwnd) {
    (void)excludeHwnd;
    // Wayland：没有全局窗口枚举，候选窗口与面板都恒为空（悬停吸附关闭）。
    return {};
}

}  // namespace region_selector
