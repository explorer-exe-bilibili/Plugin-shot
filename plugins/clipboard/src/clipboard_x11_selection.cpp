// X11 selection 协议应答：TARGETS / TIMESTAMP / MULTIPLE / image/png / text/uri-list。
//
// ICCCM 的剪贴板流程（简化）：
//   1. owner 声称拥有 CLIPBOARD；
//   2. 请求方给自己窗口选一个 property，发 SelectionRequest 到 owner；
//   3. owner 把数据 XChangeProperty 到请求方的 property 上，再回 SelectionNotify；
//   4. 请求方收到 SelectionNotify 后读取 property。
//
// 这里的写操作目标是**请求方的窗口**，不是自己的窗口；请求方可能在中途退出
// （典型的剪贴板管理器 quick-close），XChangeProperty 会异步报 BadWindow。Xlib 的
// 默认错误处理器遇到协议错误会直接 exit(1)，所以本文件只在处理一条 SelectionRequest
// 的短窗口内临时接管处理器并 XSync 消费错误（见 handleEvent）；不能长期替换进程级
// 处理器，那会影响宿主里 GLFW/settings_ui 的 Xlib 使用。
//
// MULTIPLE 是 ICCCM 规定的"一次请求多个 target"：请求方在 property 里放
// (target, property) 原子对，owner 逐个写入并把失败的 property 改成 None 再写回。
// 现代的粘贴方很少用它，但剪贴板管理器（Klipper 等）会用它批量探测，必须支持，
// 否则会被当成"不完整的 owner"。

#include "clipboard_x11.h"

#include <climits>
#include <cstdint>
#include <string>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include "clipboard_x11_impl.h"
#include "shot_plugin_helpers.h"

