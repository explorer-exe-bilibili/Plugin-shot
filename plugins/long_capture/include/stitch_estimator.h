#pragma once

// ============================================================================
//  include/stitch_estimator.h —— 滚动位移估计的公共契约（平台无关核心）
//
//  为什么把"位移估计"从 ScrollStitcher 里拆出来：逐行哈希精确比较在真实代码
//  编辑器上会被相似行/重复内容带偏（选错位移 → 成段重复），而不同内容适合不同
//  判据。这里把三种判据做成同一张函数表，ScrollStitcher 只按配置分派：
//
//    hash      —— 逐行 64 位哈希 + 高频行过滤 + 带符号全量搜索（旧基线，保留对照）；
//    correlate —— 64 列块均值矩阵上的 2D 归一化互相关（NCC），粗搜 + 精搜（默认）；
//    phase     —— 逐行全宽平均亮度的 1D 相位相关（FFT 求互功率谱峰值）。
//
//  三种算法都把最佳候选归一化成 0..1 的 score（各自归一化方式见各实现文件），
//  ScrollStitcher 只拿 kMinMatchScore 做统一门槛。固定条锁定、画布坐标、去重与
//  缺口判定留在 ScrollStitcher，保证三种算法的拼接语义完全一致。
//
//  本文件与实现只用标准库，不包含任何平台头。
// ============================================================================

#include <cstdint>
#include <vector>

#include "capture_frame.h"

