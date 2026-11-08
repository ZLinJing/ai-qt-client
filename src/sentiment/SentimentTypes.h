#pragma once                                     // 防止头文件被重复包含

#include "core/Types.h"                          // 依赖 MarketPhase / MarketRegime
#include <QDate>                                 // 交易日
#include <QString>                               // 文本字段
#include <QStringList>                           // 否决原因/条件列表
#include <QVector>                               // 分项与历史序列
#include <QPair>                                 // 历史点 (日期, 指数)

// 情绪维度领域模型
// SentimentFact 原始事实 → SentimentSubScore 六维分项 → SentimentState 复合状态 → LongSignal 做多信号
// 约定：值语义、无 IO/GUI 依赖、阶段复用既有 MarketPhase 保证下游零改动

// 情绪档位：内部仓位计算维度（对外不再展示「阶段·档位」复合标签）
enum class SentimentTier {
    NoTrade = 0,   // 禁止开仓（冰点 / 退潮 / 硬否决）
    Probe,         // 低强度档位
    Build,         // 中强度档位
    Attack,        // 高强度档位
    Harvest,       // 兑现（高潮期：只做龙头、只减不加）
};

QString sentimentTierName(SentimentTier t);      // 档位中文名（仅内部/诊断使用）
QString sentimentTierAdvice(SentimentTier t);    // 档位操作建议

// 对外统一状态标签：只用可核对的数字口径（复合指数 / 仓位上限 / 闸门），
// 不再拼接阶段名与档位名。UI / 通知 / 复盘 / 自检预览共用，避免各处口径不一。
QString sentimentStatusLabel(double composite, double capPct, bool tradeAllowed);

// 做多动作（与档位对应，但用于单只标的的最终裁决）
enum class SignalAction {
    None = 0,   // 无信号（情绪/规则硬性不满足）
    Watch,      // 观察（条件未齐备，不允许开仓）
    Probe,      // 试仓信号
    Build,      // 建仓信号
    Attack,     // 主攻信号
};

QString signalActionName(SignalAction a);        // 动作中文名

// 原始事实：全部由既有数据源统计而来，不新增网络请求
struct SentimentFact {
    QDate date;                                  // 事实所属交易日

    int limitUpCount = 0;                        // 涨停家数
    int brokenCount = 0;                         // 炸板家数
    int limitDownCount = 0;                      // 跌停家数
    int firstBoardCount = 0;                     // 首板家数
    int multiBoardCount = 0;                     // 连板家数
    int highBoardCount = 0;     // ≥3 板家数    高位板家数
    int maxBoardHeight = 0;                      // 最高连板高度

    int oneWordCount = 0;       // 真一字板家数
    int secondSealCount = 0;    // 秒板 / T 字板家数
    int sealBefore1030 = 0;     // 10:30 前首封家数
    int sealAfter1400 = 0;      // 14:00 后首封家数
    double avgSealToAmount = 0.0;   // 平均 封单额/成交额
    double avgSealToCap = 0.0;      // 平均 封单额/流通市值
    double avgTurnover = 0.0;       // 涨停股平均换手率 %
    double avgBreakCount = 0.0;     // 涨停股平均开板次数

    double brokenRate = 0.0;            // 炸板率
    double advanceRate = 0.0;           // 连板晋级率
    double firstBoardAdvanceRate = 0.0; // 首板晋级率
    double premiumPct = 0.0;            // 昨日涨停股今日平均涨幅
    int    prevLimitUpCount = 0;        // 昨日涨停家数（溢价采样基数）
    int    premiumSamples = 0;          // 溢价实际采样数
    bool   premiumKnown = false;        // 采样覆盖 ≥50% 才可信

    int    upCount = 0;                         // 全市场上涨家数
    int    downCount = 0;                       // 全市场下跌家数
    int    flatCount = 0;                       // 全市场平盘家数
    int    sampleCount = 0;         // 有效快照数
    double upRatio = 0.0;           // 上涨家数占比
    double medianChangePct = 0.0;   // 涨跌幅中位数 %
    double totalAmount = 0.0;       // 全市场成交额（元）
    bool   breadthKnown = false;    // 快照覆盖 ≥50% 且与交易日对齐

