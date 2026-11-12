// 没有全局热键能力平台的 HotkeyWindow 降级实现（当前只有 macOS 走这里）。
//
// Linux 有独立的 X11 实现（hotkey_window_x11.cpp 用 XGrabKey）；纯 Wayland 会话
// 里那份实现在运行期拿不到 Display，会按本文件同样的方式降级。Wayland 没有进程
// 可用的全局热键 API，所以这里不假装注册成功，也不让插件失败：hotkey_list 的贡献、
// 动态配置项、i18n 都和 Windows 一样工作，只是按下组合键不会有任何反应。启动时打
// 一条 WARN，把用户指向 session_control 插件的 D-Bus / 桌面自定义快捷键入口。
//
// 为什么单独一个 TU：Windows 的 Impl 持有 HWND/HINSTANCE、X11 的 Impl 持有 Display，
// 没有热键能力的平台连这些都不该有；用同一份公共头 + 各平台实现，插件入口与配置
// 代码保持平台无关（CMakeLists 按平台选源文件）。

#include "hotkey_window.h"

#include "hotkey_config.h"
#include "shot_plugin_helpers.h"

namespace hotkey {

/** @brief 降级实现没有平台资源需要保存；空结构只为满足 PIMPL 的完整类型要求。 */
struct HotkeyWindow::Impl {};

HotkeyWindow::HotkeyWindow() : m_impl(std::make_unique<Impl>()) {}

HotkeyWindow::~HotkeyWindow() = default;

bool HotkeyWindow::start(const ShotHostServices* host, HotkeyConfig* config,
                         HotkeyCatalog* catalog) {
    (void)config;
    (void)catalog;
    // 每次 start（含禁用→启用）都提示一次：用户看到配置项生效但按键没反应时，
    // 日志里必须能立刻找到原因。
    shot::Logger(host).warn(
        "[hotkey] 当前平台不支持全局热键（Wayland 没有该 API），"
        "请用 session_control 的 D-Bus 入口或桌面自定义快捷键");
    return true;
}

void HotkeyWindow::stop() {}

void HotkeyWindow::requestReapply() {}

std::int32_t HotkeyWindow::post(const char* name, const char* data, std::uint32_t len) {
    (void)data;
    (void)len;
    if (name == nullptr) {
        return -1;
    }
    // host.shutdown 在 Windows 实现里会转成 stop()；这里没有线程，直接成功。
    return 0;
}

}  // namespace hotkey
