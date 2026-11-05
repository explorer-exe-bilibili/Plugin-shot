// setting_window —— 窗口归插件自己。
//
// 这个文件演示一件宿主完全不知情的事：插件自己起线程、自己跑 Win32 消息循环、
// 自己建/销毁窗口。宿主只调 init / deinit / post_to_plugin 三个函数，
// 既不知道有几个线程，也拿不到 HWND。
//
// 与宿主的唯一通信方向是"宿主 -> 插件"：宿主在任意线程调 post_to_plugin，
// 这里把事件拷进队列并 PostMessage 唤醒窗口线程；窗口线程收工后自己 drain。

#include "setting_window.h"

#include <condition_variable>
#include <mutex>
#include <new>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace setting_window {
namespace {

const char kKeyDarkMode[] = "dark_mode";

// 配置项声明。窗口还没有渲染，暂时只有一项；等 shot.configurable 接上后
// 宿主就能据此生成通用界面。
const ShotConfigDesc kConfig[] = {
    {
        .struct_size   = sizeof(ShotConfigDesc),
        .kind          = SHOT_CONFIG_KIND_BOOL,
        .key           = kKeyDarkMode,
        .label         = "Dark Mode",
        .default_value = "false",
        .options       = nullptr,
        .min           = 0.0,
        .max           = 0.0,
        .step          = 0.0,
        .tooltip       = "Enable or disable dark mode for the setting window.",
    },
};

// 窗口线程收到新事件时用的自定义消息。
constexpr UINT kMsgDrain = WM_APP + 1;

}  // namespace

// ---------------------------------------------------------------------------
// 所有 Win32 资源都关在 Impl 里，绝不跨 DLL 边界。
// ---------------------------------------------------------------------------
struct SettingWindow::Impl {
    std::thread thread;

    // mutex 保护 hwnd 与 queue；两者都由窗口线程和任意调用线程共同访问。
    std::mutex mutex;
    std::queue<std::pair<std::string, std::string>> queue;
    HWND hwnd = nullptr;

    const ShotHostServices* host = nullptr;

    // readyMutex / readyCv 只负责 start() 与窗口线程之间的"窗口建好了没有"握手。
    std::mutex readyMutex;
    std::condition_variable readyCv;
    bool ready = false;
    bool failed = false;

    void run();
    void drain();

    uint32_t post_to_host(const char* name, const char* data, std::uint32_t len) {
        if (host == nullptr || host->post_to_host == nullptr) {
            return 1;  // 宿主不在
        }
        ShotEvent ev{};
        ev.struct_size = sizeof(ShotEvent);
        ev.name = name;
        ev.data = data;
        ev.data_len = len;
        ev.flags = 0;
        return static_cast<uint32_t>(host->post_to_host(host->host_context, &ev));
    }

    static LRESULT CALLBACK proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(wnd, msg, wp, lp);
        }
        switch (msg) {
            case kMsgDrain:
                self->drain();
                return 0;
            case WM_CLOSE:{
                ShowWindow(wnd, SW_HIDE);
                ShotEvent ev{sizeof(ShotEvent), "setting_window", "closed", 0, 0};
                self->host->post_to_host(self->host->host_context, &ev);
            }
                return 0;
            case WM_DESTROY: {
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    self->hwnd = nullptr;
                }
                PostQuitMessage(0);
                return 0;
            }
            default:
                return DefWindowProcW(wnd, msg, wp, lp);
        }
    }
};

// 只在这个线程上跑：拿队列、清空、逐条处理。
void SettingWindow::Impl::drain() {
    std::queue<std::pair<std::string, std::string>> local;
    {
        std::lock_guard<std::mutex> lock(mutex);
        local.swap(queue);
    }
    while (!local.empty()) {
        const auto& [name, payload] = local.front();
        // 事件数据已经拷进 payload 了，这里可以放心用；host 可能比本插件老，
        // 没有 log 字段，所以必须逐字段检查。
        if (host != nullptr && SHOT_HAS_FIELD(host, ShotHostServices, log) &&
            host->log != nullptr) {
            const std::string line = "[setting_window] event=" + name + " data=" + payload;
            host->log(host->host_context, SHOT_LOG_INFO, line.c_str(),
                      static_cast<std::int32_t>(line.size()));
        }
        if(name=="host.shutdown")
        {
            PostQuitMessage(0);
        }
        local.pop();
    }
}

