#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// 多维度做多选股：维度枚举与贡献度 POD。
// 放在 core 层，便于 TradePlan / Database / UI / 回测共同引用而不产生反向依赖。
enum class FactorDimension {
    Daban = 0,        // 打板核心（保留原 Scorer 8 因子）
    Trend,            // 趋势
    Momentum,         // 动量
    CapitalFlow,      // 资金流向（免费接口无主力净流入，采用代理口径）
    Fundamental,      // 基本面
    Volatility,       // 波动率（风险调节）
    Count
};

constexpr int kFactorDimensionCount = int(FactorDimension::Count);

inline QString factorDimensionName(int d)
{
    switch (d) {
    case int(FactorDimension::Daban):        return QStringLiteral("打板核心");
    case int(FactorDimension::Trend):        return QStringLiteral("趋势");
    case int(FactorDimension::Momentum):     return QStringLiteral("动量");
    case int(FactorDimension::CapitalFlow):  return QStringLiteral("资金流向");
    case int(FactorDimension::Fundamental):  return QStringLiteral("基本面");
    case int(FactorDimension::Volatility):   return QStringLiteral("波动率");
    default:                                 return QStringLiteral("-");
    }
}

inline QString factorDimensionDefinition(int d)
{
    switch (d) {
    case int(FactorDimension::Daban):
        return QStringLiteral("原有打板策略：Scorer 8 因子（换手/量能/封板时点/封单/题材/高度/涨停基因/均线），"
                              "五层硬筛选 L0-L3 保持不变");
    case int(FactorDimension::Trend):
        return QStringLiteral("均线多头排列强度 0.4 + 收盘相对 MA20 偏离 0.3 + 收盘相对 MA60 偏离 0.3，"
                              "衡量中期趋势强度");
    case int(FactorDimension::Momentum):
        return QStringLiteral("近 20 日收益 0.35 + 近 5 日收益 0.25 + 量比得分 0.20 + 20 日涨停基因 0.20，"
                              "衡量上涨动能延续性");
    case int(FactorDimension::CapitalFlow):
        return QStringLiteral("代理口径（免费接口无主力净流入）：封单强度 0.35 + 成交额放大 0.30 "
                              "+ 换手活跃度 0.20 + 收盘承接位置 0.15");
    case int(FactorDimension::Fundamental):
        return QStringLiteral("PE 估值 0.25 + PB 估值 0.20 + 流通市值适中度 0.25 "
                              "+ ST/退市风险 0.15 + 换手活跃度 0.15；历史快照缺失时仅用可得子项");
    case int(FactorDimension::Volatility):
        return QStringLiteral("ATR14% 0.50 + 20 日收益标准差 0.30 + 20 日最大回撤 0.20；"
                              "分数高 = 波动适中可控，属风险调节项");
    default:
        return QString();
    }
}

// 单个维度对综合评分的贡献分解：contribution 之和等于综合分（0~100 制）
struct FactorContribution {
    int     dimension = 0;        // FactorDimension 的整数值
    QString name;                 // 维度名（含「短板补偿」等虚拟项）
    double  normalized = 0.0;     // 维度得分 0~1
    double  weight = 0.0;         // 归一化后的有效权重 0~1
    double  contribution = 0.0;   // 贡献分（0~100 制）
    double  share = 0.0;          // 贡献占综合分比例 %
    double  coverage = 0.0;       // 数据可用子项占比 0~1
    bool    available = false;    // 是否参与本次评分
    QString detail;               // 原始值 / 口径 / 缺失说明
};

// ---------------- 「查看得分由来」展示结构 ----------------
// 单独一个得分项的来源明细（打板 8 因子 / 综合评分各维度各占一行）
struct ScoreItem {
    QString name;                 // 因子 / 维度名称
    double  value = 0.0;          // 该项归一化得分 0~1
    double  weight = 0.0;         // 该项权重（打板为原始权重，综合为归一化权重）
    double  contribution = 0.0;   // 折算到 0~100 的贡献分
    bool    available = true;     // 该项是否参与本次评分
    QString detail;               // 计算依据与数据来源
};

// 一个分数的完整由来：公式 + 数据来源 + 逐项明细 + 修正项说明
struct ScoreBreakdown {
    QString title;                // 分数名称（综合评分 / 打板评分）
    double  value = 0.0;          // 最终得分 0~100
    QString formula;              // 计算公式
    QString source;               // 数据来源
    QStringList notes;            // 修正项 / 扣分说明 / 汇总校验
    QVector<ScoreItem> items;     // 逐项明细

    bool valid() const { return !items.isEmpty(); }
};
