// region_overlay_draw —— 覆盖层的 ImGui 绘制：遮罩 / 选区 / 主题色折角 / 十字线 / 标签。
//
// 从 region_overlay.cpp 拆出来的原因（STYLE.md §6.8）：交互状态机 + 吸附动画 + 绘制
// 放在一个文件里会超过 500 行，而且"状态推进"和"每帧画什么"是两个可以命名的分区。
// 本文件只读 Impl 的状态（hover / drag / 主题色 / 背景纹理），不改任何交互状态。
//
// 绘制全部用 ImGui 的 background draw list，不创建 ImGui 窗口：覆盖层没有布局、
// 没有控件，只是把桌面快照当背景铺满、把选区外画暗、把选区挖空（露出背景快照）、
// 画十字线/主题色折角和尺寸标签。
//
// 坐标：Impl 里存的是窗口内容坐标（物理像素）。绘制用 framebuffer 坐标，两者在
// Windows PMv2 下相等（实测 2560x1600 == 2560x1600）；这里仍按 ActualScale 换算一次，
// 万一将来窗口被系统缩放也不会画歪。content scale 只放大字号与折角/边框这类装饰，
// 绝不乘进区域坐标。
//
// Linux / Wayland：背景是一张等比缩放居中显示的 portal 截图，ImGui 坐标就是窗口内容
// 坐标（framebuffer 比例由后端在投影里处理），换算系数因此恒为 1；尺寸标签显示的是
// 换算后的**截图图像像素**，与落盘文件一致。Windows 分支的绘制代码保持不变。

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>

#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

#include <GLFW/glfw3.h>

#include "imgui.h"

namespace region_selector {
namespace {

/** @brief 遮罩：纯黑约 51% 不透明度，压暗但不至于看不清下面的内容。 */
constexpr ImU32 kDimColor = IM_COL32(0, 0, 0, 130);
/** @brief 十字线：半透明白，不抢选区边框的视觉焦点。 */
constexpr ImU32 kCrosshairColor = IM_COL32(255, 255, 255, 110);
/** @brief 提示文字与它的底色。 */
constexpr ImU32 kHintTextColor = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kHintBackColor = IM_COL32(0, 0, 0, 170);

/** @brief 边框与折角的线宽（逻辑像素）：绘制时再乘显示器 content scale。 */
constexpr float kBorderThicknessLogical = 2.0f;
/** @brief 折角长度 = clamp(min(宽,高) * 0.2, 12, 40) 逻辑像素（需求给定）。 */
constexpr float kCornerRatio = 0.2f;
constexpr float kCornerMinLogical = 12.0f;
constexpr float kCornerMaxLogical = 40.0f;

/** @brief 尺寸标签与选区之间的间距（像素）。 */
constexpr float kLabelGap = 8.0f;

/**
 * @brief 取覆盖层文案：i18n 不可用（老宿主 / 初始化失败）时退回中文原文。
 *
 * 只是把空指针判断收在一处；tr() 查的是插件本地表（O(1)、不碰宿主），所以每帧
 * 直接调用没有问题。返回值指向语言表节点，窗口存活期间一直有效，但不要跨帧缓存。
 * 与任何类无关、只在本 TU 用，所以放匿名命名空间（STYLE.md §6.7）。
 */
const char* overlayText(const shot::I18n* i18n, const char* key, const char* fallback) {
    return (i18n != nullptr) ? i18n->tr(key, fallback) : fallback;
}

/** @brief 0xRRGGBB → ImGui 不透明颜色。 */
ImU32 themeColorToImU32(std::uint32_t rgb) {
    return IM_COL32(static_cast<int>((rgb >> 16) & 0xFFu),
                    static_cast<int>((rgb >> 8) & 0xFFu), static_cast<int>(rgb & 0xFFu), 255);
}

/**
 * @brief 画四个 L 形折角（主题色）。
 *
 * @param[in] min 选框左上角（绘制坐标）。
 * @param[in] max 选框右下角。
 * @param[in] length 每段折角长度；调用方已按需求 clamp 过。
 * @param[in] color 主题色。
 * @param[in] thickness 折角线宽。
 */
void drawCornerBrackets(ImDrawList* draw, const ImVec2& min, const ImVec2& max, float length,
                        ImU32 color, float thickness) {
    const ImVec2 topLeft(min.x, min.y);
    const ImVec2 topRight(max.x, min.y);
    const ImVec2 bottomLeft(min.x, max.y);
    const ImVec2 bottomRight(max.x, max.y);
    // 左上：竖→横；其余三个角对称。
    draw->AddLine(ImVec2(topLeft.x, topLeft.y + length), topLeft, color, thickness);
    draw->AddLine(topLeft, ImVec2(topLeft.x + length, topLeft.y), color, thickness);
    draw->AddLine(ImVec2(topRight.x - length, topRight.y), topRight, color, thickness);
    draw->AddLine(topRight, ImVec2(topRight.x, topRight.y + length), color, thickness);
    draw->AddLine(ImVec2(bottomLeft.x, bottomLeft.y - length), bottomLeft, color, thickness);
    draw->AddLine(bottomLeft, ImVec2(bottomLeft.x + length, bottomLeft.y), color, thickness);
    draw->AddLine(ImVec2(bottomRight.x - length, bottomRight.y), bottomRight, color, thickness);
    draw->AddLine(bottomRight, ImVec2(bottomRight.x, bottomRight.y - length), color, thickness);
}

}  // namespace


// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
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
#if defined(_WIN32)
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
                             ? static_cast<float>(framebufferHeight) /
                                   static_cast<float>(windowHeight)
                             : 1.0f;
#else
    // Wayland：ImGui 的坐标空间就是窗口内容坐标，framebuffer 与窗口的比例由后端在投影
    // 里处理一次；这里再乘一次会让分数缩放下的选区与背景错位，所以恒为 1。
    const float scaleX = 1.0f;
    const float scaleY = 1.0f;
#endif
    const auto toOverlay = [&](float localX, float localY) {
        return ImVec2(origin.x + localX * scaleX, origin.y + localY * scaleY);
    };

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (draw == nullptr) {
        return;
    }
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const ImU32 themeColor = themeColorToImU32(themeColorRgb);
    const float borderThickness = kBorderThicknessLogical * uiScale;

