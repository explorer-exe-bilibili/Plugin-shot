// region_overlay —— 框选交互状态机 + 悬停窗口吸附与指数平滑动画。
//
// 交互全部在 UI 线程上轮询（glfwGetMouseButton / glfwGetKey），不接 GLFW 回调：
// 这里只需要"当前按没按下"，轮询比回调 + 状态标志更好读，也不会漏掉回调顺序。
//
// 状态机（每次覆盖层显示都是一次全新的会话）：
//   显示覆盖层        → 定位光标；若左键还按着，先等它松开（waitRelease）
//   空闲（无按键）    → 每帧判定悬停目标（屏幕四角 / 任务栏面板 / 顶层窗口），
//                       选框用指数平滑吸附过去
//   左键按下          → 记下起点与时刻（pressed）；此时还可能是单击
//   位移 > 4px        → 进入拖动模式（dragging）：选框严格跟手，**禁用**吸附与动画
//   松开（单击）      → 位移 ≤4px 且按住 ≤400ms：直接用悬停窗口矩形确认采集
//   松开（拖动）      → 用跟手矩形确认；任一边 < 8 物理像素按取消处理
//   Esc / 右键        → 取消：只隐藏窗口，不发布任何事件（v1 不定义 capture.cancelled）
//
// 为什么拖动时禁用动画/吸附（需求硬约束）：
//   拖动时用户期待选框边缘和鼠标之间没有延迟；平滑动画会让选框"追"鼠标，选出来的
//   区域和松手位置对不上。悬停吸附还会在拖过窗口边界时突然跳变，所以两者都关掉。
//
// 绘制全部在 region_overlay_draw.cpp：本文件只维护"当前应该画哪个矩形"的状态。
//
// 坐标：drag 里存的是窗口内容坐标（物理像素）。绘制用 framebuffer 坐标，两者在
// Windows PMv2 与 X11 下相等（实测 2560x1600 == 2560x1600）；绘制侧仍按 ActualScale
// 换算一次，万一将来窗口被系统缩放也不会画歪。content scale 只放大字号与折角/边框
// 这类装饰，绝不乘进区域坐标。
//
// 悬停判定（resolveHoverHit）的优先级：屏幕四角 → 任务栏/面板窗口 → 面板占位带 →
// 普通窗口。前两者都映射成"当前显示器全屏"这个伪窗口矩形（用户要求：悬停在任务栏或
// 屏幕四角时选中整屏）；面板占位带是 X11 专有的一层——GNOME 的 shell 顶栏/dock 不是
// DOCK 客户端窗口，枚举不到，只能用 `_NET_WORKAREA` 推出的四边厚度命中（panelInsets，
// 见 region_geometry_x11.cpp）。三者绘制与确认路径完全复用窗口吸附那一套，不新增事件、
// 也不改 ABI。
//
// 悬停枚举按平台层分派：Windows 走 EnumWindows + DWM 边界；X11 走 XQueryTree +
// EWMH 类型过滤；Wayland 恒为空结果（协议不允许枚举，吸附关闭）。交互代码本身与
// 平台无关，两种新会话共用。X11 的鼠标/键盘同样用 GLFW 轮询（覆盖层拿焦点）。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

#include <GLFW/glfw3.h>

