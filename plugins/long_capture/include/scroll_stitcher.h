#pragma once

// ============================================================================
//  include/scroll_stitcher.h —— 滚动拼接（平台无关核心，位移估计可插拔）
//
//  输入是"上一帧保留的截图"和"刚抓到的新截图"，输出是带符号的行位移 d：
//    · d > 0：窗口内容整体上移（用户向下滚动），新帧底部 d 行是全新内容；
//    · d < 0：内容整体下移（用户向上滚动），新帧顶部 |d| 行是全新内容；
//    · 匹配不上 / 重叠太少：记为"缺口"，不做硬拼（宁可漏一段，也不要把
//      两段对不齐的内容糊在一起）。
//
//  职责划分（这一版把"怎么估位移"抽到了 stitch_estimator.h 的函数表）：
//    · 本类：画布坐标（m_refTop / m_canvasTop）、固定条检测与锁定、去重、
//      缺口判定，以及按方向追加/前插；
//    · IStitchEstimator 等价物（StitchEstimatorOps）：只看两帧签名给出 delta 与 score。
//  这样 hash / correlate / phase 三种判据可以独立演进，而拼接语义（坐标、固定条、
//  缺口）只有一份，不会因换算法而漂移。
//
//  本文件与实现只用标准库，不包含任何平台头。
// ============================================================================

#include <cstdint>
#include <vector>

#include "capture_frame.h"
#include "stitch_estimator.h"

namespace long_capture {

class StitchCanvas;

/**
 * @brief 一帧的拼接结论：位移、匹配分数、以及是否被当作重复/缺口。
 *
 * 三个互斥状态用 plain bool 而不是状态枚举：调用方按布尔语义分支就够了，状态之间
 * 本来也不会同时成立（只有"选哪种算法"才需要 StitchAlgorithm 枚举）。
 */
struct ScrollMatch {
    std::int32_t delta = 0;        /**< 带符号位移（像素）；仅在 matched 为 true 时有拼接意义。 */
    bool matched = false;          /**< true = 已按 delta 贴进画布。 */
    bool duplicate = false;        /**< true = 与上一保留帧几乎一致，已丢弃。 */
    bool heightLimited = false;    /**< true = 还要追加但画布已达高度上限，本帧没有贴。 */
    double score = 0.0;            /**< 最佳候选的匹配分数 0..1，日志排障用。 */
};

/**
 * @brief 滚动拼接器：保存上一保留帧的签名与缩略图，逐帧把新内容并进画布。
 *
 * 生命周期 = 一次长截图会话；第一帧到达前构造函数即可用，reset() 后重新以第一帧
 * 为基准。位移估计由构造时选择的算法函数表负责（见 stitch_estimator.h）。
 */
class ScrollStitcher {
public:
    /** @brief 去重缩略图的降采样倍数：先缩到 1/4 灰度再比，成本几乎为零。 */
    static constexpr int kThumbScale = 4;

    /** @brief 去重阈值：缩略图平均灰度差不超过它就算"几乎一致"，丢弃该帧。 */
    static constexpr double kDuplicateMeanDiff = 1.0;

    /**
     * @brief 构造：绑定一种位移估计算法。
     * @param[in] algorithm 位移估计算法（来自配置项 algorithm）。
     */
    explicit ScrollStitcher(StitchAlgorithm algorithm);

    /** @brief 不可拷贝：内部保存上一帧签名与缩略图。 */
    ScrollStitcher(const ScrollStitcher&) = delete;
    /** @brief 不可赋值。 */
    ScrollStitcher& operator=(const ScrollStitcher&) = delete;

    /**
     * @brief 丢弃上一帧基准，下一次 addFrame 重新从空画布开始。
     */
    void reset();

    /** @brief 当前位移估计算法（会话日志用）。 */
    StitchAlgorithm algorithm() const noexcept { return m_algorithm; }

    /** @brief 当前算法名（"hash"/"correlate"/"phase"，日志用）。 */
    const char* algorithmName() const noexcept { return m_estimator.name; }

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
     * @param[in]  frame 待处理帧。
     * @param[out] out   紧凑灰度缩略图（thumbHeight × thumbWidth）。
     * @param[out] w     缩略图宽度。
     * @param[out] h     缩略图高度。
     */
    static void buildThumb(const CaptureFrame& frame, std::vector<std::uint8_t>& out, int& w,
                           int& h);

    /**
     * @brief 判断新帧缩略图与上一保留帧是否几乎一致（去重）。
     * @param[in] thumb 新帧缩略图。
     * @return true = 平均灰度差低于 kDuplicateMeanDiff，丢弃这一帧。
     */
    bool looksIdentical(const std::vector<std::uint8_t>& thumb) const;

    /**
     * @brief 把当前帧记为新的"上一保留帧"（缩略图 + 签名）。
     * @param[in] frame 刚处理完的帧。
     * @param[in] thumb 该帧的缩略图（避免重复计算）。
     */
    void updateReference(const CaptureFrame& frame, const std::vector<std::uint8_t>& thumb);

    /**
     * @brief 把当前帧签名换成参考帧（交换两个缓冲，避免每帧重新分配几十 KB）。
     */
    void commitSignature() noexcept;

    StitchAlgorithm m_algorithm;             /**< 选中的位移估计算法。 */
    StitchEstimatorOps m_estimator;          /**< 对应函数表（早绑定，逐帧不查表分支）。 */
    StitchSignature m_referenceSignature;    /**< 上一保留帧签名（每帧预处理一次后缓存）。 */
    StitchSignature m_currentSignature;      /**< 当前帧签名；commit 时与参考帧交换。 */
    int m_width = 0;                         /**< 上一帧宽度；0 = 还没有基准。 */
    int m_height = 0;                        /**< 上一帧高度。 */
    std::vector<std::uint8_t> m_thumb;       /**< 上一帧灰度缩略图。 */
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
