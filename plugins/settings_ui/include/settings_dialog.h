#pragma once

// ============================================================================
//  settings_dialog.h —— 内部头：原生文件/目录选择对话框（nativefiledialog-extended）
//
//  **不是插件对外接口**：只有本插件的 .cpp 包含它。把"弹系统文件对话框"从设置页
//  模型（settings_form.cpp）里拆出来，平台差异与对话框库生命周期都收在这一个模块里；
//  也正因为如此，设置页不需要知道窗口句柄。
//
//  设计约定：
//    · 全部函数只在 UI 线程上调用（与本插件的其它 UI 代码一致，不加锁）；
//    · 对话框不可用时返回 false 并给出 error，由调用方降级成"手输路径 + 提示"；
//    · 路径一律 UTF-8（nativefiledialog-extended 的 nfdu8char_t 版本）。
// ============================================================================

#include <string>

namespace settings_ui {

/**
 * @brief 弹"打开文件"对话框让用户挑一个已存在的文件。
 *
 * @param[in,out] path  进 = 当前值（只为选目录/文件类型判断用，不强制作为初始位置）；
 *                      出 = 用户选中的路径（UTF-8），仅在返回 true 时修改。
 * @param[out]    error 失败且对话框库本身不可用时写入中文原因；用户取消时清空。
 * @return true = 用户选了文件；false = 取消或对话框不可用（看 error 是否为空区分）。
 */
bool pickExistingFile(std::string& path, std::string& error);

/**
 * @brief 弹"选择文件夹"对话框让用户挑一个已存在的目录。
 *
 * @param[in,out] path  进 = 当前值；出 = 用户选中的路径（UTF-8），仅在返回 true 时修改。
 * @param[out]    error 失败且对话框库本身不可用时写入中文原因；用户取消时清空。
 * @return true = 用户选了目录；false = 取消或对话框不可用（看 error 是否为空区分）。
 */
bool pickExistingFolder(std::string& path, std::string& error);

/**
 * @brief 关闭对话框库（如果用到过）；由 SettingsWindow 在 UI 线程收尾时调用。
 *
 * 幂等；没用过对话框时是空操作。
 */
void shutdownFileDialogs();

}  // namespace settings_ui