    // 0. 背景：触发瞬间的桌面快照。窗口是不透明的，这张图就是用户看到的"桌面"，
    //    遮罩与挖空都画在它上面（等价于透明覆盖层，但不依赖 DWM 合成）。
    if (backgroundTexture != 0) {
#if defined(_WIN32)
        // 按纹理原始尺寸画（= 显示器尺寸），不要拉满整个窗口：窗口比显示器大 1px，
        // 拉满会把快照整体拉伸 1px。多出来的那条边在屏幕外，看不见。
        const ImVec2 backgroundMax(origin.x + static_cast<float>(backgroundWidth),
                                   origin.y + static_cast<float>(backgroundHeight));
        draw->AddImage(static_cast<ImTextureID>(backgroundTexture), origin, backgroundMax);
#else
        // Wayland：整张 portal 截图（可能是多显示器合成图）等比缩放后居中显示，
        // 留出的黑边也在窗口内；布局每帧按实际窗口尺寸重算（合成器可能改变窗口大小）。
        const WaylandLayout layout = overlayLayout();
        if (layout.width > 0.0f && layout.height > 0.0f) {
            const ImVec2 backgroundMin(origin.x + layout.offsetX, origin.y + layout.offsetY);
            const ImVec2 backgroundMax(backgroundMin.x + layout.width,
                                       backgroundMin.y + layout.height);
            draw->AddImage(static_cast<ImTextureID>(backgroundTexture), backgroundMin,
                           backgroundMax);
        }
#endif
    }

    // 1. 选框：拖动时严格跟手；否则用悬停吸附框（可能正处在平滑动画中间）。
    bool hasSelection = false;
    ImVec2 selectionMin;
    ImVec2 selectionMax;
    int sizeWidth = 0;
    int sizeHeight = 0;
    if (drag.dragging) {
        int localX = 0;
        int localY = 0;
        int localWidth = 0;
        int localHeight = 0;
        normalizeRect(drag.startX, drag.startY, drag.cursorX, drag.cursorY, localX, localY,
                      localWidth, localHeight);
        if (localWidth > 0 && localHeight > 0) {
            hasSelection = true;
            selectionMin = toOverlay(static_cast<float>(localX), static_cast<float>(localY));
            selectionMax =
                toOverlay(static_cast<float>(localX + localWidth),
                          static_cast<float>(localY + localHeight));
#if defined(_WIN32)
            sizeWidth = localWidth;
            sizeHeight = localHeight;
#else
            // 标签显示换算到截图图像像素的尺寸：等比缩放后窗口像素 ≠ 最终输出像素，
            // 用户看到的数字必须和落盘文件一致（落在黑边上的部分不计）。
            const ImageRect imageRect =
                selectionToImagePixels(localX, localY, localWidth, localHeight);
            sizeWidth = imageRect.width;
            sizeHeight = imageRect.height;
#endif
        }
    } else if (hover.active && hover.width > 0.5f && hover.height > 0.5f) {
        hasSelection = true;
        selectionMin = toOverlay(hover.x, hover.y);
        selectionMax = toOverlay(hover.x + hover.width, hover.y + hover.height);
        // 标签显示**目标窗口**的真实尺寸，不是动画中间值：用户关心最终会截多大。
        sizeWidth = hover.target.width;
        sizeHeight = hover.target.height;
    }

