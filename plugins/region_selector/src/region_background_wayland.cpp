// region_background_wayland —— Wayland 版的"覆盖层背景"：portal 整屏截图 + 上传 GL 纹理。
//
// 与 Windows 版（region_background.cpp）的差别：
//   · Windows 用 GDI 截"覆盖层所在显示器"的一块，坐标是屏幕物理像素；
//   · Wayland 只能通过 xdg-desktop-portal 截**整个桌面**（多显示器会合成到一张图里），
//     拿到的是临时 PNG 文件。本文件负责：调门户 → stb 解码 → 删除临时文件 → 把整张图
//     留一份 RGBA 在内存里（后面裁剪用）→ 上传成 GL 纹理当覆盖层背景。
//
// 关键：这张图**只截一次**。用户确认后直接从内存裁剪、写 PNG、发布 capture.done，
// 不会再抓一次屏、也不会再弹第二次授权框（需求"方案 B"）。
//
// 图像的显示方式（等比缩放适配 + 居中）在 region_overlay_wayland.cpp；本文件只负责
// "拿到像素并变成纹理"。
//
// 日志（真机排查"不显示覆盖层"时按这个顺序看）：
//   1. startSelection 的「开始请求 portal 截图（等待授权，最多 30s）」
//   2. region_portal 的「仍在等待 portal 授权…」（每 5s 一条）
//   3. 本文件的「portal 截图返回 status=…（耗时 N ms）」
//   4. 本文件的「已取得 portal 截图 WxH」→ 说明失败只可能发生在显示阶段
// 第 3 行是分水岭：有它 = 门户已经返回；没有它 = 还卡在门户/授权。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>  // 这里主要用它提供的 GL 1.1 声明（纹理）

#include "region_image.h"
#include "region_portal.h"
#include "region_window_impl.h"
#include "shot_plugin_helpers.h"

namespace region_selector {

bool RegionWindow::Impl::captureBackground() {
    const shot::Logger log(host);
    // 失败时默认按 Failed 记：下面任一步失败，startSelection 的"区域框选未开始"才会
    // 写对原因（只是门户返回 ok 但解码失败时，不能报成 status=ok）。
    lastPortalStatus = PortalScreenshotStatus::Failed;
    // 1. 门户截图（首次会弹系统授权框）。取消/拒绝/超时/不可用的日志都在
    //    PortalScreenshot::take 里按级别记过，这里只负责放弃本次框选。
    //    中断检查接 shouldQuit()：宿主退出/热卸载时 stop() 要 join 本线程，不能卡在
    //    授权框上等满超时。
    PortalScreenshot portal(host);
    std::string pngPath;
    const auto startedAt = std::chrono::steady_clock::now();
    const PortalScreenshotStatus status =
        portal.take(pngPath, [this]() { return shouldQuit(); });
    // 记下结果给 startSelection：用户取消保持安静，其它失败要用 error 记录一次。
    lastPortalStatus = status;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - startedAt)
                               .count();
    log.info("region_selector：portal 截图返回 status=" + std::string(portalStatusName(status)) +
             "（耗时 " + std::to_string(static_cast<long long>(elapsedMs)) + " ms）");
    if (status != PortalScreenshotStatus::Ok) {
        return false;
    }

    // 2. 解码成 RGBA8。临时文件属于调用方：无论解码成功与否都要删掉，不然
    //    $XDG_RUNTIME_DIR 里会攒下一堆截图。
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    const bool loaded = loadPngAsRgba(pngPath, pixels, width, height);
    if (std::remove(pngPath.c_str()) != 0) {
        log.warn("region_selector：删除 portal 临时截图失败（" + pngPath + "）");
    }
    if (!loaded || width <= 0 || height <= 0) {
        // 门户给了文件但解码失败：对外仍然算失败（不是用户取消），让 startSelection 用
        // error 记录"区域框选未开始：portal 截图失败（status=failed）"。
        lastPortalStatus = PortalScreenshotStatus::Failed;
        log.warn("region_selector：portal 返回的截图无法解码（" + pngPath + "），放弃本次框选");
        return false;
    }

    // 3. 留一份整屏像素：确认时直接在这份内存上裁剪（不再第二次采集）。
    sourcePixels = std::move(pixels);
    sourceWidth = width;
    sourceHeight = height;

    // 4. 上传 GL 纹理。GL 上下文在 UI 线程上当前有效（createWindow 里 makeCurrent）。
    //    行序：stb 解码出来是自顶向下，与 GL 纹理的 v=0 顶部约定一致，不需要翻转。
    if (backgroundTexture == 0) {
        glGenTextures(1, &backgroundTexture);
    }
    glBindTexture(GL_TEXTURE_2D, backgroundTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sourceWidth, sourceHeight, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, sourcePixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    // 某些老 GPU 的纹理上限小于整屏截图尺寸（多屏 8K 级别）：纹理失败不影响裁剪，
    // 但覆盖层会只剩遮罩，真机排查"整屏黑"时先看这一行。
    if (glGetError() != GL_NO_ERROR) {
        log.warn("region_selector：截图纹理上传失败（超过 GPU 纹理上限？），覆盖层只显示遮罩");
    }
    backgroundWidth = sourceWidth;
    backgroundHeight = sourceHeight;
    log.info("region_selector：已取得 portal 截图 " + std::to_string(sourceWidth) + "x" +
             std::to_string(sourceHeight) + "（图像像素，将等比缩放显示）");
    return true;
}

void RegionWindow::Impl::releaseBackgroundTexture() {
    if (backgroundTexture != 0) {
        glDeleteTextures(1, &backgroundTexture);
        backgroundTexture = 0;
    }
    backgroundWidth = 0;
    backgroundHeight = 0;
    // 整屏像素也一起释放：下一次框选会重新截图；留着只是一份几十 MB 的常驻内存。
    std::vector<std::uint8_t>().swap(sourcePixels);
    sourceWidth = 0;
    sourceHeight = 0;
}

}  // namespace region_selector
