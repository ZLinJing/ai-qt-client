#pragma once

// ============================================================================
// DecisionReview：以「自己的决策」为对象的结构化复盘（纯计算、零 IO、零 GUI）
//
// 回答三个问题，且每个结论都带【实测值 + 判定式/阈值 + 数据窗口 + 来源】：
//   ① 当时触发买入 / 卖出条件的核心条件是什么（值多少、门槛多少、数据从哪来）
//   ② 信号之后实际走势与计划预期偏差有多大（MFE / MAE / 目标偏差 / 是否触及）
//   ③ 没执行的信号、持仓的隐性风险、板块轮动，事后各自表现如何
//
// 边界（重要）：本模块只做**事后核对**，不预测涨跌、不生成新的买卖或仓位指令、
// 不修改任何交易阈值；「跳过的机会」是事后统计，不评价当时的决策，也不构成建议。
// 数据缺失一律标 known=false / available=false 并写明原因，绝不补 0 或猜测值。
// ============================================================================

#include "core/TradePlan.h"          // TradePlan / RecommendBundle
#include "storage/TradeRecord.h"     // 交易日志（判定执行 / 跳过 / 持仓）
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace DecisionReview {

// 全部判定口径的唯一真相：计算、界面「判定条件」清单、离线断言共用同一份常量
struct Criteria {
    static constexpr int    kRsiPeriod       = 6;      // RSI 主口径周期
    static constexpr double kRsiExtreme      = 80.0;   // 极端超买线
    static constexpr double kRsiHigh         = 70.0;   // 常规超买线
    static constexpr double kMaConvergePct   = 1.5;    // 均线收敛：MA5/10/20 极差 / MA20 ≤1.5%
    static constexpr int    kSwingWindow     = 5;      // 摆动低点分窗长度（交易日）
    static constexpr int    kSwingSamples    = 3;      // 判定「低点下移」所需的连续窗口数
    static constexpr double kChipMinShare    = 0.02;   // 有效压力峰最小筹码占比（与 Chip 一致）
    static constexpr int    kChipLayers      = 2;      // 「多层阻力」的层数门槛
    static constexpr int    kOutcomeHorizon  = 5;      // 事后观察窗口（交易日）
    static constexpr double kFlowPositiveYi  = 0.0;    // 主力净流入 > 0 亿元
    static constexpr double kSealProxyMin    = 0.20;   // 封单额/成交额 代理口径门槛
    static constexpr int    kSectorTopN      = 3;      // 领涨 / 领跌各取前 N
};

// 一条可核对的证据：数值、判定式、是否成立、来源窗口、口径限制缺一不可
struct Evidence {
    QString name;        // 条件名
    QString role;        // 触发条件 / 观测指标 / 风险标记 / 事后核对
    QString value;       // 实测值（含单位）；unknown 时为「数据缺失」
    QString rule;        // 判定式（含阈值），如 "RSI6 ≥ 80.0"
    bool    known = false;   // 数据是否可用；false 时 passed 无意义
    bool    passed = false;  // 是否成立 / 是否触发
    QString source;      // 数据来源 + 样本窗口，如 "日K前复权 · 截至 2026-11-08 · 近 6 根"
    QString caveat;      // 口径限制 / 代理说明 / 缺失原因

    QJsonObject toJson() const;
    static Evidence fromJson(const QJsonObject& o);
};

// 板块资金流的一行客观数据（不做题材持续性推断）
struct SectorFlowRow {
    QString code;            // BKxxxx
    QString name;            // 板块名
    double  changePct = 0.0; // 当日涨跌幅 %（f3）
    double  mainNetYi = 0.0; // 主力净流入（亿元，f62 元 / 1e8）
    double  mainPct = 0.0;   // 主力净额占成交额 %（f184）
    bool    flowKnown = false;   // 资金流字段是否解析成功
    QString tag;             // 领涨 / 领跌 / 中间

    QJsonObject toJson() const;
    static SectorFlowRow fromJson(const QJsonObject& o);
};

// ① 信号溯源：一条当时的信号 + 逐条核心条件
struct SignalTrace {
    QDate   date;
    QString code;
    QString name;
    QString side;            // 买入信号 / 离场条件（卖出侧）
    QString origin;          // 当日计划快照 / 交易日志持仓 + 计划离场条件
    QString level;           // 信号档位或状态标签
    QString expectation;     // 策略当时的预期（含计划价与持有口径）
    QVector<Evidence> conditions;   // 逐条核心条件（带数值 / 阈值 / 来源）
    int     knownCount = 0;         // 有数据的条件数
    int     passedCount = 0;        // 成立的条件数
    bool    complete = false;       // 所有条件是否都有数据
    QString verdict;         // 数据充分 / 数据不足（列出缺项）

    QJsonObject toJson() const;
    static SignalTrace fromJson(const QJsonObject& o);
};

// ② 预期 vs 实际：从信号日之后最多 kOutcomeHorizon 根日 K 复算
struct OutcomeDeviation {
    QString code;
    QString name;
    QDate   signalDate;
    QDate   lastBarDate;
    int     bars = 0;               // 实际拿到的后续交易日数
    double  basePrice = 0.0;        // 信号日收盘（不复权真实价）
    double  targetPrice = 0.0;      // 计划止盈下沿（预期）
    double  stopPrice = 0.0;        // 计划止损上沿（预期）
    double  maxHigh = 0.0;
    double  minLow = 0.0;
    double  lastClose = 0.0;
    double  mfePct = 0.0;           // 最大有利偏移 (最高/基准-1)*100
    double  maePct = 0.0;           // 最大不利偏移 (最低/基准-1)*100
    double  closePct = 0.0;         // 窗口末收盘相对基准
    double  targetDeviationPct = 0.0; // 实际最高 vs 预期目标：(最高-目标)/目标*100
    bool    targetTouched = false;  // 窗口内最高价是否触及止盈下沿
    bool    stopTouched = false;    // 窗口内最低价是否触及止损上沿
    bool    known = false;          // 窗口内是否有可用日 K
    QString dataNote;               // 数据不足 / 窗口未闭合的原因
    QString summary;                // 白话结论，全部数字来自上面的字段
    QVector<Evidence> checks;       // 逐条事后核对证据

    QJsonObject toJson() const;
    static OutcomeDeviation fromJson(const QJsonObject& o);
};

// ③ 被跳过的信号：事后表现只做统计，不评价当时决策
struct SkippedOpportunity {
    QString code;
    QString name;
    QDate   signalDate;
    QString skipRule;        // 判定「未执行」的具体条件（可核对）
    double  buyMid = 0.0;    // 计划买入中枢
    double  targetPrice = 0.0;
    double  stopPrice = 0.0;
    int     bars = 0;
    QDate   lastBarDate;
    double  maxHigh = 0.0;
    double  minLow = 0.0;
    double  lastClose = 0.0;
    double  mfePct = 0.0;
    double  maePct = 0.0;
    double  closePct = 0.0;
    double  hypotheticalPct = 0.0;  // 机械假设：计划中枢买入 → 窗口末收盘（扣双边费用）
    bool    targetTouched = false;
    bool    stopTouched = false;
    bool    known = false;
    QString basis;           // 假设收益的计算口径与不包含项
    QString caveat;          // 事后统计边界（固定文案）
    QString dataNote;

    QJsonObject toJson() const;
    static SkippedOpportunity fromJson(const QJsonObject& o);
};

// ④ 持仓隐性风险：一类风险一条记录，逐条带判定式
struct HoldingRisk {
    QString code;
    QString name;
    QString type;            // RSI 极端超买 / 低点下移·假企稳 / 上方多层阻力
    QString level;           // 触发 / 未触发 / 数据不足
    bool    triggered = false;
    bool    known = false;
    QVector<Evidence> conditions;
    QString conclusion;      // 带数值的一句话结论

    QJsonObject toJson() const;
    static HoldingRisk fromJson(const QJsonObject& o);
};

// ⑤ 板块轮动：领涨 / 领跌的客观数值 + 数据可用性
struct SectorRotation {
    QDate   date;
    QDateTime fetchedAt;
    bool    available = false;
    QString reason;          // 不可用原因（接口失败 / 字段缺失 / 非交易日）
    QString source;          // 接口 + 字段 + 单位口径
    QString criteria;        // 领涨 / 领跌的排序口径
    QVector<SectorFlowRow> rows;

    QJsonObject toJson() const;
    static SectorRotation fromJson(const QJsonObject& o);
};

// 组装报告所需的全部事实（由 UI 层从本地缓存 / 日志 / 行情快照注入）
struct Input {
    QDate date;              // 被复盘的决策日
    QDate asOf;              // 复盘基准日（可用日 K 的最后日期）
    QVector<TradePlan> plans;                 // 当日计划（可执行 + 观察）
    QHash<QString, KlineSeries> klines;      // 代码 → 日 K（含不复权）
    QVector<TradeRecord> trades;             // 交易日志（执行 / 跳过 / 持仓）

    // 信号当时的市场与资金事实（历史日期未留档时留空，报告会明确标数据缺失）
    bool    sentimentKnown = false;
    double  composite = 0.0;
    bool    tradeAllowed = false;
    double  positionCapPct = 0.0;
    QHash<QString, double> mainNetYi;        // 代码 → 个股主力净流入（亿元）
    QHash<QString, QString> mainNetSource;   // 代码 → 资金流数据来源与窗口
    QHash<QString, double> sealToAmount;     // 代码 → 封单额/成交额（代理口径）

    QVector<SectorFlowRow> sectorRows;       // 板块客观数据
    QDateTime sectorFetchedAt;
    QString   sectorReason;                  // 板块数据不可用原因；空表示可用
    QString   sectorSource;
};

struct Report {
    QDate   date;
    QDateTime generatedAt;
    bool    valid = false;
    QString purpose;         // 使用边界（固定文案）
    // 成员名不可叫 signals —— Qt 把它宏展开为 public（项目里已有同样的坑）
    QVector<SignalTrace> signalTraces;
    QVector<OutcomeDeviation> outcomes;
    QVector<SkippedOpportunity> skipped;
    QVector<HoldingRisk> risks;
    SectorRotation sectors;
    QStringList gaps;        // 数据缺失清单（结构化、可核对）
    QStringList criteria;    // 本轮用到的全部判定条件文本

    QJsonObject toJson() const;
    static Report fromJson(const QJsonObject& o);
};

// 固定文案：使用边界 / 判定条件清单（界面与报告共用，避免两处口径不一）
QString purposeText();
QStringList criteriaLines();

// 主入口：纯计算，不访问网络 / 数据库 / 界面
Report build(const Input& in);

// 便于离线断言与复用的指标（全部先按 asOf 截断日 K，杜绝未来函数）
namespace Metric {

QVector<double> rsi(const QVector<double>& close, int period);  // Wilder RSI，与图表副图同口径
QVector<double> closes(const KlineSeries& k, const QDate& asOf, int need);  // 截断后取最近 need 根收盘价

struct MaInfo {
    bool   known = false;
    double ma5 = 0.0, ma10 = 0.0, ma20 = 0.0;
    double spreadPct = 0.0;      // (max-min)/ma20*100
    bool   converged = false;
    int    sample = 0;
};
MaInfo ma(const KlineSeries& k, const QDate& asOf);

struct LowInfo {
    bool   known = false;
    QVector<double> lows;        // 由旧到新的各窗口最低价
    bool   declining = false;    // 最近 kSwingSamples 个窗口低点严格递减
    double slopePct = 0.0;       // (末窗低点-首窗低点)/首窗低点*100
    int    window = 0;
};
LowInfo swingLows(const KlineSeries& k, const QDate& asOf);

struct ChipInfo {
    bool   known = false;
    int    layers = 0;
    QStringList detail;          // 每个有效压力峰的价格区间 + 占比
    int    sample = 0;
};
ChipInfo chipLayers(const KlineSeries& k, const QDate& asOf, double price);

struct FollowUp {
    bool   known = false;
    int    bars = 0;
    QDate  firstDate, lastDate;
    double base = 0.0, maxHigh = 0.0, minLow = 0.0, lastClose = 0.0;
};
FollowUp followUp(const KlineSeries& k, const QDate& signalDate, int horizon);

} // namespace Metric
} // namespace DecisionReview