void SettingWindow::Impl::run() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &Impl::proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ShotSettingWindow";
    // IDC_ARROW 展开成 MAKEINTRESOURCE(_A)：本工程没定义 UNICODE，得到的是窄字符
    // 版本，喂不进 LoadCursorW。它本质是个资源整数而不是真指针，所以按整数语义转换。
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW) + 1);
    RegisterClassExW(&wc);  // 已注册过会失败：同进程第二次 init 时这是正常的，忽略

    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Plugin-shot 设置", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 640, 360,
                               nullptr, nullptr, wc.hInstance, this);
    {
        std::lock_guard<std::mutex> lock(mutex);
        hwnd = wnd;
    }
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = (wnd != nullptr);
        failed = (wnd == nullptr);
    }
    readyCv.notify_all();
    if (wnd == nullptr) {
        return;  // 建窗口失败：告过 start() 了，线程直接结束
    }

    ShowWindow(wnd, SW_SHOW);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

SettingWindow::SettingWindow() : m_impl(std::make_unique<Impl>()) {}

SettingWindow::~SettingWindow() {
    stop();  // Impl 在这里才是完整类型，所以构造/析构必须写在本文件里
}

bool SettingWindow::start(const ShotHostServices* host) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->thread.joinable()) {
        return true;  // 已经在跑：重复 init 是空操作
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->readyMutex);
        m_impl->ready = false;
        m_impl->failed = false;
    }
    m_impl->host = host;

    try {
        m_impl->thread = std::thread(&Impl::run, m_impl.get());
    } catch (...) {
        return false;  // 异常绝不跨越 DLL 边界
    }

    {
        std::unique_lock<std::mutex> lock(m_impl->readyMutex);
        m_impl->readyCv.wait(lock, [this] { return m_impl->ready || m_impl->failed; });
        if (!m_impl->failed) {
            return true;
        }
    }
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();  // 已经 return 了，join 不会阻塞
    }
    return false;
}

void SettingWindow::stop() {
    if (!m_impl) {
        return;
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        wnd = m_impl->hwnd;
    }
    if (wnd != nullptr) {
        PostMessageW(wnd, WM_CLOSE, 0, 0);  // 让窗口线程自己走完销毁流程
    }
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    std::queue<std::pair<std::string, std::string>>().swap(m_impl->queue);
}

std::int32_t SettingWindow::post(const char* name, const char* data, std::uint32_t len) {
    if (!m_impl || name == nullptr) {
        return -1;
    }
    std::string payload;
    if (data != nullptr && len > 0) {
        payload.assign(data, len);  // 契约要求：返回前把 data_len 字节拷走
    }
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->hwnd == nullptr) {
            return 1;  // 窗口不在：事件丢弃，让宿主知道没送到
        }
        m_impl->queue.emplace(name, std::move(payload));
        wnd = m_impl->hwnd;
    }
    if (PostMessageW(wnd, kMsgDrain, 0, 0) == 0) {
        return 2;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ABI 胶水：宿主只认这三个函数 + 一张能力目录。
// ---------------------------------------------------------------------------
namespace {

struct SettingWindowState {
    bool dark_mode = false;
    SettingWindow window;
};

void* create_state() { return new (std::nothrow) SettingWindowState(); }

void destroy_state(void* self) {
    auto* state = static_cast<SettingWindowState*>(self);
    if (state != nullptr) {
        state->window.stop();  // 先收窗口，再释放状态
        delete state;
    }
}

std::int32_t ui_init(void* self, const ShotHostServices* host) {
    auto* state = static_cast<SettingWindowState*>(state_of(self));
    if (state == nullptr) {
        return 1;
    }
    return state->window.start(host) ? 0 : 1;
}

std::int32_t ui_deinit(void* self) {
    auto* state = static_cast<SettingWindowState*>(state_of(self));
    if (state != nullptr) {
        state->window.stop();  // 返回前线程已停、窗口已销毁
    }
    return 0;
}

std::int32_t ui_post(void* self, const ShotEvent* ev) {
    auto* state = static_cast<SettingWindowState*>(state_of(self));
    if (state == nullptr || ev == nullptr || ev->name == nullptr) {
        return -1;
    }
    if (!SHOT_HAS_FIELD(ev, ShotEvent, data_len)) {
        return -1;  // 老宿主的事件结构里没有 data_len，不猜
    }
    return state->window.post(ev->name, ev->data, ev->data_len);
}

const ShotUIProviderV1 kUiOps = SHOT_OPS(ShotUIProviderV1,
                                         .init           = &ui_init,
                                         .deinit         = &ui_deinit,
                                         .post_to_plugin = &ui_post);

const ShotIfaceEntry kIfaces[] = {
    { SHOT_IFACE_UI_PROVIDER, 1, &kUiOps },
    { nullptr, 0, nullptr },
};

}  // namespace

const PluginEntry kSettingWindow = {
    "setting_window.main", "Setting Window", "1.0.0", &create_state, &destroy_state, kIfaces,
};

}  // namespace setting_window