namespace region_selector {
namespace {

/** @brief 主题色默认值：配置项 `core/theme_color` 缺失 / 非法时用它。 */
constexpr char kDefaultThemeColorHex[] = "#4C9AFF";
constexpr std::uint32_t kDefaultThemeRgb = 0x4C9AFFu;

/**
 * @brief 解析 `#RRGGBB`（`#` 可省略）成 0xRRGGBB。
 *
 * 只接受 6 位十六进制：宿主配置项约定就是这个格式，多支持 #RGB/#AARRGGBB 只会让
 * "非法时用默认色"的边界变得含糊。解析失败返回 false，由调用方回退默认色。
 */
bool parseThemeColor(const std::string& text, std::uint32_t& out) {
    std::size_t index = 0;
    if (!text.empty() && text[0] == '#') {
        index = 1;
    }
    if (text.size() - index != 6) {
        return false;
    }
    std::uint32_t value = 0;
    for (; index < text.size(); ++index) {
        const char ch = text[index];
        int digit = -1;
        if (ch >= '0' && ch <= '9') {
            digit = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            digit = ch - 'a' + 10;
        } else if (ch >= 'A' && ch <= 'F') {
            digit = ch - 'A' + 10;
        } else {
            return false;
        }
        value = value * 16u + static_cast<std::uint32_t>(digit);
    }
    out = value;
    return true;
}

/** @brief 把浮点值按指数平滑逼近目标；足够近时直接吸附，避免永远差零点几像素。 */
void approachValue(float& value, float target, float alpha) {
    const float delta = target - value;
    if (std::abs(delta) <= 0.5f) {
        value = target;
        return;
    }
    value += delta * alpha;
}

/** @brief 点是否落在矩形内（左闭右开，与命中测试一致；宽高为 0 的矩形永不命中）。 */
bool rectContains(const ScreenRect& rect, int x, int y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

/** @brief 点是否落在窗口/面板矩形内（同一套左闭右开语义）。 */
bool rectContains(const WindowTarget& target, int x, int y) {
    return x >= target.x && x < target.x + target.width && y >= target.y &&
           y < target.y + target.height;
}

/**
 * @brief 光标是否落在当前显示器四角的方块里（距两条相邻边都 < kCornerSnapSize）。
 *
 * 用**显示器矩形**而不是虚拟桌面/整个 X screen 矩形：多显示器时只有光标所在那块屏
 * 的四角才算"屏幕四角"（用户要求）。右/下边用"离最后一行/列多远"来算，否则右下角
 * 的方块会变成 21 像素宽（差 1 的经典 off-by-one）。
 */
bool isInMonitorCorner(const ScreenRect& monitor, int screenX, int screenY) {
    if (monitor.width <= 0 || monitor.height <= 0) {
        return false;
    }
    const int left = screenX - monitor.x;
    const int top = screenY - monitor.y;
    const int right = monitor.x + monitor.width - 1 - screenX;
    const int bottom = monitor.y + monitor.height - 1 - screenY;
    if (left < 0 || top < 0 || right < 0 || bottom < 0) {
        return false;  // 光标不在这块显示器上
    }
    const bool nearVerticalEdge = (left < kCornerSnapSize) || (right < kCornerSnapSize);
    const bool nearHorizontalEdge = (top < kCornerSnapSize) || (bottom < kCornerSnapSize);
    return nearVerticalEdge && nearHorizontalEdge;
}

/**
 * @brief 光标是否落在当前显示器四条边的"面板占位带"里（工作区之外的区域）。
 *
 * 面板占位来自 X11 的 `_NET_WORKAREA`（见 `RegionWindow::Impl::x11ReadPanelInsets`）：
 * GNOME 的顶栏 / dash-to-dock 是 shell 自己画的，不在 XQueryTree 的窗口列表里，按窗口
 * 矩形永远命中不到；工作区与显示器矩形相减得到的四条厚度就是它们实际占的区域。
 * inset 全 0（其它平台 / 读不到 / 没有面板）时恒返回 false，不影响原有命中。
 *
 * 语义与"任务栏/面板窗口"一致：命中后吸附到**当前显示器全屏**，而不是那条带本身。
 *
 * @param[in] monitor 当前覆盖的显示器矩形（绝对屏幕坐标）。
 * @param[in] insets  这块显示器四条边的面板厚度（物理像素，均由 x11ReadPanelInsets 校验）。
 * @param[in] screenX 光标的绝对屏幕横坐标。
 * @param[in] screenY 光标的绝对屏幕纵坐标。
 * @return true = 光标落在上/下/左/右任一条带内，且位于这块显示器上。
 */
bool isInPanelInsetBand(const ScreenRect& monitor, const PanelInsets& insets, int screenX,
                        int screenY) {
    if (monitor.width <= 0 || monitor.height <= 0) {
        return false;
    }
    if (insets.top <= 0 && insets.left <= 0 && insets.right <= 0 && insets.bottom <= 0) {
        return false;  // 没有可识别的面板占位（其它平台 / 读不到）：保持原有命中行为
    }
    if (!rectContains(monitor, screenX, screenY)) {
        return false;  // inset 是相对这块显示器算的，不要拿去命中另一块屏
    }
    const int localX = screenX - monitor.x;
    const int localY = screenY - monitor.y;
    // 上/左边用"距起点多远"，下/右边用"到终点还有多远"（与四角判定同一套左闭右开语义）。
    return localY < insets.top || localY >= monitor.height - insets.bottom ||
           localX < insets.left || localX >= monitor.width - insets.right;
}

}  // namespace

void RegionWindow::Impl::normalizeRect(int ax, int ay, int bx, int by, int& x, int& y,
                                       int& width, int& height) {
    x = std::min(ax, bx);
    y = std::min(ay, by);
    width = std::abs(bx - ax);
    height = std::abs(by - ay);
}

// ---------------------------------------------------------------------------
// 主题色：每次显示覆盖层读一次（配置变化不需要实时刷新）
// ---------------------------------------------------------------------------
void RegionWindow::Impl::loadThemeColor() {
    shot::Config config(host);
    const std::string text = config.getString("core/theme_color", kDefaultThemeColorHex);
    std::uint32_t rgb = kDefaultThemeRgb;
    if (!parseThemeColor(text, rgb)) {
        // 读不到（老宿主没有配置服务）或格式非法都走这里：用需求给定的默认亮蓝色，
        // 保证覆盖层永远有可见的颜色，而不是画成黑色。
        rgb = kDefaultThemeRgb;
        if (!text.empty() && text != kDefaultThemeColorHex) {
            shot::Logger(host).debug(
                "region_selector：core/theme_color 不是 #RRGGBB，使用默认 #4C9AFF");
        }
    }
    themeColorRgb = rgb;
}

// ---------------------------------------------------------------------------
// 悬停目标判定：屏幕四角 / 任务栏面板 / 普通窗口（列表顺序即优先级）
// ---------------------------------------------------------------------------
HoverHit RegionWindow::Impl::resolveHoverHit(int screenX, int screenY) const {
    HoverHit hit;
    // 没有显示器矩形（还没开始框选）时退化成纯窗口命中：绝不拿一块 0x0 的"全屏"去吸附。
    const bool hasMonitor = monitorRect.width > 0 && monitorRect.height > 0;

    // 1) 屏幕四角：优先级最高。角落方块常常整个压在最大化窗口（甚至任务栏）上，
    //    用户要的是"这块屏"，而不是被压住的那个窗口（用户要求）。
    if (hasMonitor && isInMonitorCorner(monitorRect, screenX, screenY)) {
        hit.kind = HoverTargetKind::Corner;
        hit.rect = monitorRect;
        return hit;
    }

    // 2) 任务栏 / 面板：矩形一律用 monitorRect（鼠标所在显示器），不是面板自己的矩形。
    //    副屏任务栏命中时也要选"鼠标所在那块屏的全屏"（用户要求）。面板在 Z 序最上层，
    //    命中它就不该再选中它下面的窗口，所以这里直接返回。
    if (hasMonitor) {
        for (const WindowTarget& panel : windowScan.panels) {
            if (rectContains(panel, screenX, screenY)) {
                hit.kind = HoverTargetKind::Panel;
                hit.hwnd = panel.hwnd;
                hit.rect = monitorRect;
                return hit;
            }
        }
    }

    // 3) 面板占位带（X11）：GNOME 的 shell 顶栏 / dash-to-dock 不是 DOCK 客户端窗口，
    //    上面的 panels 列表里没有它们，只能靠 `_NET_WORKAREA` 推出的四边厚度命中。
    //    排在四角之后、普通窗口之前：面板带全在屏幕边缘，那里通常压着最大化窗口，
    //    用户要的是"这块屏"；屏幕中间的面板/窗口仍按普通窗口命中。inset 全 0
    //    （其它平台 / 读不到）时这个分支恒不命中。
    if (hasMonitor && isInPanelInsetBand(monitorRect, panelInsets, screenX, screenY)) {
        hit.kind = HoverTargetKind::PanelArea;
        hit.rect = monitorRect;
        return hit;
    }

    // 4) 普通窗口：列表已按 Z 序排好，第一个包含点的就是最上层窗口。
    for (const WindowTarget& target : windowScan.windows) {
        if (!rectContains(target, screenX, screenY)) {
            continue;
        }
        hit.kind = HoverTargetKind::Window;
        hit.hwnd = target.hwnd;
        hit.rect.x = target.x;
        hit.rect.y = target.y;
        hit.rect.width = target.width;
        hit.rect.height = target.height;
        return hit;
    }
    return hit;
}

// ---------------------------------------------------------------------------
// 悬停窗口：节流枚举 + 命中 + 指数平滑
// ---------------------------------------------------------------------------
void RegionWindow::Impl::updateHover() {
    if (window == nullptr || overlayHwnd == 0) {
        // 拿不到自己的原生句柄时宁可关掉吸附：否则枚举结果里可能包含覆盖层自己，
        // 选框会永远吸附在一块全屏矩形上。拖动框选不受影响。
        // Wayland 分支恒为这种情况（nativeWindowHandle() 返回 0，且协议不允许全局
        // 窗口枚举）：悬停吸附关闭，单击不拖 = 取消。X11 下句柄非 0，走 XQueryTree。
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    // 最多每 30ms 枚举一次：覆盖层每帧 120Hz，逐帧枚举顶层窗口（Windows 是
    // EnumWindows + DWM 查询，X11 是 XQueryTree + X 属性查询）会白白吃掉 UI 线程。
    // 30ms 的 hover 延迟用户感觉不出来。
    if (!windowScanValid ||
        now - lastWindowScan >= std::chrono::milliseconds(kWindowScanIntervalMs)) {
#if defined(__linux__)
        // Linux 两套实现都编进来了：运行期按会话类型调用对应的窗口扫描。
        if (linuxBackend == LinuxSessionBackend::X11) {
            windowScan = scanWindowTargetsX11(overlayHwnd);
        } else {
            // Wayland：空实现（协议不允许枚举），返回空结果。
            windowScan = scanWindowTargets(overlayHwnd);
        }
#else
        windowScan = scanWindowTargets(overlayHwnd);
#endif
        lastWindowScan = now;
        windowScanValid = true;
    }

    // 光标屏幕坐标 = 窗口位置 + 窗口内坐标。用未夹取的原始值：鼠标暂时移出窗口
    // 边缘时，夹取后的坐标会命中错误的窗口。
    int windowX = 0;
    int windowY = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    double cursorX = 0.0;
    double cursorY = 0.0;
    glfwGetCursorPos(window, &cursorX, &cursorY);
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0 || cursorX < 0.0 || cursorY < 0.0 ||
        cursorX >= static_cast<double>(windowWidth) ||
        cursorY >= static_cast<double>(windowHeight)) {
        // 光标已移出本屏（覆盖层只铺当前显示器）：不要吸附到另一块屏上的窗口——
        // 选框画不出来，用户会盲点。跨屏选择是 README §7 的已知限制。
        if (hover.active) {
            shot::Logger(host).debug("region_selector：光标移出覆盖层所在显示器，隐藏悬停框");
        }
        hover.active = false;
        hover.initialized = false;
        hover.hwnd = 0;
        hover.kind = HoverTargetKind::NoTarget;
        return;
    }
    const int screenX = windowX + static_cast<int>(cursorX);
    const int screenY = windowY + static_cast<int>(cursorY);

    const HoverHit hit = resolveHoverHit(screenX, screenY);
    if (hit.kind == HoverTargetKind::NoTarget) {
        if (hover.active) {
            shot::Logger(host).debug(
                "region_selector：光标下没有可吸附目标（窗口 / 任务栏面板 / 面板占位区域 / "
                "屏幕四角），隐藏悬停框");
        }
        // 下次从"无框"变"有框"时直接落位，不要从上一个目标的旧位置飞进来。
        hover.active = false;
        hover.initialized = false;
        hover.hwnd = 0;
        hover.kind = HoverTargetKind::NoTarget;
        return;
    }

    ScreenRect local;
    local.x = hit.rect.x - windowX;
    local.y = hit.rect.y - windowY;
    local.width = hit.rect.width;
    local.height = hit.rect.height;

    const bool changed = !hover.active || hover.hwnd != hit.hwnd || hover.kind != hit.kind ||
                         hover.target.x != local.x || hover.target.y != local.y ||
                         hover.target.width != local.width ||
                         hover.target.height != local.height;
    if (!hover.active) {
        // 无框 → 有框：直接等于目标，不从旧位置飞入（与"显示首帧不飞入"同一原则）。
        hover.x = static_cast<float>(local.x);
        hover.y = static_cast<float>(local.y);
        hover.width = static_cast<float>(local.width);
        hover.height = static_cast<float>(local.height);
        hover.initialized = true;
        hover.lastTick = now;
    } else if (changed) {
        // 目标变了：从这一帧开始计时，避免把上一帧到现在的 dt 也算进新目标。
        hover.lastTick = now;
    }
    if (changed) {
        char text[224] = {};
        if (hit.kind == HoverTargetKind::Window) {
            std::snprintf(text, sizeof(text),
                          "region_selector：悬停窗口 hwnd=0x%llx rect=(%d,%d) %dx%d"
                          "（已换算到覆盖层坐标）",
                          static_cast<unsigned long long>(hit.hwnd), local.x, local.y,
                          local.width, local.height);
        } else {
            // "全屏"伪目标没有真实窗口，日志里写清是哪种命中，排查时不用猜。
            const char* source = "任务栏/面板";
            if (hit.kind == HoverTargetKind::Corner) {
                source = "屏幕四角";
            } else if (hit.kind == HoverTargetKind::PanelArea) {
                source = "面板区域（X11 _NET_WORKAREA 占位）";
            }
            std::snprintf(text, sizeof(text),
                          "region_selector：悬停%s → 当前显示器全屏 rect=(%d,%d) %dx%d"
                          "（已换算到覆盖层坐标）",
                          source, local.x, local.y, local.width, local.height);
        }
        shot::Logger(host).debug(text);
    }
    hover.active = true;
    hover.hwnd = hit.hwnd;
    hover.kind = hit.kind;
    hover.target = local;
    animateHover();
}

void RegionWindow::Impl::animateHover() {
    if (!hover.active || !hover.initialized) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - hover.lastTick).count();
    hover.lastTick = now;
    if (dt <= 0.0) {
        return;
    }
    // 卡顿/断点恢复时不让一步跳过大半个屏幕：夹到 250ms，最多一次走完约 92%。
    dt = std::min(dt, 0.25);
    // 指数平滑：alpha = 1 - exp(-dt/tau)；tau=100ms 时每帧约完成 8%（120Hz）。
    const float alpha =
        static_cast<float>(1.0 - std::exp(-dt / kHoverSmoothTimeConstantSeconds));
    approachValue(hover.x, static_cast<float>(hover.target.x), alpha);
    approachValue(hover.y, static_cast<float>(hover.target.y), alpha);
    approachValue(hover.width, static_cast<float>(hover.target.width), alpha);
    approachValue(hover.height, static_cast<float>(hover.target.height), alpha);
}

// ---------------------------------------------------------------------------
// 交互状态机
// ---------------------------------------------------------------------------
void RegionWindow::Impl::updateSelection() {
    if (window == nullptr || !imguiInitialized) {
        return;
    }
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0) {
        return;
    }

