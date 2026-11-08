#pragma once                                     // 防止头文件被重复包含
#include "core/Types.h"                          // 依赖 Candidate / KlineSeries / MarketRegime
#include "core/FactorTypes.h"                    // 多因子维度贡献度
#include <QString>                               // 文本字段
#include <QVector>                               // 计划列表
#include <QDate>                                 // 交易日

// 小资金集中持仓口径：仓位摊分与「仅推荐两只标的」共用同一只数，避免两处口径漂移
constexpr int kConcentratedPositionCount = 2;

// TradePlan：交易计划（买入区间 / 止盈区间 / 止损区间 / 仓位建议）
// 约束：纯计算、零 IO、零 GUI；每个价格都带「依据」；K 线缺失时退化为「涨停价 ± 百分比」
// 价格语义（元，2 位小数）：买入区间 [buyLow, buyHigh]、止盈 [tpLow, tpHigh]、止损 [slLow, slHigh]

struct TradePlan {
    QString code;                                // 股票代码
    QString name;                                // 股票名称
    double currentPrice = 0.0;                   // 最新价（涨停池 / 快照口径；0 = 暂无实时价）
    QString tag;        // 首板 / 二板 / 龙头
    QString sector;     // 行业（兜底口径）
    QString theme;      // 题材（主口径）
    int    themeRank = 0;       // 同题材身位 1=龙头
    bool   themeLeader = false; // 是否为同题材最高身位

    double buyLow = 0.0;                         // 买入价格下沿
    double buyHigh = 0.0;                        // 买入价格上沿
    double takeProfitLow = 0.0;                  // 止盈下沿（保守兑现价）
    double takeProfitHigh = 0.0;                 // 止盈上沿（乐观兑现价）
    double stopLossLow = 0.0;                    // 止损下沿（破位无条件走）
    double stopLossHigh = 0.0;                   // 止损上沿（跌破即减仓）

    double positionPct = 0.0;   // 建议单票仓位（占总资金 %）
    double riskReward = 0.0;    // 盈亏比（保守口径：止盈下沿兑现）
    double riskRewardMax = 0.0; // 盈亏比（乐观口径：次日涨停兑现，仅供对比）
    double riskPct = 0.0;       // 买入中枢到止损中枢的距离（%）
    bool   riskTooHigh = false; // 止损空间超过上限

    double fillProbability = 1.0;                // 0~1 的预估成交概率（打板常买不到）
    QString fillNote;           // 概率依据（封单/首封/开板/换手）

    double score = 0.0;                          // 原打板综合评分 0~100
    QString grade;                               // 评级 A/B/C
    ScoreBreakdown dabanBreakdown;               // 打板分由来（8 因子 + 修正项）

    // ---- 多维度做多因子选股结果（实盘与回测共用 MultiFactorEngine 产出）----
    double compositeScore = 0.0;   // 多维度综合评分 0~100
    int    factorRank = 0;         // 综合分排名（1 起，0 = 未参与排名）
    QVector<FactorContribution> contributions;  // 各维度贡献度分解（贡献之和 = 综合分）
    ScoreBreakdown compositeBreakdown;          // 综合分由来（维度明细 + 组合规则）
    QString factorReject;          // 被多因子过滤规则淘汰的原因
    QString factorSummary;         // 主导维度摘要

    bool   auctionEvaluated = false;   // 已拿到真实竞价数据（仅当日 9:25 后）
    int    auctionVerdict = 0;         // 0 待竞价 / 1 可接力 / 2 观察 / 3 放弃
    double auctionOpenPct = 0.0;       // 实际高开 %
    double auctionAmountRatio = 0.0;   // 竞价额 / 昨日成交额
    double auctionBuyLow = 0.0;        // 竞价可接价格下沿
    double auctionBuyHigh = 0.0;       // 竞价可接价格上沿（超过即放弃）
    double auctionAbandonPrice = 0.0;  // 跌破即放弃的价格
    QString auctionBasis;              // 规则说明
    QString auctionNote;               // 判定结论

    bool   actionable = true;   // true = 闸门放行可执行；false = 观察名单（今日不开仓）
    QString note;               // 状态说明

