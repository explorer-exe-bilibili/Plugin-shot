// 热键字符串解析：把配置里的 "Ctrl+Alt+S" 这类文本变成修饰键位 + 虚拟键码。
//
// 语法（大小写不敏感）：
//   [Ctrl+][Alt+][Shift+][Win+]<A-Z 或 0-9 或 F1..F12>
//
// 解析只做语法与键位校验，不碰任何系统 API：非法配置在注册路径统一按
// "warn + 跳过这一项"处理，所以解析器用返回值报告原因，绝不抛异常、也绝不崩。
// 本文件不 include <windows.h>（VK 数值见头文件说明），便于单独测试与将来移植。
//
// 匿名命名空间里都是只服务本文件的纯辅助函数（不属于任何类，STYLE.md §6.7）。

#include "hotkey_binding.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace hotkey {
namespace {

/** @brief Win32 虚拟键码：F1 = VK_F1 = 0x70，F12 = 0x7B。 */
constexpr std::uint32_t kVkF1 = 0x70u;
/** @brief F 键个数上限：语法只接受 F1..F12。 */
constexpr int kMaxFunctionKey = 12;

/** @brief 去掉两端空白；返回原字符串的子串视图（不分配内存）。 */
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/** @brief ASCII 小写化一个字符；非字母原样返回。 */
char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/** @brief ASCII 小写化整段文本（修饰键与主键都按小写比较）。 */
std::string asciiLowerCopy(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        result.push_back(asciiLower(c));
    }
    return result;
}

/**
 * @brief 解析主键：单个字母 A-Z、单个数字 0-9、或 F1..F12。
 * @param[in]  token      已经小写化的 token（如 "s" / "7" / "f12"）。
 * @param[out] virtualKey 成功时写入 Win32 VK 数值。
 * @param[out] label      成功时写入规范化显示名（"S" / "7" / "F12"）。
 * @return true = 是合法主键。
 */
bool parseMainKey(std::string_view token, std::uint32_t& virtualKey, std::string& label) {
    if (token.size() == 1) {
        const char c = token.front();
        if (c >= 'a' && c <= 'z') {
            virtualKey = static_cast<std::uint32_t>(c - 'a' + 'A');  // VK 与 ASCII 大写一致
            label.assign(1, static_cast<char>(c - 'a' + 'A'));
            return true;
        }
        if (c >= '0' && c <= '9') {
            virtualKey = static_cast<std::uint32_t>(c);  // VK_0..VK_9 = '0'..'9'
            label.assign(1, c);
            return true;
        }
        return false;
    }
    // F1..F12：长度 2..3，首字符 f，其余全是数字，数值必须在 1..12。
    if (token.size() < 2 || token.size() > 3 || token.front() != 'f') {
        return false;
    }
    int number = 0;
    for (std::size_t index = 1; index < token.size(); ++index) {
        const char c = token[index];
        if (c < '0' || c > '9') {
            return false;
        }
        number = number * 10 + (c - '0');
    }
    if (number < 1 || number > kMaxFunctionKey) {
        return false;
    }
    virtualKey = kVkF1 + static_cast<std::uint32_t>(number - 1);
    label = "F" + std::to_string(number);
    return true;
}

/** @brief 按固定顺序拼出规范化显示名："Ctrl+Alt+Shift+Win+主键"。 */
std::string buildLabel(std::uint32_t modifiers, const std::string& mainLabel) {
    std::string label;
    if ((modifiers & kModCtrl) != 0) {
        label += "Ctrl+";
    }
    if ((modifiers & kModAlt) != 0) {
        label += "Alt+";
    }
    if ((modifiers & kModShift) != 0) {
        label += "Shift+";
    }
    if ((modifiers & kModWin) != 0) {
        label += "Win+";
    }
    label += mainLabel;
    return label;
}

}  // namespace

bool parse_binding(const char* text, Binding& out, std::string& error) {
    out = Binding{};
    error.clear();
    if (text == nullptr) {
        error = "热键为空";
        return false;
    }
    const std::string_view input = trim(std::string_view(text));
    if (input.empty()) {
        error = "热键为空";
        return false;
    }

    std::uint32_t modifiers = 0;
    std::uint32_t virtualKey = 0;
    std::string mainLabel;
    bool haveMainKey = false;

    // 按 '+' 切分：修饰键可任意顺序、可重复；主键必须恰好一个。
    std::size_t start = 0;
    for (;;) {
        const std::size_t plus = input.find('+', start);
        const std::string_view token =
            trim(input.substr(start, plus == std::string_view::npos ? std::string_view::npos
                                                                    : plus - start));
        if (token.empty()) {
            error = "存在空的键位（多余的 +）";
            return false;
        }
        const std::string lower = asciiLowerCopy(token);
        if (lower == "ctrl" || lower == "control") {
            modifiers |= kModCtrl;
        } else if (lower == "alt") {
            modifiers |= kModAlt;
        } else if (lower == "shift") {
            modifiers |= kModShift;
        } else if (lower == "win" || lower == "windows") {
            modifiers |= kModWin;
        } else {
            std::uint32_t candidateKey = 0;
            std::string candidateLabel;
            if (!parseMainKey(lower, candidateKey, candidateLabel)) {
                error = "不认识的键位 \"" + std::string(token) + "\"";
                return false;
            }
            if (haveMainKey) {
                error = "只能有一个主键";
                return false;
            }
            virtualKey = candidateKey;
            mainLabel = std::move(candidateLabel);
            haveMainKey = true;
        }
        if (plus == std::string_view::npos) {
            break;
        }
        start = plus + 1;
    }

    if (!haveMainKey) {
        error = "缺少主键（A-Z / 0-9 / F1..F12）";
        return false;
    }
    out.modifiers = modifiers;
    out.virtualKey = virtualKey;
    out.label = buildLabel(modifiers, mainLabel);
    return true;
}

}  // namespace hotkey
