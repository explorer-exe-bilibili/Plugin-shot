// region_overlay —— 框选交互状态机 + ImGui 遮罩/十字线/选区绘制。
//
// 交互全部在 UI 线程上轮询（glfwGetMouseButton / glfwGetKey），不接 GLFW 回调：
// 这里只需要"当前按没按下"，轮询比回调 + 状态标志更好读，也不会漏掉回调顺序。
//
// 状态机（每次覆盖层显示都是一次全新的会话）：
//   显示覆盖层        → 记录光标位置；若左键还按着，先等它松开（waitRelease）
//   左键按下          → dragging = true，记下起点
//   移动              → 每帧刷新 drag.cursor，绘制用
//   左键松开          → 确认：任一边 < 8 物理像素按取消处理；否则交给
//                       region_window.cpp 的 finishSelection（隐藏 → 等待 → 发布）
//   Esc / 右键        → 取消：只隐藏窗口，不发布任何事件（v1 不定义 capture.cancelled）
//
// 绘制全部用 ImGui 的 background draw list，不创建 ImGui 窗口：覆盖层没有布局、
// 没有控件，只是把桌面快照当背景铺满、把选区外画暗、把选区挖空（露出背景快照）、
// 画十字线和一个尺寸标签。
//
// 坐标：drag 里存的是窗口内容坐标（物理像素）。绘制用 framebuffer 坐标，两者在
// Windows PMv2 下相等（实测 2560x1600 == 2560x1600）；这里仍按 ActualScale 换算一次，
// 万一将来窗口被系统缩放也不会画歪。

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include "region_window_impl.h"

#include <GLFW/glfw3.h>

#include "imgui.h"

namespace region_selector {
namespace {

/** @brief 遮罩：纯黑约 51% 不透明度，压暗但不至于看不清下面的内容。 */
constexpr ImU32 kDimColor = IM_COL32(0, 0, 0, 130);
/** @brief 选区边框：亮蓝色，和深色遮罩对比度足够。 */
constexpr ImU32 kSelectionBorderColor = IM_COL32(90, 200, 250, 255);
/** @brief 十字线：半透明白，不抢选区边框的视觉焦点。 */
constexpr ImU32 kCrosshairColor = IM_COL32(255, 255, 255, 110);
/** @brief 提示文字与它的底色。 */
constexpr ImU32 kHintTextColor = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kHintBackColor = IM_COL32(0, 0, 0, 170);
/** @brief 选区边框粗细（像素）。 */
constexpr float kBorderThickness = 2.0f;
/** @brief 尺寸标签与选区之间的间距（像素）。 */
constexpr float kLabelGap = 8.0f;

/** @brief 让矩形从任意两个对角点生成（左上 + 宽高，宽高非负）。 */
void normalizeRect(int ax, int ay, int bx, int by, int& x, int& y, int& width, int& height) {
    x = std::min(ax, bx);
    y = std::min(ay, by);
    width = std::abs(bx - ax);
    height = std::abs(by - ay);
}

}  // namespace

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
    if (drag.waitRelease) {
        // 覆盖层显示时左键还按着（例如从托盘菜单触发、菜单收起前事件已经发出）：
        // 先等它松开，否则这次按下会被当成拖拽起点，松手立刻确认出一个 0x0 选区。
        if (!leftDown) {
            drag.waitRelease = false;
        }
        return;
    }
    if (!drag.dragging) {
        if (leftDown) {
            drag.dragging = true;
            drag.startX = drag.cursorX;
            drag.startY = drag.cursorY;
        }
        return;
    }
    if (leftDown) {
        return;  // 还在拖：drag.cursor 每帧已刷新，绘制直接用
    }

    // 左键松开 = 确认。尺寸检查与"隐藏 → 等待 → 发布"的时序在 finishSelection 里，
    // 因为那是窗口生命周期/提交关注点，不属于交互绘制文件。
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    normalizeRect(drag.startX, drag.startY, drag.cursorX, drag.cursorY, x, y, width, height);
    finishSelection(x, y, width, height);
}

