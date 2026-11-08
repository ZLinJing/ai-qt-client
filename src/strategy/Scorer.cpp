#include "strategy/Scorer.h"             // 本模块声明
#include "core/Indicators.h"             // 各因子归一化函数
#include <QStringList>                   // 因子名称列表

namespace {                              // 评分权重与归一化口径（固定）

// 八项因子权重：顺序与 factorNames() / breakdown() 一致
constexpr double kWTurnover  = 1.0;      // 换手
constexpr double kWVolRatio  = 1.0;      // 量能
constexpr double kWSealTime  = 1.2;      // 封板时点
constexpr double kWSealOrder = 1.2;      // 封单
constexpr double kWSector    = 1.5;      // 板块/题材效应
constexpr double kWHeight    = 1.0;      // 连板高度
constexpr double kWLimitGene = 0.8;      // 涨停基因
constexpr double kWMaBull    = 0.6;      // 均线多头

constexpr double kMinVolRatio5      = 1.5;   // 量比合格下限
constexpr double kMaxVolRatio5      = 3.0;   // 量比上限（超过=爆量分歧）
constexpr double kMinSealOrderRatio = 0.20;  // 封单额/成交额 下限
constexpr double kMinSealToFloatCap = 0.01;  // 封单额/流通市值 下限

// 权重表：score() 与 describe() 共用，避免两处权重不一致
QVector<double> factorWeights()
{
    return { kWTurnover, kWVolRatio, kWSealTime, kWSealOrder,
             kWSector,   kWHeight,   kWLimitGene, kWMaBull };
}

} // namespace

// 因子名称：顺序必须与 breakdown() 的输出顺序一致
QStringList Scorer::factorNames() const
{
    return { QStringLiteral("换手"), QStringLiteral("量能"), QStringLiteral("封板时点"),
             QStringLiteral("封单"),   QStringLiteral("板块"), QStringLiteral("高度"),
             QStringLiteral("涨停基因"), QStringLiteral("均线") };
}

// 逐因子归一化到 0~1
QVector<double> Scorer::breakdown(const Candidate& c) const
{
    QVector<double> out;                         // 分项得分

    const double capYi = c.entry.floatCap / 1e8; // 流通市值换算成亿元
    const Ind::TurnoverBand band = Ind::turnoverBandFor(capYi, c.entry.board);  // 选市值档（双创已放大）
    out.append(band.dangerTurnover > 0.0 ? Ind::turnoverScore(c.entry.turnover, band) : 0.0);  // 换手得分

    out.append(Ind::volRatioScore(c.volRatio5, kMinVolRatio5, kMaxVolRatio5));  // 量能得分

    out.append(Ind::sealTimeScore(c.entry.firstSealTime, 570, 690));  // 封板时点得分（10:30 前后分界）

    const double sealAmt = c.entry.sealToAmountRatio() / qMax(1e-6, kMinSealOrderRatio);  // 封单/成交额 相对下限
    const double sealCap = c.entry.sealToFloatCapRatio() / qMax(1e-6, kMinSealToFloatCap); // 封单/流通市值 相对下限
    out.append(qBound(0.0, 0.6 * qBound(0.0, sealAmt, 1.5) / 1.5
                          + 0.4 * qBound(0.0, sealCap, 1.5) / 1.5, 1.0));                     // 两口径加权得封单得分

    const double sectorScore = qBound(0.0, c.sectorLimitCount / 6.0, 1.0);  // 板块效应：6 家封顶
    out.append(sectorScore);                     // 板块得分

    out.append(Ind::heightScore(c.entry.boardCount));  // 连板高度得分

    out.append(qBound(0.0, c.limitCount20 / 5.0, 1.0));  // 涨停基因：20 日内 5 次封顶

    out.append(qBound(0.0, c.maBull, 1.0));      // 均线多头强度（本身已 0~1）

    return out;                                  // 返回 8 项得分
}

// 加权求和归一化到 0~100，并对高位连板额外扣分
double Scorer::score(const Candidate& c) const
{
    const QVector<double> b = breakdown(c);      // 取分项得分
    const QVector<double> w = factorWeights();  // 对应权重
    double sum = 0.0, wsum = 0.0;                // 加权和 / 权重和
    for (int i = 0; i < b.size() && i < w.size(); ++i) {  // 逐项累加
        sum  += b[i] * w[i];                     // 累加加权分
        wsum += w[i];                            // 累加权重
    }
    if (wsum <= 0) return 0.0;                   // 权重非法则返回 0
    double s = sum / wsum * 100.0;               // 归一化到 0~100

    if (c.entry.boardCount >= 5) s -= (c.entry.boardCount - 4) * 3.0;  // 5 板起每多一板扣 3 分风险溢价
    return qBound(0.0, s, 100.0);                // 夹紧到 0~100
}