namespace long_capture {

/**
 * @brief 位移估计算法（插件内部枚举，不是 ABI 枚举）。
 *
 * 配置项以字符串暴露（long_capture_config 的 algorithm，SHOT_CONFIG_KIND_ENUM），
 * 这里只是解析后的内部分派键；增删算法不需要动契约头。
 */
enum class StitchAlgorithm {
    Hash,       /**< 逐行哈希 + 高频行过滤（旧基线）。 */
    Correlate,  /**< 64 列块均值 + 2D NCC（默认）。 */
    Phase,      /**< 逐行平均亮度 + 1D 相位相关。 */
};

/** @brief 重叠不足移动区高度的 30% 就不参与搜索：重叠太小，任何判据都不可信。 */
inline constexpr int kMinOverlapPercent = 30;

/**
 * @brief 匹配门槛：三种算法都把最佳候选归一化成 0..1 的 score，>= 它才按匹配拼接。
 *
 * 沿用旧版 0.90 的思路；各算法内部归一化方式不同（hash 是信息行相等比例、NCC 是
 * 相关系数、phase 用重叠区相关系数打分），但含义统一为"重叠区里绝大多数样本都
 * 对上了"，所以同一个门槛对三者都成立。
 */
inline constexpr double kMinMatchScore = 0.90;

/** @brief 参与打分的样本行下限：太少说明重叠区全是空白/重复内容，位移不可信。 */
inline constexpr int kMinInformativeRows = 64;

/** @brief 行哈希的采样列数：每行在这些等距列上取 24 位颜色（越密越能区分相似行）。 */
inline constexpr int kRowSamples = 256;

/**
 * @brief 一帧的预处理结果（每帧只构建一次，估计器缓存"参考帧"这一份）。
 *
 * 只构建被选中算法需要的表示，避免每帧把三种降采样都算一遍：
 *   · rowHashes —— 所有算法共用：固定条判定要比"同一行号是否逐像素相同"，
 *                  hash 算法也直接用它；由 ScrollStitcher 统一构建一次；
 *   · blockMeans / blockMeansCoarse —— correlate：64 列块均值矩阵，外加一份
 *                  垂直 4 行合并的粗粒度矩阵（粗搜用，见 stitch_correlate.cpp）；
 *   · rowMeans —— phase：每行全宽平均亮度。
 */
struct StitchSignature {
    int height = 0;                              /**< 帧高（行数）；估计时两帧必须相等。 */
    std::vector<std::uint64_t> rowHashes;        /**< 每行一个 64 位哈希（共用）。 */
    std::vector<std::uint8_t> blockMeans;        /**< correlate：height × 64 块均值，行优先。 */
    std::vector<std::uint8_t> blockMeansCoarse;  /**< correlate：粗粒度块均值（4 行合并）。 */
    std::vector<float> rowMeans;                 /**< phase：每行全宽平均亮度（0..255）。 */
};

/** @brief 位移估计结果：带符号位移、归一化分数、参与打分的样本行数。 */
struct StitchEstimate {
    std::int32_t delta = 0;  /**< 带符号位移：>0 内容上移（向下滚动），<0 内容下移。 */
    double score = 0.0;      /**< 0..1 的匹配分数，算法内部归一化。 */
    int informative = 0;     /**< 参与打分的重叠行数（缺口判据用）。 */
};

/**
 * @brief 预处理一帧：只填算法专属表示（rowHashes 已由调用方填好）。
 * @param[in]  frame     待处理帧。
 * @param[out] signature 帧签名；实现按算法 resize/fill 自己的向量。
 */
using StitchPrepareFn = void (*)(const CaptureFrame& frame, StitchSignature& signature);

/**
 * @brief 估计当前帧相对参考帧的带符号位移。
 * @param[in] reference    参考帧签名（上一保留帧）。
 * @param[in] current      当前帧签名；height 必须与 reference 相同。
 * @param[in] topStatic    锁定/检测出的顶部固定条行数（不参与打分）。
 * @param[in] bottomStatic 底部固定条行数。
 * @return 最佳位移与归一化分数；没有可用候选时 score 为 0。
 */
using StitchEstimateFn = StitchEstimate (*)(const StitchSignature& reference,
                                            const StitchSignature& current, int topStatic,
                                            int bottomStatic);

/** @brief 一种位移估计算法：日志名 + 预处理 + 估计（函数表分派，不引入对象/虚函数）。 */
struct StitchEstimatorOps {
    const char* name = "";               /**< 日志里的算法名（hash/correlate/phase）。 */
    StitchPrepareFn prepare = nullptr;   /**< 帧预处理（每帧一次）。 */
    StitchEstimateFn estimate = nullptr; /**< 位移估计。 */
};

/**
 * @brief 构建逐行哈希（所有算法共用：固定条判定 + hash 算法）。
 * @param[in]  frame 待处理帧。
 * @param[out] out   每行一个哈希；函数内部按 frame.height resize。
 */
void buildRowHashes(const CaptureFrame& frame, std::vector<std::uint64_t>& out);

/**
 * @brief 允许的最大 |位移|：移动区高度减掉至少 30% 的重叠。
 * @param[in] movingHeight 去掉固定条后的移动区行数。
 * @return 最大位移；视口太矮凑不出重叠时返回 0（调用方按缺口处理）。
 */
int maxStitchShift(int movingHeight) noexcept;

/** @brief hash 算法的函数表（实现在 stitch_hash.cpp）。 */
StitchEstimatorOps hashEstimatorOps() noexcept;
/** @brief correlate 算法的函数表（实现在 stitch_correlate.cpp）。 */
StitchEstimatorOps correlateEstimatorOps() noexcept;
/** @brief phase 算法的函数表（实现在 stitch_phase.cpp）。 */
StitchEstimatorOps phaseEstimatorOps() noexcept;

/**
 * @brief 按枚举取算法函数表。
 * @param[in] algorithm 算法枚举。
 * @return 对应函数表；未知值回落到 correlate（默认算法）。
 */
StitchEstimatorOps estimatorOps(StitchAlgorithm algorithm) noexcept;

/**
 * @brief 算法名（日志与配置值共用同一拼写）。
 * @param[in] algorithm 算法枚举。
 * @return "hash" / "correlate" / "phase"；未知值返回 "correlate"。
 */
const char* stitchAlgorithmName(StitchAlgorithm algorithm) noexcept;

/**
 * @brief 解析配置里的算法名。
 *
 * 只接受描述符 options 里的三个值；另外接受 "auto"（多算法融合的预留值，当前
 * 回落到 correlate，不写进 options，用户暂时看不到）。
 *
 * @param[in]  text 配置值；nullptr 返回 false。
 * @param[out] out  解析结果。
 * @return true = 已识别；false = 未知值（调用方保留原值）。
 */
bool parseStitchAlgorithm(const char* text, StitchAlgorithm& out) noexcept;

}  // namespace long_capture