void RegionWindow::Impl::drawOverlay() const {
    if (window == nullptr || !imguiInitialized) {
        return;
    }
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return;
    }
    const ImVec2 origin = viewport->Pos;
    const ImVec2 size = viewport->Size;
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }
    const ImVec2 bottomRight(origin.x + size.x, origin.y + size.y);

    // 窗口坐标 → ImGui 绘制坐标的换算系数。PMv2 下窗口物理像素 == framebuffer 像素，
    // 系数恒为 1；保留换算是为了不把"坐标空间相同"当成硬编码假设写死。
    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    const float scaleX = (windowWidth > 0 && framebufferWidth > 0)
                             ? static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth)
                             : 1.0f;
    const float scaleY = (windowHeight > 0 && framebufferHeight > 0)
                             ? static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight)
                             : 1.0f;
    const auto toOverlay = [&](int localX, int localY) {
        return ImVec2(origin.x + static_cast<float>(localX) * scaleX,
                      origin.y + static_cast<float>(localY) * scaleY);
    };

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (draw == nullptr) {
        return;
    }
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();

    // 0. 背景：触发瞬间的桌面快照。窗口是不透明的，这张图就是用户看到的"桌面"，
    //    遮罩与挖空都画在它上面（等价于透明覆盖层，但不依赖 DWM 合成）。
    if (backgroundTexture != 0) {
        draw->AddImage(static_cast<ImTextureID>(backgroundTexture), origin, bottomRight);
    }

    // 1. 暗色遮罩：拖拽中只压暗"选区之外"的四块，选区本身保持完全透明——
    //    窗口是透明帧缓冲，挖空的地方能直接看到真实桌面，所见即所截。
    bool hasSelection = false;
    ImVec2 selectionMin;
    ImVec2 selectionMax;
    int localWidth = 0;
    int localHeight = 0;
    if (drag.dragging) {
        int localX = 0;
        int localY = 0;
        normalizeRect(drag.startX, drag.startY, drag.cursorX, drag.cursorY, localX, localY,
                      localWidth, localHeight);
        if (localWidth > 0 && localHeight > 0) {
            hasSelection = true;
            selectionMin = toOverlay(localX, localY);
            selectionMax = toOverlay(localX + localWidth, localY + localHeight);
        }
    }
    if (hasSelection) {
        // 上 / 下 / 左 / 右 四块遮罩（顺序：先上下整行，再左右只盖选区高度）。
        draw->AddRectFilled(origin, ImVec2(bottomRight.x, selectionMin.y), kDimColor);
        draw->AddRectFilled(ImVec2(origin.x, selectionMax.y), bottomRight, kDimColor);
        draw->AddRectFilled(ImVec2(origin.x, selectionMin.y),
                            ImVec2(selectionMin.x, selectionMax.y), kDimColor);
        draw->AddRectFilled(ImVec2(selectionMax.x, selectionMin.y),
                            ImVec2(bottomRight.x, selectionMax.y), kDimColor);
    } else {
        draw->AddRectFilled(origin, bottomRight, kDimColor);
    }

    // 2. 十字线：贯穿整屏，方便对齐边缘。
    const ImVec2 cursor = toOverlay(drag.cursorX, drag.cursorY);
    draw->AddLine(ImVec2(origin.x, cursor.y), ImVec2(bottomRight.x, cursor.y), kCrosshairColor,
                  1.0f);
    draw->AddLine(ImVec2(cursor.x, origin.y), ImVec2(cursor.x, bottomRight.y), kCrosshairColor,
                  1.0f);

    // 3. 选区边框 + 尺寸标签（选区内部不填充：透明窗口已经能透出真实桌面）。
    if (hasSelection) {
        draw->AddRect(selectionMin, selectionMax, kSelectionBorderColor, 0.0f, 0,
                      kBorderThickness);

        // 尺寸标签：优先放选区上方，上方没空间就放下方；再夹进屏幕内。
        char label[64] = {};
        std::snprintf(label, sizeof(label), "%d x %d px", localWidth, localHeight);
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
        ImVec2 textPos(selectionMin.x, selectionMin.y - textSize.y - kLabelGap);
        if (textPos.y < origin.y + 4.0f) {
            textPos.y = selectionMax.y + kLabelGap;
        }
        if (textPos.y + textSize.y > bottomRight.y - 4.0f) {
            textPos.y = selectionMin.y + kLabelGap;
        }
        textPos.x = std::clamp(textPos.x, origin.x + 4.0f,
                               std::max(origin.x + 4.0f, bottomRight.x - textSize.x - 12.0f));
        draw->AddRectFilled(ImVec2(textPos.x - 6.0f, textPos.y - 4.0f),
                            ImVec2(textPos.x + textSize.x + 6.0f, textPos.y + textSize.y + 4.0f),
                            kHintBackColor, 4.0f);
        draw->AddText(font, fontSize, textPos, kHintTextColor, label);
    }

    // 4. 顶部提示：说明怎么用、怎么取消。始终画在最上层。
    const char* hint = "拖拽选择区域（物理像素） · Esc / 右键取消";
    const ImVec2 hintSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, hint);
    const ImVec2 hintPos(origin.x + (size.x - hintSize.x) * 0.5f, origin.y + 24.0f);
    draw->AddRectFilled(ImVec2(hintPos.x - 8.0f, hintPos.y - 5.0f),
                        ImVec2(hintPos.x + hintSize.x + 8.0f, hintPos.y + hintSize.y + 5.0f),
                        kHintBackColor, 5.0f);
    draw->AddText(font, fontSize, hintPos, kHintTextColor, hint);
}

}  // namespace region_selector
