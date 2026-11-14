#pragma once

// ============================================================================
//  include/lib_provider.h —— 运行库提供者模板的"库身份"声明
//
//  shot.lib_provider 只回答两件事：这个包带进来的库叫什么、实际版本是什么。
//  名字必须与 metadata.json 的 provides[] 一致（宿主按它满足别的插件的
//  requires），版本只在运行期报告（provides 里刻意不写版本：vcpkg/库升级时
//  只需要改这里一处，metadata 不用动）。
//
//  真正带库的插件（如 OpenCV provider）应该：
//    1) 把下面两个常量换成真实库名 / 从库的版本宏取版本（不要手写会漂移的字面量）；
//    2) 在 CMakeLists 的 shot_add_plugin() 里加 LINK_LIBRARIES；
//    3) 确保库的 DLL 随宿主一起发布（不要塞进 plugins/，那是插件目录）。
// ============================================================================

namespace lib_provider_template {

/** @brief 本包带进进程的库名；必须与 metadata.json 的 provides[] 一致。 */
inline constexpr const char* kLibraryName = "acme.mocklib";

/** @brief 库的实际版本（这里写死演示；真实插件应取自库的版本头/宏）。 */
inline constexpr const char* kLibraryVersion = "1.0.0";

}  // namespace lib_provider_template
