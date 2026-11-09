#pragma once

// ============================================================================
//  include/clipboard_writer.h —— 把图片文件写进 Win32 剪贴板
//
//  输入是保存插件在 capture.done 里报告的**文件路径**（插件不持有像素：像素不进
//  事件总线，见 document/CAPTURE.md §3）。本类负责：
//    1. 用 std::filesystem（UTF-8 → 宽字符）读文件，不依赖 stb 的按文件名接口
//       （那条路走 ANSI，中文目录会打不开）；
//    2. 用 stb_image 解码成 RGBA，转成 CF_DIB 期望的 32bpp BGRA + bottom-up 行序；
//    3. OpenClipboard（被占用时重试）→ EmptyClipboard → SetClipboardData；
//    4. 可选再放一份 CF_HDROP（DROPFILES + 宽字符路径双 NUL 结尾）。
//
//  头文件里**不出现任何 Win32 类型**：窗口/句柄/结构体都封在 .cpp 里，调用方
//  只看到一个 bool + 一条中文失败原因（STYLE.md §6.3 的 PIMPL 边界精神）。
// ============================================================================

#include <string>

namespace clipboard {

/**
 * @brief 图片文件 → 系统剪贴板的写入器；无状态，可重复使用。
 *
 * 只在宿主事件分发线程上调用（capture.done 的订阅回调里），不自带线程；剪贴板是
 * 进程间共享资源，写入失败（被其它程序占用、文件读不出来、解码失败）只返回
 * false + error，由调用方记 WARN，绝不影响宿主继续截屏。
 */
class ClipboardWriter {
public:
    ClipboardWriter() = default;

    /** @brief 无状态，可以共用；禁止拷贝只是为了保持"工具类"语义清晰。 */
    ClipboardWriter(const ClipboardWriter&) = delete;
    /** @brief 禁止赋值。 */
    ClipboardWriter& operator=(const ClipboardWriter&) = delete;

    /**
     * @brief 读取图片文件并写入系统剪贴板。
     *
     * 支持的输入格式由 stb_image 决定（PNG / JPEG / BMP / GIF / TGA / HDR / PSD /
     * PNM），与保存插件实际落盘的格式一致（v1 是 PNG/JPEG）。像素统一转成
     * 32bpp BGRA、bottom-up 行序的 CF_DIB；alsoFileDrop 为 true 时再额外放一份
     * CF_HDROP（贴进资源管理器会复制该文件）。
     *
     * @param[in]  utf8Path    保存插件写回的文件路径（UTF-8），不能为空。
     * @param[in]  alsoFileDrop true = 同时提供 CF_HDROP 文件列表。
     * @param[out] error       失败时写入中文原因；成功时清空。
     * @return true = 剪贴板已按请求的格式全部写入；false = 未写入或只写了一部分
     *         （原因见 error，此时剪贴板状态以实际写入为准）。
     * @note OpenClipboard 被其它程序占用时最多重试 10 次、每次间隔 20ms；仍失败
     *       返回 false，调用方 warn 后丢弃本次事件即可。
     */
    bool copyImageFile(const std::string& utf8Path, bool alsoFileDrop, std::string& error) const;
};

}  // namespace clipboard
