#pragma once

// ============================================================================
//  include/hotkey_binding.h —— 热键字符串解析（不依赖 <windows.h>）
//
//  语法（大小写不敏感）：
//      [Ctrl+][Alt+][Shift+][Win+]<A-Z 或 0-9 或 F1..F12 或 PrintScreen>
//  修饰键可省略、顺序任意、重复出现按一次算；主键必须恰好一个。
//
//  解析结果里的 virtualKey 用 Win32 的 VK_* 数值（A-Z / 0-9 就是 ASCII，
//  F1..F12 = 0x70..0x7B，PrintScreen = 0x2C）：本仓库只支持 Windows（CMake 里非 Windows 直接跳过），
//  但解析逻辑本身不 include <windows.h>，方便单独测试、也方便将来移植到
//  macOS RegisterEventHotKey / Linux XGrabKey 时在各平台做一次 VK 到本地键码映射。
// ============================================================================

#include <cstdint>
#include <string>

namespace hotkey {

/**
 * @brief 修饰键位掩码；与 Win32 的 MOD_* 是两套值，由窗口实现负责转换。
 *
 * 用 constexpr 常量而不是枚举：本仓库约定"枚举不进插件侧公共头"
 * （STYLE.md §3.2），位掩码本来也是无符号整数。
 */
constexpr std::uint32_t kModCtrl = 1u << 0;  /**< Ctrl（Control 也接受） */
constexpr std::uint32_t kModAlt = 1u << 1;   /**< Alt */
constexpr std::uint32_t kModShift = 1u << 2; /**< Shift */
constexpr std::uint32_t kModWin = 1u << 3;   /**< Win（Windows 也接受） */

/**
 * @brief 一个解析后的热键：修饰键位集合 + 主键虚拟键码。
 */
struct Binding {
    std::uint32_t modifiers = 0;  /**< 上面 kMod* 的按位或；0 = 没有修饰键。 */
    std::uint32_t virtualKey = 0; /**< Win32 VK 数值；0 = 非法（解析失败时保持 0）。 */
    std::string label;            /**< 规范化显示名，如 "Ctrl+Alt+S" / "F9"；日志与 README 用。 */
};

/**
 * @brief 把配置里的热键字符串解析成 Binding。
 *
 * 纯函数、无状态、任意线程可调；失败时返回 false 并写入中文原因，调用方
 * （热键注册路径）按"非法 = warn + 跳过这一项"处理，不影响其它热键。
 *
 * @param[in]  text  配置值，如 "Ctrl+Alt+S"；nullptr / 空串视为非法。
 * @param[out] out   解析成功时写入；失败时不保证内容有意义（仍会被清空）。
 * @param[out] error 失败时的中文原因；成功时清空。
 * @return true = 解析成功；false = 语法错误（缺少主键、未知键位等）。
 * @note 修饰键写法：Ctrl / Control、Alt、Shift、Win / Windows；主键：A-Z、0-9、F1..F12、
 *       PrintScreen（也接受 Print Screen / PrtSc / PrtScn 拼写，统一规范化成 "PrintScreen"）。
 * @note 命名按 STYLE.md §3.1："插件侧自由函数"用 snake_case（C++ 类方法才用 camelCase）。
 */
bool parse_binding(const char* text, Binding& out, std::string& error);

}  // namespace hotkey