    // 2. 暗色遮罩：只压暗"选框之外"的四块，选框本身保持快照原样（等价于挖空）。
    if (hasSelection) {
        // 选框可能大到超出覆盖层（窗口横跨多屏）：先夹到视口内，避免出现倒置的矩形。
        const ImVec2 dimMin(std::max(origin.x, selectionMin.x),
                            std::max(origin.y, selectionMin.y));
        const ImVec2 dimMax(std::min(bottomRight.x, selectionMax.x),
                            std::min(bottomRight.y, selectionMax.y));
        const auto fillDim = [&](const ImVec2& min, const ImVec2& max) {
            if (min.x < max.x && min.y < max.y) {
                draw->AddRectFilled(min, max, kDimColor);
            }
        };
        fillDim(origin, ImVec2(bottomRight.x, dimMin.y));
        fillDim(ImVec2(origin.x, dimMax.y), bottomRight);
        fillDim(ImVec2(origin.x, dimMin.y), ImVec2(dimMin.x, dimMax.y));
        fillDim(ImVec2(dimMax.x, dimMin.y), ImVec2(bottomRight.x, dimMax.y));
    } else {
        draw->AddRectFilled(origin, bottomRight, kDimColor);
    }

    // 3. 十字线：贯穿整屏，方便对齐边缘。
    const ImVec2 cursor =
        toOverlay(static_cast<float>(drag.cursorX), static_cast<float>(drag.cursorY));
    draw->AddLine(ImVec2(origin.x, cursor.y), ImVec2(bottomRight.x, cursor.y), kCrosshairColor,
                  1.0f);
    draw->AddLine(ImVec2(cursor.x, origin.y), ImVec2(cursor.x, bottomRight.y), kCrosshairColor,
                  1.0f);

    // 4. 选框边框、四角折角、尺寸标签。边框与折角都用主题色。
    if (hasSelection) {
        draw->AddRect(selectionMin, selectionMax, themeColor, 0.0f, 0, borderThickness);

        // 折角长度按需求 clamp(min(w,h)*0.2, 12, 40) 逻辑像素，再乘 content scale
        // 变成物理像素（150% 屏上 12~40 逻辑像素 = 18~60 物理像素）。
        const float selectionWidth = selectionMax.x - selectionMin.x;
        const float selectionHeight = selectionMax.y - selectionMin.y;
        const float minSide = std::min(selectionWidth, selectionHeight);
        const float logicalLength =
            std::clamp(minSide * kCornerRatio / ((uiScale > 0.0f) ? uiScale : 1.0f),
                       kCornerMinLogical, kCornerMaxLogical);
        float cornerLength = logicalLength * uiScale;
        // 极小的选框（拖动初期）让折角别越过对面角，否则四个 L 会叠成一团。
        cornerLength = std::min(cornerLength, minSide * 0.5f);
        if (cornerLength > 0.5f) {
            drawCornerBrackets(draw, selectionMin, selectionMax, cornerLength, themeColor,
                               borderThickness);
        }

        // 尺寸标签：优先放选框上方，上方没空间就放下方；再夹进屏幕内。
        // 数字本身与语言无关，只有单位跟着界面语言走（zh-CN"像素" / en-US"px"）。
        char label[64] = {};
        const char* unit = overlayText(i18n, "overlay.size.unit", "像素");
        std::snprintf(label, sizeof(label), "%d x %d %s", sizeWidth, sizeHeight, unit);
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

    // 5. 顶部提示：说明怎么用、怎么取消。始终画在最上层。
    //    文案来自语言表（region.hint；Wayland 用 region.hint.wayland，因为单击不再截窗口）。
    //    日志才是固定中文，别把这里改回写死的中文。
#if defined(_WIN32)
    const char* hint =
        overlayText(i18n, "region.hint", "单击截取窗口 · 拖拽选择区域 · Esc / 右键取消");
#else
    const char* hint = overlayText(i18n, "region.hint.wayland", "拖拽选择区域 · Esc / 右键取消");
#endif
    const ImVec2 hintSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, hint);
    const ImVec2 hintPos(origin.x + (size.x - hintSize.x) * 0.5f, origin.y + 24.0f);
    draw->AddRectFilled(ImVec2(hintPos.x - 8.0f, hintPos.y - 5.0f),
                        ImVec2(hintPos.x + hintSize.x + 8.0f, hintPos.y + hintSize.y + 5.0f),
                        kHintBackColor, 5.0f);
    draw->AddText(font, fontSize, hintPos, kHintTextColor, hint);
}

}  // namespace region_selector
