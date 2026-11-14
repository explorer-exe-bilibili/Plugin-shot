#pragma once

// ============================================================================
//  include/pluginTestChecks.h —— 内部头，不属于对外接口
//
//  host self-test（--test-plugin）的检查实现按职责拆成两个 TU：
//    · src/pluginTestChecks.cpp      —— 加载状态、metadata 声明、配置、列表、事件；
//    · src/pluginTestCapabilities.cpp—— 四类具名能力的实际调用检查。
//  这里只声明两边共用的入口；调用方（pluginTestChecks.cpp）把每个已加载实例连同
//  隔离目录传进来。不要把这个头暴露给宿主其它模块。
// ============================================================================

#include <memory>
#include <string>
#include <vector>

#include "pluginTest.h"

namespace Plugin {
class PluginBase;
}  // namespace Plugin

namespace Host {

/**
 * @brief 内部：按实例的真实类型跑能力检查（image_processor / image_saver /
 *        screen_capture / lib_provider；UIProvider 与 Generic 只报 SKIP）。
 *
 * 为什么按实例类型而不是 metadata 声明：能力是运行期事实（query_interface 决定），
 * metadata 只是声明；两者不一致由 pluginTestChecks.cpp 的 metadata 检查单独判 FAIL。
 *
 * @param[in]     instance 已加载插件实例包装；不能为空。
 * @param[in]     workDir  隔离数据目录，保存类检查在这里落临时文件。
 * @param[in,out] checks   追加检查结果。
 * @note 只在宿主 main 线程调用；能力回调里的 C++ 异常在这里就地兜底成 FAIL。
 */
void runCapabilityChecks(const std::shared_ptr<Plugin::PluginBase>& instance,
                         const std::string& workDir, std::vector<PluginTestCheck>& checks);

}  // namespace Host
