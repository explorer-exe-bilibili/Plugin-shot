#pragma once

// ============================================================================
//  include/long_capture_i18n.h —— 本插件的中英文语言表（内部头）
//
//  语言表数据量不小（每语言二十多条），放在 long_capture.cpp 里会把生命周期/事件
//  编排的文件撑过 500 行（STYLE.md §6.8），所以单独一个 TU。只有 LongCapture::start()
//  需要它，不属于对外接口。
// ============================================================================

#include <cstddef>

namespace long_capture {

/**
 * @brief 取本插件的语言表数组。
 *
 * @param[out] count 语言表张数（当前 2：zh-CN / en-US）。
 * @return 语言表数组首地址；生命周期 = 整个 DLL，调用方不释放。
 * @note 每张表是一行 JSON，`@meta` 必填；两张表的 key 集必须完全一致。
 */
const char* const* languageTables(std::size_t& count) noexcept;

}  // namespace long_capture