    int    themeCount = 0;          // 涨停覆盖的题材数
    int    topThemeCount = 0;       // 最强题材涨停家数
    double themeFocus = 0.0;        // topThemeCount / limitUpCount（主线集中度）
    int    themeCoveredStocks = 0;  // 有题材归属的涨停股数
    bool   themeKnown = false;      // 题材数据是否可用

    bool   quotesAligned = false;   // 快照与交易日是否对齐（历史回看时可能不对齐）
    int    snapshotLimitLike = 0;   // 快照中涨幅≥9.7% 的家数（反证涨停池是否缺页）
    bool   valid() const { return date.isValid() && limitUpCount > 0; }  // 有日期且有涨停才算有效
};

// 六维分项之一
struct SentimentSubScore {
    QString key;        // S1..S6                分项编号
    QString name;       // 中文名
    double  score = 0.0;// 0~100                分项得分
    double  weight = 0.0;                        // 该分项权重（自动归一化）
    bool    available = true;   // false = 数据缺失，权重不计入复合
    QString detail;     // 实际值说明（UI / 自检可核对）
};

// 情绪状态：复合指数 + 阶段 + 档位 + 有效仓位上限 + 兼容用的 MarketRegime
struct SentimentState {
    QDate date;                                  // 状态所属交易日
    SentimentFact fact;                          // 原始事实
    QVector<SentimentSubScore> subScores;        // 六维分项

    double composite = 0.0;      // 复合情绪指数 0~100
    double trend = 0.0;          // 相对近 N 日均值（正 = 升温）
    double acceleration = 0.0;   // 相对上一交易日（情绪加速度）
    bool   trendKnown = false;                   // 趋势是否可算（历史足够）
    QVector<QPair<QDate, double>> history;   // 近 N 日指数序列（升序）

    MarketPhase phase = MarketPhase::Unknown;     // 复用的情绪阶段
    SentimentTier tier = SentimentTier::NoTrade;  // 仓位档位
    double positionCapPct = 0.0; // 有效总仓上限（%，已按情绪/数据可信度折减）
    bool   tradeAllowed = false;                  // 是否允许开仓

    QStringList vetoReasons;     // 硬否决原因
    QString note;                // 一行结论（UI 直接展示）
    bool degraded = false;       // 任一分项数据缺失 → 降级（仓位折减）
    bool dataSuspect = false;    // 交叉校验失败（池子与快照互相矛盾）→ 禁止开仓
    QStringList dataWarnings;    // 校验明细（通过/失败都记，便于事后追）

    MarketRegime regime;         // 兼容层：下游只认 MarketRegime

    bool valid() const { return date.isValid(); }  // 有日期即视为有效
};

// 做多信号：单只标的的最终可执行裁决
struct LongSignal {
    QString code;                                // 股票代码
    QString name;                                // 股票名称

    SignalAction action = SignalAction::None;    // 最终动作
    int    level = 0;             // 0=无 1=观察 2=试仓 3=建仓 4=主攻

    double score = 0.0;                          // 个股评分
    double riskReward = 0.0;                     // 盈亏比
    double riskPct = 0.0;         // 买入中枢→止损中枢（%）

    double triggerLow = 0.0;      // 触发价下沿（买入区间下沿）
    double triggerHigh = 0.0;     // 触发价上沿（追高上限）
    double invalidation = 0.0;    // 失效价（跌破无条件离场）

    double positionPct = 0.0;     // 建议单票仓位（%，已按成交概率折减）
    double riskBudgetPct = 0.0;   // 单笔风险预算（%）
    double fillProbability = 1.0; // 预估成交概率 0~1
    int    maxHoldDays = 2;                      // 计划持有上限（天）

    QStringList conditions;       // 已满足条件（可核对）
    QStringList blockers;         // 未满足条件
    QString rationale;            // 一句话结论
};
