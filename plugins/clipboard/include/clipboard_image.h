#pragma once

// ============================================================================
//  include/clipboard_image.h —— Linux/X11 剪贴板的负载：图片文件 → PNG + URI 列表
//
//  为什么单独成一个类：读图、编码、拼 text/uri-list 都是纯数据处理，和 X11 的
//  selection 所有权 / 事件循环（ClipboardX11）是两件事。前者在宿主事件分发线程
//  同步做（与 Windows 的 ClipboardWriter 一致），后者在专属线程里常驻；拆开后
//  X11 线程只碰字节缓冲，不再关心文件与 stb。
//
//  像素来源仍然是 capture.done 的 path（像素不进事件总线，见 document/CAPTURE.md
//  §3），本类只消费保存插件刚写好的完整文件。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace clipboard {

/**
 * @brief 把图片文件编码成 X11 CLIPBOARD 要提供的两种 target 数据。
 *
 * 与 Windows 的 `ClipboardWriter` 对应，但 Linux 的剪贴板不是"一次性写入系统"，
 * 而是"插件自己拥有 selection、在别的程序粘贴时按需返回数据"：所以这里只准备
 * 字节，真正的所有权与请求应答由 ClipboardX11 在 X11 线程完成。
 */
class ClipboardImage {
public:
    /** @brief 一次发布的全部数据；在宿主线程构造后整体移交给 X11 线程。 */
    struct Payload {
        std::vector<std::uint8_t> png;  /**< image/png 编码后的完整字节。 */
        std::string uriList;            /**< text/uri-list 文本（RFC 2483，CRLF 结尾）。 */
        bool hasUriList = false;        /**< false = 不提供 text/uri-list（also_file_drop=0）。 */
    };

    ClipboardImage() = delete;

    /**
     * @brief 读图片文件、用 stb_image 解码并用 stb_image_write 编码成 PNG。
     *
     * 解码格式由 stb_image 决定（保存插件实际落盘的 PNG/JPEG 都支持）；输出统一
     * 是 RGBA PNG。`alsoFileDrop` 为 true 时再生成一份 `text/uri-list`：Linux 上
     * "拖一个文件"的协议就是 URI 列表，等价于 Windows 的 CF_HDROP。
     *
     * @param[in]  utf8Path     保存插件写回的文件路径（UTF-8），不能为空。
     * @param[in]  alsoFileDrop true = 同时准备文件 URI 列表。
     * @param[out] out          成功时写满；失败时内容未定义。
     * @param[out] error        失败时写入中文原因；成功时清空。
     * @return true = PNG 已编码完成（以及按需生成的 URI 列表）。
     * @note 文件上限 512 MiB，防止事件里的路径指向巨大非图片文件时吃爆内存。
     */
    static bool build(const std::string& utf8Path, bool alsoFileDrop, Payload& out,
                      std::string& error);

private:
    /**
     * @brief 把路径转成单行 `file:///...` URI（按 RFC 3986 做百分号编码）。
     * @param[in] utf8Path UTF-8 路径；相对路径先转成绝对路径。
     * @return 以 `\r\n` 结尾的 text/uri-list 文本；路径无法转换时返回空串。
     */
    static std::string makeFileUriList(const std::string& utf8Path);
};

}  // namespace clipboard
