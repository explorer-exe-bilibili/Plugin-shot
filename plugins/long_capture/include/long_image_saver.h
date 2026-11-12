#pragma once

// ============================================================================
//  include/long_image_saver.h —— 长图 PNG 落盘（平台无关核心）
//
//  写盘策略只有两条：
//    1. 目录：调用方从 config_list 里读 image_saver.main/directory（读不到就由
//       这里退回系统图片目录），文件名固定 shot_long_YYYYMMDD_HHMMSS.png；
//    2. 编码：stb_image_write 的 *_to_func 接口写进自己的 ofstream —— stb 的
//       按文件名接口在 Windows 上是 ANSI，中文目录会坏（与 image_saver 同一坑）。
//
//  本文件与实现只用标准库 + stb，不包含任何平台头（取系统图片目录是唯一的
//  #if 分支，与 image_saver/src/output_path.cpp 的既有做法一致）。
// ============================================================================

#include <filesystem>
#include <string>

#include "stitch_canvas.h"

namespace long_capture {

/**
 * @brief 一次保存的结果：成功与否、最终绝对路径、失败原因。
 */
struct SaveOutcome {
    bool ok = false;      /**< true = 文件已完整写下。 */
    std::string path;     /**< 成功时是 UTF-8 绝对路径；失败时为空。 */
    std::string error;    /**< 失败时的中文原因；成功时为空。 */
};

/**
 * @brief 长图保存器：把画布编码成 PNG 落盘。
 *
 * 无状态、任意线程可调（每次调用自己开文件）；不缓存目录、不持有任何句柄。
 */
class LongImageSaver {
public:
    /**
     * @brief 把画布写成一张 PNG。
     *
     * @param[in] canvas    已定稿的画布（width × height，RGBA 紧凑行）。
     * @param[in] directory 目标目录（UTF-8）；空串 = 系统图片目录。
     * @return 保存结果；失败时 error 是给用户看的中文原因（HUD/托盘气泡会展示）。
     * @note 目录不存在会逐级创建；同名文件已存在时追加 _1/_2… 后缀，绝不覆盖。
     */
    static SaveOutcome save(const StitchCanvas& canvas, const std::string& directory);

    /**
     * @brief 系统图片目录（Windows: %USERPROFILE%\Pictures；其它平台: $HOME/Pictures）。
     * @return UTF-8 绝对路径；环境变量缺失时退回进程当前目录。
     */
    static std::string defaultDirectory();

    /** @brief 生成本次保存的文件名：shot_long_YYYYMMDD_HHMMSS.png。 */
    static std::string makeFileName();

private:
    /**
     * @brief 取本地时间的紧凑时间戳（YYYYMMDD_HHMMSS）。
     * @return 14 位数字文本；取时间失败时退回 "19700101_000000"。
     */
    static std::string timestamp();

    /**
     * @brief 同名文件已存在时追加 _1/_2…（最多 1000 次）。
     * @param[in] target 期望路径。
     * @return 目录里当前不存在的路径；都占满时原样返回 target。
     */
    static std::filesystem::path makeUnique(const std::filesystem::path& target);

    /**
     * @brief 逐级创建父目录。
     * @param[in] filePath 目标文件路径。
     * @return true = 父目录已存在或创建成功。
     */
    static bool ensureParentDirectory(const std::filesystem::path& filePath);

    /**
     * @brief stb_image_write 的回调：把编码好的字节写进 ofstream。
     * @param[in] context std::ofstream*。
     * @param[in] data    字节地址。
     * @param[in] size    字节数。
     */
    static void writeToStream(void* context, void* data, int size);
};

}  // namespace long_capture