    // 光标位置：窗口内容坐标（物理像素）。拖到窗口外时夹到边上，按住不放也能贴边。
    double cursorX = 0.0;
    double cursorY = 0.0;
    glfwGetCursorPos(window, &cursorX, &cursorY);
    drag.cursorX = std::clamp(static_cast<int>(cursorX), 0, windowWidth - 1);
    drag.cursorY = std::clamp(static_cast<int>(cursorY), 0, windowHeight - 1);

    // Esc / 右键：取消。只隐藏，不发布事件（v1 不定义 capture.cancelled）。
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS ||
        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
        cancelSelection();
        return;
    }

    const bool leftDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const auto now = std::chrono::steady_clock::now();

    if (drag.waitRelease) {
        // 覆盖层显示时左键还按着（例如从托盘菜单触发、菜单收起前事件已经发出）：
        // 先等它松开，否则这次按下会被当成单击/拖拽起点。
        if (!leftDown) {
            drag.waitRelease = false;
        }
        updateHover();  // 等松手期间仍然显示吸附框，用户能看到即将截哪块
        return;
    }

    if (drag.dragging) {
        // 拖动中**不枚举窗口、不做动画**：需求要求选框严格跟手。这里提前返回，
        // 连 windowScan 都不刷新，避免拖拽时每 30ms 偷偷枚举一次。
        if (leftDown) {
            return;  // drag.cursor 每帧已刷新，绘制直接用
        }
        // 左键松开 = 确认。尺寸检查与"隐藏 → 等待 → 发布"的时序在 finishSelection 里，
        // 因为那是窗口生命周期/提交关注点，不属于交互绘制文件。
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        normalizeRect(drag.startX, drag.startY, drag.cursorX, drag.cursorY, x, y, width, height);
        finishSelection(x, y, width, height, false);
        return;
    }

    if (drag.pressed) {
        if (leftDown) {
            // 还没越过阈值：仍然算"悬停"，选框可以平滑到新窗口（用户可能只想单击）。
            updateHover();
            const int moveX = std::abs(drag.cursorX - drag.startX);
            const int moveY = std::abs(drag.cursorY - drag.startY);
            if (moveX > kClickMaxMovePixels || moveY > kClickMaxMovePixels) {
                drag.dragging = true;
                shot::Logger(host).debug(
                    "region_selector：位移超过 4px，进入拖动模式（禁用窗口吸附与动画）");
            }
            return;
        }

        // 松开：位移 ≤4px 且按住 ≤400ms 才是"单击截窗口"；否则按取消处理。
        const int moveX = std::abs(drag.cursorX - drag.startX);
        const int moveY = std::abs(drag.cursorY - drag.startY);
        const auto heldMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - drag.pressTime).count();
        const bool isClick = moveX <= kClickMaxMovePixels &&
                             moveY <= kClickMaxMovePixels && heldMs <= kClickMaxHoldMs;
        drag.pressed = false;
        if (isClick && hover.active) {
            // 用 hover.target（真实窗口矩形），不要用 hover.current（动画中的中间值）。
            finishSelection(hover.target.x, hover.target.y, hover.target.width,
                            hover.target.height, true);
        } else {
            // 按住超时或点在空白处：与旧版 0x0 选区一致，按取消处理，不发布事件。
            cancelSelection();
        }
        return;
    }

    // 空闲：先刷新悬停框（这是唯一会触发枚举/动画的路径），再处理本次按下。
    updateHover();
    if (leftDown) {
        drag.pressed = true;
        drag.startX = drag.cursorX;
        drag.startY = drag.cursorY;
        drag.pressTime = now;
    }
}

}  // namespace region_selector
