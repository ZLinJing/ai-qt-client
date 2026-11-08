#pragma once                                     // 防止头文件被重复包含

#include "core/Types.h"                          // 依赖 LimitEntry / Snapshot / QDate
#include "sentiment/SentimentTypes.h"            // 依赖 SentimentFact / SentimentState
#include <QDate>                                 // 交易日
#include <QHash>                                 // 快照/题材索引
#include <QStringList>                           // 题材列表
#include <QVector>                               // 历史序列

// SentimentEngine：情绪维度的「采集 → 量化 → 判定」引擎（纯函数，无 IO）
// collect()  把既有数据源折算成 SentimentFact（不新增网络接口）
// evaluate() 六维量化 → 加权复合指数 → 阶段/档位/仓位上限，并同步产出兼容 MarketRegime
// 六维：S1 赚钱效应 0.30 / S2 封板质量 0.20 / S3 空间高度 0.15 / S4 市场宽度 0.15
//       S5 分歧收敛 0.10 / S6 主线集中度 0.10
// 所有分项用「上下界线性归一」而非 z-score —— 情绪是绝对量，滚动标准化会在冰点期失真

class SentimentEngine
{
public:
    static SentimentFact collect(const QDate& date,                  // 交易日
                                  const QVector<LimitEntry>& limitUp,   // 当日涨停池
                                  const QVector<LimitEntry>& broken,    // 当日炸板池
                                  const QVector<LimitEntry>& limitDown, // 当日跌停池
                                  const QVector<LimitEntry>& prevLimitUp,  // 昨日涨停池（先行指标分母）
                                  const QHash<QString, Snapshot>& quotes,  // 全市场快照
                                  const QHash<QString, QStringList>& themesOfStock);  // 代码→题材

    static SentimentState evaluate(const SentimentFact& fact,        // 原始事实
                                    const QVector<QPair<QDate, double>>& history = {});  // 历史指数（升序）

    static SentimentState run(const QDate& date,                     // 一步到位：collect + evaluate
                               const QVector<LimitEntry>& limitUp,
                               const QVector<LimitEntry>& broken,
                               const QVector<LimitEntry>& limitDown,
                               const QVector<LimitEntry>& prevLimitUp,
                               const QHash<QString, Snapshot>& quotes,
                               const QHash<QString, QStringList>& themesOfStock,
                               const QVector<QPair<QDate, double>>& history = {});

    static int maxBoardForPhase(MarketPhase phase);   // 阶段允许参与的最大板数

    // 情绪域收窄：只排除「结构性不可参与」（板数窗口）；是否开仓由 tradeAllowed 控制
    static bool passUniverse(const LimitEntry& entry, const SentimentState& state,
                             QString& reason);

    static QString tierName(SentimentTier t) { return sentimentTierName(t); }  // 档位中文名（转发）
    static QString factorSummary(const SentimentState& s);   // 六维明细多行文本
    static QString phaseAdvice(const SentimentState& s);     // 阶段建议一行文本

    // 情绪序列回看天数（趋势 / 加速度 / 退潮确认）
    static constexpr int kHistoryDays = 10;

private:
    static void buildRegime(SentimentState& s);      // 生成兼容的 MarketRegime
    static void buildNote(SentimentState& s);        // 生成一行结论
};