// 打板分由来：把 8 因子、权重、贡献分、原始数据与修正项全部摊开
ScoreBreakdown Scorer::describe(const Candidate& c, double finalScore) const
{
    ScoreBreakdown sb;                            // 结果
    sb.title = QStringLiteral("打板评分");
    sb.value = finalScore;
    sb.formula = QStringLiteral(
        "打板分 = Σ(因子归一化得分 × 权重) ÷ 权重合计 × 100，"
        "再叠加高位连板扣分 / 风险标记扣分 / 同题材身位第一加分");
    sb.source = QStringLiteral(
        "涨停池（换手 hs / 封单 fund / 首封 fbt / 连板 lbc）+ 日 K（量比 / 涨停基因 / 均线）");

    const QVector<double> b = breakdown(c);       // 8 项归一化得分
    const QVector<double> w = factorWeights();    // 对应权重
    const QStringList names = factorNames();      // 对应名称

    double wsum = 0.0;                             // 权重合计
    for (double v : w) wsum += v;

    const double capYi = c.entry.floatCap / 1e8;   // 流通市值（亿元）
    const Ind::TurnoverBand band = Ind::turnoverBandFor(capYi, c.entry.board);  // 换手档位
    const QString sealTimeText = c.entry.firstSealTime.isValid()
        ? c.entry.firstSealTime.toString(QStringLiteral("HH:mm:ss"))
        : QStringLiteral("缺失");
    const QString themeText = c.entry.theme.isEmpty() ? QStringLiteral("未分类") : c.entry.theme;

    QStringList details;                           // 与 8 项一一对应
    details << (band.dangerTurnover > 0.0
        ? QStringLiteral("换手 %1%（%2 亿档：合格 ≥%3% / 黄金 %4~%5% / 危险 ≥%6%）· 来源：涨停池换手率")
              .arg(c.entry.turnover, 0, 'f', 1).arg(capYi, 0, 'f', 1)
              .arg(band.minTurnover, 0, 'f', 0).arg(band.goodTurnover, 0, 'f', 0)
              .arg(band.bestTurnover, 0, 'f', 0).arg(band.dangerTurnover, 0, 'f', 0)
        : QStringLiteral("流通市值缺失，无有效换手档位，该项按 0 分计"));

    details << QStringLiteral("量比 %1（当日量 ÷ 前 5 日均量，黄金区间 %2~%3）· 来源：日 K 成交量")
                   .arg(c.volRatio5, 0, 'f', 2).arg(kMinVolRatio5, 0, 'f', 1).arg(kMaxVolRatio5, 0, 'f', 1);

    details << (c.entry.firstSealTime.isValid()
        ? QStringLiteral("首封 %1（09:30 前满分，11:30 后归零，区间线性衰减）· 来源：涨停池首封时间")
              .arg(sealTimeText)
        : QStringLiteral("无首封时间，该项按 0 分计"));

    details << QStringLiteral("封单/成交额 %1%（基准 %2%）、封单/流通市值 %3%（基准 %4%），按 0.6 / 0.4 加权"
                              " · 来源：涨停池封单额 / 成交额 / 流通市值")
                   .arg(c.entry.sealToAmountRatio() * 100.0, 0, 'f', 1)
                   .arg(kMinSealOrderRatio * 100.0, 0, 'f', 0)
                   .arg(c.entry.sealToFloatCapRatio() * 100.0, 0, 'f', 2)
                   .arg(kMinSealToFloatCap * 100.0, 0, 'f', 2);

    details << QStringLiteral("同题材「%1」当日涨停 %2 家（6 家封顶）· 来源：概念题材 + 涨停池")
                   .arg(themeText).arg(c.sectorLimitCount);

    details << QStringLiteral("%1 板（4 板满分，5 板起每板扣 3 分风险溢价）· 来源：涨停池连板数")
                   .arg(c.entry.boardCount);

    details << QStringLiteral("近 20 日涨停 %1 次（5 次封顶）· 来源：日 K 不复权序列涨停判定")
                   .arg(c.limitCount20, 0, 'f', 0);

    details << QStringLiteral("均线多头强度 %1（MA5>MA10>MA20 且价站上 MA5 为 1.0）· 来源：日 K 前复权")
                   .arg(c.maBull, 0, 'f', 2);

    double itemSum = 0.0;                          // 各项贡献合计
    for (int i = 0; i < b.size() && i < w.size(); ++i) {
        ScoreItem it;
        it.name = i < names.size() ? names.at(i) : QStringLiteral("-");
        it.value = b.at(i);
        it.weight = w.at(i);
        it.contribution = wsum > 0.0 ? b.at(i) * w.at(i) / wsum * 100.0 : 0.0;
        it.available = true;
        it.detail = i < details.size() ? details.at(i) : QString();
        sb.items.append(it);
        itemSum += it.contribution;
    }

    sb.notes << QStringLiteral("权重合计 %1，已归一化到 0~100；8 项贡献合计 %2 分")
                    .arg(wsum, 0, 'f', 1).arg(itemSum, 0, 'f', 1);

    const double base = score(c);                  // 基础加权分（含高位扣分）
    if (c.entry.boardCount >= 5)
        sb.notes << QStringLiteral("高位连板扣分：%1 板 → -(%1-4)×3 = -%2 分")
                        .arg(c.entry.boardCount).arg((c.entry.boardCount - 4) * 3);

    if (!c.riskFlags.isEmpty()) {
        const int n = qMin(c.riskFlags.size(), 3);  // 与筛选管线一致：最多 3 条参与扣分
        sb.notes << QStringLiteral("风险标记扣分：%1 条 × 3 分 = -%2 分（%3）")
                        .arg(n).arg(n * 3).arg(c.riskFlags.join(QStringLiteral(" / ")));
    }
    if (c.entry.themeLeader && c.entry.themeLimitCount >= 3)
        sb.notes << QStringLiteral("同题材身位第一加分：+8 分（题材涨停 %1 家已成势）")
                        .arg(c.entry.themeLimitCount);

    sb.notes << QStringLiteral("基础加权分 %1 分 → 修正后最终得分 %2 分（评级 %3）")
                    .arg(base, 0, 'f', 1).arg(finalScore, 0, 'f', 1)
                    .arg(c.grade.isEmpty() ? QStringLiteral("-") : c.grade);

    return sb;
}
