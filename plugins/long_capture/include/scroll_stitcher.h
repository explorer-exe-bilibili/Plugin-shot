#pragma once

// ============================================================================
//  include/scroll_stitcher.h —— 滚动位移估计（平台无关核心）
//
//  输入是"上一帧保留的截图"和"刚抓到的新截图"，输出是带符号的行位移 d：
//    · d > 0：窗口内容整体上移（用户向下滚动），新帧底部 d 行是全新内容；
//    · d < 0：内容整体下移（用户向上滚动），新帧顶部 |d| 行是全新内容；
//    · 匹配不上 / 重叠太少：记为"缺口"，不做硬拼（宁可漏一段，也不要把
//      两段对不齐的内容糊在一起）。
//
//  匹配手段是灰度行指纹：每行按固定列抽样成一条灰度向量，先粗搜（少量行）
//  再局部精搜（重叠区全部行），避免 O(H² × 全行) 的暴力比对。
//
//  本文件与实现只用标准库，不包含任何平台头。
// ============================================================================

#include <cstdint>
#include <vector>

#include "capture_frame.h"

namespace long_capture {

class StitchCanvas;

/**
 * @brief 一帧的拼接结论：位移、匹配分数、以及是否被当作重复/缺口。
 *
 * 字段用 plain bool 而不是枚举：本仓库约定插件侧不定义枚举（STYLE.md §3.2），
 * 调用方按布尔语义分支就够了。
 */
struct ScrollMatch {
    std::int32_t delta = 0;        /**< 带符号位移（像素）；仅在 matched 为 true 时有拼接意义。 */
    bool matched = false;          /**< true = 已按 delta 贴进画布。 */
    bool duplicate = false;        /**< true = 与上一保留帧几乎一致，已丢弃。 */
    bool heightLimited = false;    /**< true = 还要追加但画布已达高度上限，本帧没有贴。 */
    double score = 0.0;            /**< 最佳候选的行匹配分数 0..1，日志排障用。 */
};

/**
 * @brief 滚动位移估计器：保存"上一保留帧"的降采样缩略图与逐行指纹。
 *
 * 生命周期 = 一次长截图会话；reset() 后重新以第一帧为基准。
 */
class ScrollStitcher {
public:
    /** @brief 重叠不足的阈值：重叠行少于视口高度的 30% 按缺口处理（document/README 同步）。 */
    static constexpr int kMinOverlapPercent = 30;

    /** @brief 最佳候选的行匹配分数低于它就不拼（0.90 = 90% 的信息行要对上）。 */
    static constexpr double kMinMatchScore = 0.90;

    /** @brief 行指纹采样列数：每行在这些列上取 24 位颜色做 64 位哈希（越密越能区分相似行）。 */
    static constexpr int kRowSamples = 256;

    /** @brief 参与打分的"信息行"下限：太少说明重叠区全是空白/重复行，位移不可信。 */
    static constexpr int kMinInformativeRows = 64;

    /** @brief 去重缩略图的降采样倍数：先缩到 1/4 灰度再比，成本几乎为零。 */
    static constexpr int kThumbScale = 4;

    /** @brief 去重阈值：缩略图平均灰度差不超过它就算"几乎一致"，丢弃该帧。 */
    static constexpr double kDuplicateMeanDiff = 1.0;

    /** @brief 构造空估计器；第一帧到来前 addFrame 会把它当作画布起点。 */
    ScrollStitcher();

    /** @brief 不可拷贝：内部保存上一帧的指纹与缩略图。 */
    ScrollStitcher(const ScrollStitcher&) = delete;
    /** @brief 不可赋值。 */
    ScrollStitcher& operator=(const ScrollStitcher&) = delete;

    /**
     * @brief 丢弃上一帧基准，下一次 addFrame 重新从空画布开始。
     */
    void reset();

    /**
     * @brief 把一帧并入画布：首帧直接铺满，后续帧先求位移再按方向追加/前插。
     *
     * @param[in] frame  刚解码的一帧；调用方在返回后可以立刻 release()。
     * @param[in] canvas 目标画布；宽度必须与帧一致（调用方保证会话内尺寸不变）。
     * @return 本帧的结论；matched/duplicate/heightLimited 三种情况互斥（matched 优先）。
     * @note 缺口（既没匹配也不是重复）时仍会更新基准帧：否则连续快速滚动会让
     *       后面每一帧都拿"很久以前的内容"去比，永远恢复不了。
     */
    ScrollMatch addFrame(const CaptureFrame& frame, StitchCanvas& canvas);

private:
    /**
     * @brief 把一帧降采样成 1/4 灰度缩略图（去重用）。
     * @param[in] frame 待处理帧。
     * @param[out] out  紧凑灰度缩略图（thumbHeight × thumbWidth）。
     * @param[out] w    缩略图宽度。
     * @param[out] h    缩略图高度。
     */
    static void buildThumb(const CaptureFrame& frame, std::vector<std::uint8_t>& out, int& w,
                           int& h);

