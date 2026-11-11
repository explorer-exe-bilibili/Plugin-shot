// settings_panel —— SettingsPanel 的公开接口入口：构造/析构、公共接口转发，
// 以及 render() 的顶层窗口框架（操作条 + 滚动内容区）。
//
// 具体实现按职责拆在：
//   settings_model.cpp        —— config_list JSON 解析、模型刷新与纯值工具
//   settings_form.cpp         —— 操作条 + 配置节 + 控件渲染
//   settings_hotkey.cpp       —— 热键组合录入控件（模态弹窗 + suspend/resume）
//   settings_plugin_table.cpp —— 插件管理表渲染
//   settings_apply.cpp        —— 应用状态机 / 事件 / 超时
// 这个文件只留"进门"的那一层，避免又长回一个上帝类（STYLE.md §6.8）。

#include "settings_panel.h"

#include <memory>

#include "settings_panel_impl.h"

namespace settings_ui {

void SettingsPanel::Impl::render() {
    checkWaitTimeout();

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(displaySize);
    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##settings_page", nullptr, windowFlags)) {
        // 热键录入弹窗必须每帧都画：它可能因为 lists.changed / plugins.changed 的刷新
        // 脱离配置行的渲染路径，但录入会话（以及 hotkey 插件的暂停状态）必须始终可取消。
        renderHotkeyCapturePopup();
        renderActionBar();
        ImGui::Separator();
        // 内容区单独一个子窗口：操作条固定在上面，只有配置节跟着滚。
        if (ImGui::BeginChild("##settings_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
            renderConfigSections();
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

SettingsPanel::SettingsPanel() : m_impl(std::make_unique<Impl>()) {}

SettingsPanel::~SettingsPanel() = default;

void SettingsPanel::refresh(const ShotHostServices* host) {
    if (!m_impl) {
        return;
    }
    m_impl->host = host;
    m_impl->refresh();
}

void SettingsPanel::setI18n(const shot::I18n* i18n) {
    if (!m_impl) {
        return;
    }
    m_impl->i18n = i18n;
}

void SettingsPanel::render() {
    if (!m_impl) {
        return;
    }
    m_impl->render();
}

void SettingsPanel::apply() {
    if (!m_impl) {
        return;
    }
    m_impl->apply();
}

void SettingsPanel::cancel() {
    if (!m_impl) {
        return;
    }
    m_impl->cancel();
}

void SettingsPanel::restoreDefaults() {
    if (!m_impl) {
        return;
    }
    m_impl->restoreDefaults();
}

void SettingsPanel::cancelHotkeyCapture() {
    if (!m_impl) {
        return;
    }
    m_impl->endHotkeyCapture();
}

bool SettingsPanel::isCapturingHotkey() const {
    return m_impl && m_impl->hotkeyCapture.active;
}

void SettingsPanel::onConfigApplied() {
    if (!m_impl) {
        return;
    }
    m_impl->onConfigApplied();
}

void SettingsPanel::onPluginsChanged(std::string_view payload) {
    if (!m_impl) {
        return;
    }
    m_impl->onPluginsChanged(payload);
}

void SettingsPanel::onListsChanged(std::string_view payload) {
    if (!m_impl) {
        return;
    }
    m_impl->onListsChanged(payload);
}

void SettingsPanel::onListsUpdated(std::string_view payload) {
    if (!m_impl) {
        return;
    }
    m_impl->onListsUpdated(payload);
}

}  // namespace settings_ui