    // ---- 筹码分布与量能确认（定价的第二套锚点，可与上方三段区间互相印证）----
    double chipSupportLow = 0.0;    // 下方筹码支撑密集峰下沿（跌破 = 买入逻辑破坏）
    double chipSupportHigh = 0.0;   // 下方筹码支撑密集峰上沿（承接位 / 买入下沿锚点）
    double chipPressureLow = 0.0;   // 上方筹码压力密集峰下沿（兑现区起点）
    double chipPressureHigh = 0.0;  // 上方筹码压力密集峰上沿（现实目标位）
    double chipProfitRatio = 0.0;   // 获利盘比例（%，当前价以下筹码占比）
    double volRatio = 0.0;          // 当日量比（当日量 / 前 5 日均量）
    QString supportQuality;         // 承接质量结论（缩量衰竭 / 温和放量确认 / 放量击穿 / 筹码不足）
    QString divergenceNote;         // 量价背离等卖出预警说明

    int    signalLevel = 0;     // 情绪驱动信号：0=无 1=观察 2=试仓 3=建仓 4=主攻
    double riskBudgetPct = 0.0; // 单笔风险预算（占总资金 %）
    QString sentimentNote;      // 情绪状态摘要（复合指数 / 档位 / 有效上限）

    QString buyBasis;           // 买入依据
    QString tpBasis;            // 止盈依据
    QString slBasis;            // 止损依据
    QStringList riskFlags;      // 问题股 / 风险事件标记
    QString riskNote;           // 风险提示 / 淘汰原因

    double buyMid() const { return (buyLow + buyHigh) * 0.5; }             // 买入中枢价
    double tpMid() const { return (takeProfitLow + takeProfitHigh) * 0.5; } // 止盈中枢价
    double slMid() const { return (stopLossLow + stopLossHigh) * 0.5; }     // 止损中枢价
    bool valid() const { return !code.isEmpty() && buyHigh > 0.0; }         // 有代码且买入上沿有效才算可用

    QString reasonLine() const;   // 一行推荐理由（题材身位 · 板数 · 评级 · 档位）
    QString statusTag() const;    // 状态短标签（可执行 / 竞价否决 / 止损过远 …）
};

// 一次荐股输出的完整快照
struct RecommendBundle {
    QVector<TradePlan> plans;                    // 本次输出的交易计划列表（小资金仅前 2 只可执行）
    QVector<TradePlan> backups;                  // 候选池备选（第二梯队，仅观察，仅决策台展示）
    MarketRegime regime;                         // 当日市场情绪指标
    QDate tradeDate;                             // 结论所属交易日
    QString updatedAt;      // 数据时间戳
    bool cached = false;    // true = 来自本地缓存（离线兜底）
    QString notice;         // 顶部结论提示
    double positionCapPct = -1.0;                // 情绪引擎给出的有效总仓上限（%），<0 表示未启用

    QVector<FunnelStage> funnel;                 // 筛选漏斗各层通过数
    QVector<RejectRow> rejects;                  // 逐只淘汰明细（回答「47 只去哪了」）

    QString factorDigest;   // 本次评分所用因子配置指纹（复盘可追溯当时口径）
    QString factorRule;     // 组合规则与权重摘要（缓存展示用）
};

// 信号级别 → 中文名。UI 与邮件共用，避免两处口径不一：0 无 / 1 观察 / 2 试仓 / 3 建仓 / 4 主攻
QString signalLevelName(int level);              // 输入 TradePlan::signalLevel

// 小资金集中持仓口径的**唯一实现**：候选池里只保留综合分最高的 kConcentratedPositionCount
// 只「可执行」标的，其余一律降级为备选（actionable=false、仓位归零）。
// 决策台与候选池面板共用此函数，保证「推荐」名额在任何入口下都不会超过 2 只。
// 入参就地修改（调用方传自己的副本，不影响原始计划 / 推送 / 落库）。
void applyConcentratedPicks(QVector<TradePlan>& pool);

namespace Plan {                                 // 交易计划构建命名空间

double phasePositionCap(MarketPhase phase);      // 情绪阶段 → 总仓位上限（%）

// 计算单只标的的交易计划
//   c          —— 已通过筛选并评分的候选
//   kline      —— 该标的日 K（可为 nullptr，此时用保守退化估计）
//   regime     —— 当日市场情绪
//   actionable —— 是否可执行
TradePlan build(const Candidate& c, const KlineSeries* kline,
                const MarketRegime& regime, bool actionable);

} // namespace Plan