    /**
     * @brief 把一帧每行算成 64 位哈希（拼接用）。
     *
     * 为什么用哈希而不是"灰度差 <= 阈值"：会话层只在画面静止时送帧，同一行在两帧里
     * 是逐像素一致的，哈希相等是精确判据；原来 32 列灰度容差在文本/代码这类高频
     * 内容上会把"差一两行"也算成匹配，导致位移估小、拼出重复行。
     *
     * @param[in] frame 待处理帧。
     * @param[out] out  每行一个哈希。
     */
    static void buildRowHashes(const CaptureFrame& frame, std::vector<std::uint64_t>& out);

    /**
     * @brief 在新帧哈希与上一帧哈希之间搜索带符号位移。
     * @param[in]  hashes 新帧的行哈希。
     * @param[out] score  最佳候选的信息行匹配分数。
     * @param[out] informative 最佳候选参与打分的信息行数。
     * @return 最佳位移；没有可用候选时返回 0（与"完全没滚动"由 score 区分）。
     */
    std::int32_t findDelta(const std::vector<std::uint64_t>& hashes, int topStatic,
                           int bottomStatic, double& score, int& informative) const;

    /**
     * @brief 统计位移 d 下重叠区里"信息行"的匹配分数。
     *
     * 信息行 = 两边都不是高频重复行（空白行、分隔线、大面积纯色）。不过滤的话这些
     * 行在任意位移下都相等，会把错误位移刷到满分。
     *
     * @param[in]  hashes      新帧行哈希。
     * @param[in]  delta       带符号位移。
     * @param[in]  isFrequent  高频哈希判定（下标是哈希本身的值，用 lambda 传入）。
     * @param[out] informative 参与打分的信息行数。
     * @return 0..1 的匹配分数；没有信息行时返回 0。
     */
    template <typename FrequentPredicate>
    double scoreDelta(const std::vector<std::uint64_t>& hashes, std::int32_t delta, int topStatic,
                      int bottomStatic, FrequentPredicate isFrequent, int& informative) const;

    /**
     * @brief 判断新帧缩略图与上一保留帧是否几乎一致（去重）。
     * @param[in] thumb 新帧缩略图。
     * @return true = 平均灰度差低于 kDuplicateMeanDiff，丢弃这一帧。
     */
    bool looksIdentical(const std::vector<std::uint8_t>& thumb) const;

    /**
     * @brief 把当前帧记为新的"上一保留帧"（缩略图 + 行哈希）。
     * @param[in] frame  刚处理完的帧。
     * @param[in] thumb  该帧的缩略图（避免重复计算）。
     * @param[in] hashes 该帧的行哈希。
     */
    void updateReference(const CaptureFrame& frame, const std::vector<std::uint8_t>& thumb,
                         const std::vector<std::uint64_t>& hashes);

    int m_width = 0;                         /**< 上一帧宽度；0 = 还没有基准。 */
    int m_height = 0;                        /**< 上一帧高度。 */
    std::vector<std::uint8_t> m_thumb;       /**< 上一帧灰度缩略图。 */
    std::vector<std::uint64_t> m_rowHashes;  /**< 上一帧逐行哈希。 */
    /** 上一保留帧"移动区顶部"在内容坐标里的行号（内容坐标 0 = 首帧移动区第一行）。 */
    std::int64_t m_refTop = 0;
    /** 画布第 0 行对应的内容坐标行号；画布只存移动内容，固定条不进来。 */
    std::int64_t m_canvasTop = 0;
    /** 固定条是否已判定并裁掉（只在第一次出现非零位移时判定一次）。 */
    bool m_bandsLocked = false;
    /** 锁定的顶部固定条行数（0 = 没有/未判定）。 */
    int m_staticTop = 0;
    /** 锁定的底部固定条行数。 */
    int m_staticBottom = 0;
};

}  // namespace long_capture