namespace clipboard {
namespace {

/**
 * @brief 忽略本插件应答期间产生的 Xlib 协议错误。
 *
 * Xlib 的默认错误处理器遇到 BadWindow 会 `exit(1)`。剪贴板管理器完全可能在发出
 * SelectionRequest 后马上销毁自己的临时探测窗口，这时 XChangeProperty / XSendEvent
 * 指向的窗口已经不存在；不能让一次探测把宿主杀掉。所以只在处理一条请求的短窗口内
 * 临时接管错误处理器，处理完用 XSync 把异步错误消费掉再恢复。
 *
 * @note 不能长期替换：错误处理器是进程级的，会和 settings_ui/GLFW 的 Xlib 调用
 *       互相影响；这里借用的是“同步点之前”的短临界区。
 *
 * @param[in] display 出错连接（不区分，均由本插件请求触发）。
 * @param[in] event   错误事件（只用于忽略，不读取）。
 * @return 0 = 已处理，Xlib 继续运行。
 */
int ignoreXError(Display* /*display*/, XErrorEvent* /*event*/) {
    return 0;
}

/**
 * @brief 把一段数据写进请求方窗口的 property（XChangeProperty 的薄封装）。
 * @param[in] display   本插件的 X11 连接。
 * @param[in] requestor 请求方窗口；不能为 None。
 * @param[in] property  请求方指定的 property；不能为 None。
 * @param[in] type      属性的类型 atom（XA_ATOM / image/png / text/uri-list ...）。
 * @param[in] format    8 / 16 / 32。
 * @param[in] data      数据首地址；count 为 0 时可为 nullptr。
 * @param[in] count     元素个数（32 位格式时是 long 个数，不是字节数）。
 * @return true = 已提交给 X server（异步错误不影响返回值）。
 */
bool changeProperty(Display* display, Window requestor, Atom property, Atom type, int format,
                    const void* data, unsigned long count) {
    if (display == nullptr || requestor == None || property == None) {
        return false;
    }
    if (count > static_cast<unsigned long>(INT_MAX)) {
        return false;
    }
    XChangeProperty(display, requestor, property, type, format, PropModeReplace,
                    static_cast<const unsigned char*>(data), static_cast<int>(count));
    return true;
}

}  // namespace

void ClipboardX11::Impl::handleEvent(const XEvent& event) {
    switch (event.type) {
        case SelectionRequest: {
            // 只在处理一条请求的短临界区里接管错误处理器；XSync 把异步的 BadWindow
            // 消费掉后再恢复。用 try 是为了 handleSelectionRequest 里万一有异常
            // （容器分配失败）时也一定恢复，别把函数指针留在 DLL 里。
            const XErrorHandler previous = XSetErrorHandler(&ignoreXError);
            try {
                handleSelectionRequest(event.xselectionrequest);
                XSync(display, False);
            } catch (...) {
                XSetErrorHandler(previous);
                throw;
            }
            XSetErrorHandler(previous);
            break;
        }
        case SelectionClear:
            handleSelectionClear(event.xselectionclear);
            break;
        case SelectionNotify:
            // SelectionNotify 结构体在 Xlib 里叫 XSelectionEvent（不是 XSelectionNotifyEvent）。
            handleSelectionNotify(event.xselection);
            break;
        default:
            break;  // PropertyNotify 等：serverTime() 已经在自己的等待里消费掉
    }
}

void ClipboardX11::Impl::handleSelectionRequest(const XSelectionRequestEvent& request) {
    if (request.requestor == None) {
        return;
    }
    if (request.selection != clipboardAtom || !hasPayload) {
        // 没数据/不是 CLIPBOARD：按 ICCCM 回 property=None，请求方会当作"不支持"。
        sendSelectionNotify(request, None);
        return;
    }

    Atom property = None;
    if (request.property != None) {
        bool written = false;
        if (request.target == multipleAtom) {
            written = writeMultiple(request);
        } else {
            written = writeTarget(request.requestor, request.property, request.target);
        }
        if (written) {
            property = request.property;
        }
    }
    sendSelectionNotify(request, property);
}

void ClipboardX11::Impl::handleSelectionClear(const XSelectionClearEvent& event) {
    if (event.selection != clipboardAtom) {
        return;
    }
    // 别的程序（用户复制了文字 / 剪贴板管理器接管）拿到了所有权：按 ICCCM 必须
    // 停止提供服务并释放数据，之后本插件的 SelectionRequest 都不再合法。
    owned = false;
    awaitingSelfCheck = false;
    releasePayload();
    shot::Logger(host).info("[clipboard] 收到 SelectionClear：另一个程序取得 CLIPBOARD，已释放本插件数据");
}

void ClipboardX11::Impl::handleSelectionNotify(const XSelectionEvent& event) {
    if (!awaitingSelfCheck || event.selection != clipboardAtom || event.target != targetsAtom) {
        return;
    }
    awaitingSelfCheck = false;
    if (event.property == selfCheckAtom) {
        XDeleteProperty(display, window, selfCheckAtom);
        XFlush(display);
        shot::Logger(host).info(
            "[clipboard] TARGETS 自检成功（收到 SelectionNotify），selection 已可被其它程序读取");
        return;
    }
    // property=None：X server 或 owner 拒绝了这次转换。数据仍在内存里，下一次
    // capture.done 会重新 claim；这次只 WARN，不影响宿主。
    shot::Logger(host).warn(
        "[clipboard] TARGETS 自检失败（SelectionNotify.property=None），本次剪贴板可能不可用");
}

bool ClipboardX11::Impl::writeTarget(Window requestor, Atom property, Atom target) {
    if (!hasPayload) {
        return false;
    }
    if (target == targetsAtom) {
        // TARGETS 必须列出所有能提供的 target；TIMESTAMP 让剪贴板管理器判断
        // "这份数据比它缓存的新还是旧"，MULTIPLE 表示支持批量请求。
        Atom supported[5] = {targetsAtom, timestampAtom, multipleAtom, pngAtom, None};
        unsigned long count = 4;
        if (payload.hasUriList && !payload.uriList.empty()) {
            supported[count] = uriListAtom;
            ++count;
        }
        return changeProperty(display, requestor, property, XA_ATOM, 32, supported, count);
    }
    if (target == pngAtom) {
        if (payload.png.empty()) {
            return false;
        }
        return changeProperty(display, requestor, property, pngAtom, 8, payload.png.data(),
                              payload.png.size());
    }
    if (target == uriListAtom) {
        if (!payload.hasUriList || payload.uriList.empty()) {
            return false;
        }
        return changeProperty(display, requestor, property, uriListAtom, 8,
                              payload.uriList.data(), payload.uriList.size());
    }
    if (target == timestampAtom) {
        // TIMESTAMP 的 32 位值是取得所有权时的服务器时间，不是本机墙钟。
        const long stamp = static_cast<long>(ownershipTime);
        return changeProperty(display, requestor, property, XA_INTEGER, 32, &stamp, 1);
    }
    // MULTIPLE 只作为请求入口处理，不能作为 MULTIPLE 里的子 target 递归。
    return false;
}

bool ClipboardX11::Impl::writeMultiple(const XSelectionRequestEvent& request) {
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* raw = nullptr;
    // 1024 个 32 位槽足够放几十个 target；remaining 非 0 说明列表被截断，直接拒绝
    // 比返回半份列表安全（请求方会认为整个 MULTIPLE 失败）。
    const int status = XGetWindowProperty(display, request.requestor, request.property, 0, 1024,
                                          False, XA_ATOM, &actualType, &actualFormat, &count,
                                          &remaining, &raw);
    if (status != Success || raw == nullptr) {
        if (raw != nullptr) {
            XFree(raw);
        }
        return false;
    }
    const bool valid = actualType == XA_ATOM && actualFormat == 32 && remaining == 0 &&
                       count > 0 && (count % 2) == 0;
    bool convertedAny = false;
    if (valid) {
        auto* atoms = reinterpret_cast<Atom*>(raw);
        for (unsigned long index = 0; index < count; index += 2) {
            if (atoms[index + 1] == None) {
                continue;  // 请求方明确要求跳过这一对
            }
            if (writeTarget(request.requestor, atoms[index + 1], atoms[index])) {
                convertedAny = true;
            } else {
                // 不支持的 target：按 ICCCM 把对应的 property 改成 None，其余继续。
                atoms[index + 1] = None;
            }
        }
        XChangeProperty(display, request.requestor, request.property, XA_ATOM, 32,
                        PropModeReplace, static_cast<const unsigned char*>(raw),
                        static_cast<int>(count));
    }
    XFree(raw);
    // 一个都没转换成功时回 SelectionNotify.property=None；否则请求方会拿到一份
    // 全是 None 的属性，误以为 MULTIPLE 整体成功。
    return valid && convertedAny;
}

void ClipboardX11::Impl::sendSelectionNotify(const XSelectionRequestEvent& request,
                                             Atom property) {
    XSelectionEvent notify = {};
    notify.type = SelectionNotify;
    notify.display = request.display;
    notify.requestor = request.requestor;
    notify.selection = request.selection;
    notify.target = request.target;
    notify.property = property;
    notify.time = request.time;
    XSendEvent(display, request.requestor, False, 0, reinterpret_cast<XEvent*>(&notify));
    XFlush(display);
}

Time ClipboardX11::Impl::serverTime() {
    // XSetSelectionOwner 需要一个真实的 server timestamp（ICCCM）：部分剪贴板管理器
    // 会把 CurrentTime(0) 当成"过期所有权"而拒绝接管。借一次 PropertyNotify 拿时间：
    // 改自己的 property → XWindowEvent 等这条事件 → 事件里的 time 就是服务器时间。
    // 用 Replace 而不是 Append：只要属性变化 server 就会发 PropertyNotify，Replace
    // 不会让一个内部属性随着截图次数无限增长。
    long marker = 0;
    XChangeProperty(display, window, timeAtom, XA_INTEGER, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&marker), 1);
    XEvent event = {};
    for (;;) {
        XWindowEvent(display, window, PropertyChangeMask, &event);
        if (event.type == PropertyNotify && event.xproperty.atom == timeAtom) {
            return event.xproperty.time;
        }
    }
}

}  // namespace clipboard
