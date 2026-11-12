#pragma once

// ============================================================================
//  include/clipboard_x11.h —— Linux/X11 剪贴板：拥有 CLIPBOARD selection
//
//  X11 的剪贴板不是"写进去就完事"：数据属于 selection 的 owner，别的程序粘贴时
//  会发 SelectionRequest 来要；owner 退出或放弃所有权（SelectionClear）后数据就
//  没了。所以本类必须常驻一个线程跑 Xlib 事件循环，把最近一次 capture.done 的
//  图片与文件路径保存在内存里，按请求返回 image/png / text/uri-list。
//
//  线程模型：宿主事件分发线程（main）只调用 copyImageFile()，同步完成读图 + PNG
//  编码，然后把字节移交给 X11 线程；所有 Xlib 调用（XOpenDisplay / XSetSelectionOwner
//  / XChangeProperty / XSendEvent）都只在 X11 线程里发生，避免 Xlib 的线程安全
//  问题扩散到宿主与其它插件。
// ============================================================================

#include <memory>
#include <string>

#include "clipboard_image.h"
#include "plugin_api.h"

namespace clipboard {

/**
 * @brief Linux/X11 剪贴板写入器：在专属线程里拥有 CLIPBOARD selection 并按需应答。
 *
 * 与 Windows 的 `ClipboardWriter` 接口形状保持一致（`copyImageFile`），调用方只
 * 看到 bool + 中文失败原因；但语义是"把这份数据设为当前剪贴板内容"，成功后插件
 * 会一直拥有 selection，直到下一次截图替换它、其它程序抢走它（SelectionClear），
 * 或插件卸载（stop() 主动放弃所有权）。
 */
class ClipboardX11 {
public:
    /** @brief 构造：还没有 X11 线程与 Display，start() 之后才可用。 */
    ClipboardX11();

    /** @brief 析构：等价于 stop()，保证线程与 Display 已释放。 */
    ~ClipboardX11();

    /** @brief 不可拷贝：持有线程、Display 与 selection 状态，复制没有意义。 */
    ClipboardX11(const ClipboardX11&) = delete;
    /** @brief 不可赋值。 */
    ClipboardX11& operator=(const ClipboardX11&) = delete;

    /**
     * @brief 起 X11 线程、打开 Display 并等待就绪。
     *
     * Wayland 会话通常没有 X11 Display（DISPLAY 未设置或 XWayland 不可用）：
     * 此时返回 false + error，调用方只记 WARN，截屏与其它插件不受影响。
     *
     * @param[in]  host  宿主服务表，用于日志；可为 nullptr（老宿主）。
     * @param[out] error 失败原因（中文）；成功时清空。
     * @return true = X11 线程与 Display 已就绪；false = 无 X11 或初始化失败。
     */
    bool start(const ShotHostServices* host, std::string& error);

    /**
     * @brief 请求退出 X11 线程：先放弃 selection 所有权，再 join。
     *
     * 可重复调用；返回时线程一定已退出、Display 已关闭，宿主之后 FreeLibrary 不会
     * 留下指向本 DLL 的事件处理代码。
     */
    void stop();

    /**
     * @brief 读取图片文件并把它设为当前 CLIPBOARD 内容（image/png + 可选文件列表）。
     *
     * 读图与 PNG 编码在调用线程同步完成；编码成功后把字节排队给 X11 线程。函数
     * 返回 true 只表示"数据已交给 X11 线程"，真正的所有权建立与 TARGETS 自检
     * （等待 SelectionNotify）在 X11 线程里异步完成，结果写日志。
     *
     * @param[in]  utf8Path    保存插件写回的文件路径（UTF-8），不能为空。
     * @param[in]  alsoFileDrop true = 额外提供 text/uri-list（拖文件语义）。
     * @param[out] error       失败时写入中文原因；成功时清空。
     * @return true = 已排入 X11 线程；false = 读图/编码失败或 X11 线程不可用。
     */
    bool copyImageFile(const std::string& utf8Path, bool alsoFileDrop, std::string& error);

private:
    struct Impl;                    /**< 内部实现：X11 线程、selection 状态与协议处理。 */
    std::unique_ptr<Impl> m_impl;   /**< PIMPL：Xlib 类型只出现在内部头与 .cpp 里。 */
};

}  // namespace clipboard
