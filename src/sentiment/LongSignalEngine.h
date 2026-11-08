#pragma once                                     // 防止头文件被重复包含

#include "core/Types.h"                          // 依赖 Candidate
#include "core/TradePlan.h"                      // 依赖 TradePlan / RecommendBundle
#include "sentiment/SentimentTypes.h"            // 依赖 SentimentState / LongSignal
#include <QHash>                                 // 代码 → 日 K
#include <QVector>                               // 候选与信号列表

// LongSignalEngine：情绪驱动的做多信号生成 + 仓位控制（只做多）
// 信号链（全满足才升级，任一不满足降级为观察）：E1 情绪门 / E2 高度门 / E3 身位门 /
// E4 评分门 / E5 盈亏门 / E6 风险门 / E7 结构门 / E8 成交门
// 仓位 = min(风险预算/止损空间, 总仓上限/持仓数, 单票上限) × 档位系数 × 龙头加成
// 约束：高潮期只做龙头只减不加；禁止开仓时全部降级为观察；竞价否决不可因情绪好转复活

class LongSignalEngine
{
public:
    // 生成信号（不修改计划）；candidates 建议传「闸门拦截前的完整名单」，禁止开仓时也能给观察信号
    static QVector<LongSignal> generate(const QVector<Candidate>& candidates,   // 候选
                                        const QHash<QString, KlineSeries>& klines,  // 日 K
                                        const SentimentState& state);        // 情绪状态

    // 把信号写回计划（就地覆写 actionable / positionPct / note / signalLevel），并追加情绪结论
    // 注意：参数名不可用 signals —— Qt 的 signals 是展开为 public 的宏
    static void apply(RecommendBundle& bundle,                // 待覆写的计划集合
                      const QVector<LongSignal>& signalList,  // 已生成的信号
                      const SentimentState& state);           // 情绪状态

    // 单票仓位（%，占总资金）。公开以便自检与敏感性分析复用
    static double positionFor(double totalCapPct, double riskPct, bool leader, SignalAction action);

    static QString actionName(SignalAction a) { return signalActionName(a); }  // 动作中文名（转发）
};
