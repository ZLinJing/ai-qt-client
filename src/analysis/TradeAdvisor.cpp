#include "analysis/TradeAdvisor.h"
#include "core/ChipDistribution.h"               // 上方筹码压力峰（修复目标位）
#include "core/Indicators.h"                     // 涨停判定 / 均线口径
#include <QtGlobal>                              // qBound / qMax / qMin
#include <cmath>                                 // std::ceil / std::fabs

namespace TradeAI {
namespace {

// ---------------------------------------------------------------- 文本工具
QString money(double v) { return QString::number(v, 'f', 2); }
QString pct(double v, int digits = 2) { return QString::number(v, 'f', digits) + QStringLiteral("%"); }

// 止损 / 止盈展示：未记录时明确写「未记录」，不用 0 冒充
QString priceOrDash(double v)
{
    return v > 0.0 ? money(v) : QStringLiteral("未记录");
}

// ---------------------------------------------------------------- 指标快照
// 只保留分析真正用到的量，避免把 K 线整段复制进结果
struct Metrics {
    bool   hasKline = false;      // 是否有可用日 K
    bool   rawPrices = false;     // true = 不复权（可与成交价直接比较）
    int    bars = 0;              // 有效 K 线根数
    double close = 0.0;           // 最后一根收盘
    double ma5 = 0.0;
    double ma10 = 0.0;
    double ma20 = 0.0;
    double ma20Prev = 0.0;        // 5 根之前的 MA20（判断上翘 / 下弯）
    double atr = 0.0;             // ATR14（元）
    double atrPct = 0.0;          // ATR14 / 现价（%）
    double ret20 = 0.0;           // 近 20 日涨跌（%）
    double volRatio = 0.0;        // 当日量 / 前 5 日均量
    double highSinceBuy = 0.0;    // 买入后最高价（利润回吐保护）
    bool   postExitValid = false; // 是否可评估「离场后走势」
    double postExitPct = 0.0;     // 卖出价至今的涨跌（%）
    double chipPressureLow = 0.0; // 上方筹码压力区下沿
    double chipPressureHigh = 0.0;// 上方筹码压力区上沿
    int    barsSinceBuy = 0;      // 买入后经过的交易日数（持仓中 -> 至今）
    int    barsHeld = 0;          // 已了结的实际持有交易日数（买入日 -> 卖出日）
};

double meanOfLast(const QVector<Candle>& bars, int n)
{
    if (n <= 0 || bars.size() < n) return 0.0;
    double sum = 0.0;
    for (int i = bars.size() - n; i < bars.size(); ++i) sum += bars.at(i).close;
    return sum / n;
}

// 序列在指定下标区间的均线（用于 MA20 斜率）
double meanRange(const QVector<Candle>& bars, int endExclusive, int n)
{
    if (n <= 0 || endExclusive < n) return 0.0;
    double sum = 0.0;
    for (int i = endExclusive - n; i < endExclusive; ++i) sum += bars.at(i).close;
    return sum / n;
}

Metrics computeMetrics(const Input& in, double refPrice)
{
    Metrics m;
    if (!in.kline) return m;

    // 价格口径：优先不复权（与成交价 / 涨停价同口径），缺失时退回前复权并标注
    const QVector<Candle> all = in.kline->hasRaw() ? in.kline->rawCandles : in.kline->candles;
    m.rawPrices = in.kline->hasRaw();
    if (all.isEmpty()) return m;

    const int end = KlineSeries::lastIndexAt(all, in.asOf);
    if (end < 0) return m;
    const QVector<Candle> bars = all.mid(0, end + 1);
    if (bars.size() < 2) return m;

    m.hasKline = true;
    m.bars = bars.size();
    m.close = bars.back().close;

    m.ma5 = meanOfLast(bars, 5);
    m.ma10 = meanOfLast(bars, 10);
    m.ma20 = meanOfLast(bars, 20);
    m.ma20Prev = meanRange(bars, bars.size() - 5, 20);

    // ATR14：真实波幅均值，缺失高低价时用收盘价差兜底
    {
        double sum = 0.0;
        int cnt = 0;
        const int from = qMax(1, bars.size() - 14);
        for (int i = from; i < bars.size(); ++i) {
            const double tr = qMax(bars.at(i).high - bars.at(i).low,
                                   qMax(qAbs(bars.at(i).high - bars.at(i - 1).close),
                                        qAbs(bars.at(i).low - bars.at(i - 1).close)));
            sum += tr;
            ++cnt;
        }
        if (cnt > 0) m.atr = sum / cnt;
    }
    const double base = refPrice > 0.0 ? refPrice : m.close;
    m.atrPct = base > 0.0 ? m.atr / base * 100.0 : 0.0;

    if (bars.size() >= 21 && bars.at(bars.size() - 21).close > 0.0) {
        m.ret20 = (m.close / bars.at(bars.size() - 21).close - 1.0) * 100.0;
    }

    // 量比：最后一根 / 前 5 日均量
    if (bars.size() >= 6) {
        double avg = 0.0;
        for (int i = bars.size() - 6; i < bars.size() - 1; ++i) avg += bars.at(i).volume;
        avg /= 5.0;
        if (avg > 0.0) m.volRatio = bars.back().volume / avg;
    }

    // 买入后最高价与经过的交易日数
    if (in.trade.buyDate.isValid()) {
        int start = -1;
        for (int i = 0; i < bars.size(); ++i) {
            if (bars.at(i).date.isValid() && bars.at(i).date >= in.trade.buyDate) { start = i; break; }
        }
        if (start >= 0) {
            m.barsSinceBuy = bars.size() - start;
            for (int i = start; i < bars.size(); ++i) m.highSinceBuy = qMax(m.highSinceBuy, bars.at(i).high);
        }
    }

    // 离场后走势：卖出日（含）之后到基准日的收盘涨跌
    if (in.trade.closed() && in.trade.sellDate.isValid()) {
        int sellIdx = -1;
        for (int i = 0; i < bars.size(); ++i) {
            if (bars.at(i).date.isValid() && bars.at(i).date >= in.trade.sellDate) { sellIdx = i; break; }
        }
        if (sellIdx >= 0 && bars.size() - 1 > sellIdx && in.trade.sellPrice > 0.0) {
            m.postExitValid = true;
            m.postExitPct = (bars.back().close / in.trade.sellPrice - 1.0) * 100.0;
        }

        // 已了结的持有期：买入日 -> 卖出日（不能用「买入日 -> 今天」，否则会把离场后的天数算进去）
        if (in.trade.buyDate.isValid()) {
            int buyIdx = -1;
            for (int i = 0; i < bars.size(); ++i) {
                if (bars.at(i).date.isValid() && bars.at(i).date >= in.trade.buyDate) { buyIdx = i; break; }
            }
            if (buyIdx >= 0 && sellIdx >= buyIdx) m.barsHeld = sellIdx - buyIdx + 1;
        }
    }

    // 上方筹码压力峰（修复路径的现实阻力位）：只在真实价口径下计算，
    // 前复权价与成交价不同基准，混用会把压力位整体搬错。
    if (bars.size() >= 20 && refPrice > 0.0 && m.rawPrices) {
        const Chip::Distribution chip = Chip::build(bars, refPrice);
        if (chip.valid) {
            if (const Chip::Peak* p = chip.pressureAbove(refPrice)) {
                m.chipPressureLow = p->low;
                m.chipPressureHigh = p->high;
            }
        }
    }
    return m;
}

// ---------------------------------------------------------------- 公共上下文
struct Ctx {
    const TradeRecord& t;
    const Input& in;
    const Metrics& m;
    double price = 0.0;      // 评估价（现价 / 卖出价）
    bool   closed = false;
    bool   priced = false;
    int    holdDays = 0;     // 优先用交易日口径
    double breakeven = 0.0;  // 含费回本价
    bool   brokenStop = false;
    double dayChangePct = 0.0;   // 当日涨跌（无快照时为 0，且 hasDayChange=false）
    bool   hasDayChange = false;
    double pnl = 0.0;            // 评估盈亏（元）
    double pnlPct = 0.0;         // 评估盈亏（%）
};

// 含双边费用的真实保本价：
//   p·q·(1 − 卖出费率) = buyPrice·q·(1 + 买入佣金率)
//   p = buyPrice × (1 + 买入佣金率) ÷ (1 − 卖出费率)
double breakevenPrice(const TradeRecord& t)
{
    if (t.buyPrice <= 0.0) return 0.0;
    const double denom = 1.0 - TradeRecord::sellFeeRate();
    if (denom <= 0.0) return t.buyPrice;
    return t.buyPrice * (1.0 + TradeRecord::buyFeeRate()) / denom;
}

// ---------------------------------------------------------------- ① 盈亏原因
QVector<ReasonItem> buildReasons(const Ctx& c)
{
    const TradeRecord& t = c.t;
    const Metrics& m = c.m;
    QVector<ReasonItem> out;

    // 1) 入场执行：买价相对计划中枢的偏离（±3% 内视为按计划）
    if (t.planBuy > 0.0 && t.buyPrice > 0.0) {
        const double dev = (t.buyPrice - t.planBuy) / t.planBuy * 100.0;
        if (qAbs(dev) <= 3.0) {
            out.append({ QStringLiteral("入场执行"),
                         QStringLiteral("按计划入场：买入 %1 相对计划中枢 %2 偏离 %3%（容差 3%），成本可控。")
                             .arg(money(t.buyPrice), money(t.planBuy), pct(dev)),
                         1 });
        } else if (dev > 0.0) {
            out.append({ QStringLiteral("入场执行"),
                         QStringLiteral("追高入场：买入 %1 高于计划中枢 %2 的 %3%，成本抬升会直接压缩止盈空间。")
                             .arg(money(t.buyPrice), money(t.planBuy), pct(dev)),
                         -1 });
        } else {
            out.append({ QStringLiteral("入场执行"),
                         QStringLiteral("低于计划成本：买入 %1 比计划中枢 %2 低 %3%，同样的止盈价对应更高收益。")
                             .arg(money(t.buyPrice), money(t.planBuy), pct(-dev)),
                         1 });
        }
    } else {
        out.append({ QStringLiteral("入场执行"),
                     QStringLiteral("未记录计划买入价，无法核对执行力；建议从计划页「记录买入」自动带出。"),
                     0 });
    }

    // 2) 兑现位置：相对计划止盈的进度
    if (t.planTake > 0.0 && t.buyPrice > 0.0) {
        const QString priceWord = c.closed ? QStringLiteral("卖出价") : QStringLiteral("现价");
        if (c.price >= t.planTake) {
            out.append({ QStringLiteral("兑现位置"),
                         QStringLiteral("已达计划止盈：%1 %2 ≥ 目标 %3（+%4），本笔主要目标已完成。")
                             .arg(priceWord, money(c.price), money(t.planTake),
                                  pct((c.price / t.planTake - 1.0) * 100.0)),
                         2 });
        } else if (c.price >= t.planTake * 0.97) {
            out.append({ QStringLiteral("兑现位置"),
                         QStringLiteral("临近止盈：%1 %2 距计划止盈 %3 仅 %4%，可按计划分批兑现。")
                             .arg(priceWord, money(c.price), money(t.planTake),
                                  pct((t.planTake / c.price - 1.0) * 100.0)),
                         1 });
        } else {
            const double gap = (t.planTake / c.price - 1.0) * 100.0;
            out.append({ QStringLiteral("兑现位置"),
                         QStringLiteral("未到止盈：%1 %2 距计划止盈 %3 还差 %4。")
                             .arg(priceWord, money(c.price), money(t.planTake), pct(gap)),
                         c.pnl >= 0.0 ? 0 : -1 });
        }
    } else {
        out.append({ QStringLiteral("兑现位置"),
                     QStringLiteral("未记录计划止盈价，止盈只能按均线与波动率动态确定（见「止盈建议」）。"),
                     0 });
    }

    // 3) 趋势结构
    if (m.hasKline && m.ma20 > 0.0) {
        if (c.price > m.ma5 && m.ma5 > m.ma10 && m.ma10 > m.ma20) {
            out.append({ QStringLiteral("趋势结构"),
                         QStringLiteral("均线多头排列：收盘 %1 > MA5 %2 > MA10 %3 > MA20 %4。")
                             .arg(money(c.price), money(m.ma5), money(m.ma10), money(m.ma20)),
                         2 });
        } else if (c.price < m.ma20) {
            out.append({ QStringLiteral("趋势结构"),
                         QStringLiteral("收盘 %1 位于 MA20 %2 下方（偏离 %3），中期趋势尚未修复。")
                             .arg(money(c.price), money(m.ma20), pct((c.price / m.ma20 - 1.0) * 100.0)),
                         -2 });
        } else if (c.price > m.ma5) {
            out.append({ QStringLiteral("趋势结构"),
                         QStringLiteral("收盘 %1 站上 MA5 %2，但均线仍未完全多头（MA10 %3 / MA20 %4）。")
                             .arg(money(c.price), money(m.ma5), money(m.ma10), money(m.ma20)),
                         1 });
        } else {
            out.append({ QStringLiteral("趋势结构"),
                         QStringLiteral("收盘 %1 跌破 MA5 %2，短线节奏转弱。")
                             .arg(money(c.price), money(m.ma5)),
                         -1 });
        }

        if (m.ma20 > 0.0 && m.ma20Prev > 0.0) {
            const double slope = (m.ma20 / m.ma20Prev - 1.0) * 100.0;
            out.append({ QStringLiteral("均线斜率"),
                         QStringLiteral("MA20 近 5 日%1（%2），%3。")
                             .arg(slope >= 0.0 ? QStringLiteral("上翘") : QStringLiteral("下弯"),
                                  pct(slope, 2),
                                  slope >= 0.0 ? QStringLiteral("中期结构仍在改善")
                                               : QStringLiteral("中期结构走坏，反弹容易被均线压制")),
                         slope >= 0.0 ? 1 : -1 });
        }

        if (qAbs(m.ret20) >= 1.0) {
            out.append({ QStringLiteral("区间动量"),
                         QStringLiteral("近 20 个交易日%1 %2%，%3。")
                             .arg(m.ret20 >= 0.0 ? QStringLiteral("上涨") : QStringLiteral("下跌"),
                                  pct(qAbs(m.ret20), 1),
                                  m.ret20 >= 0.0 ? QStringLiteral("中期动量为正，修复有惯性")
                                                 : QStringLiteral("中期动量为负，反弹先按修复而非反转对待")),
                         m.ret20 >= 5.0 ? 1 : (m.ret20 <= -5.0 ? -1 : 0) });
        }
    } else {
        out.append({ QStringLiteral("趋势结构"),
                     QStringLiteral("无日 K 数据，趋势维度跳过；结论仅基于计划价与费用口径。"),
                     0 });
    }

    // 4) 量能
    if (m.volRatio > 0.0) {
        const double chg = c.hasDayChange ? c.dayChangePct : 0.0;
        if (m.volRatio >= 1.5 && chg > 0.0) {
            out.append({ QStringLiteral("量能"),
                         QStringLiteral("放量上行：量比 %1，价格同步走强，买盘承接真实。")
                             .arg(QString::number(m.volRatio, 'f', 2)),
                         1 });
        } else if (m.volRatio >= 1.5 && chg < 0.0) {
            out.append({ QStringLiteral("量能"),
                         QStringLiteral("放量下跌：量比 %1 且当日 %2，主动性抛压，修复难度上升。")
                             .arg(QString::number(m.volRatio, 'f', 2), pct(chg)),
                         -2 });
        } else if (m.volRatio < 0.8 && chg < 0.0) {
            out.append({ QStringLiteral("量能"),
                         QStringLiteral("缩量阴跌：量比 %1，买盘缺席，反弹缺乏推力。")
                             .arg(QString::number(m.volRatio, 'f', 2)),
                         -1 });
        } else if (m.volRatio < 0.8) {
            out.append({ QStringLiteral("量能"),
                         QStringLiteral("缩量：量比 %1，抛压有限，但上攻也需要新的量能。")
                             .arg(QString::number(m.volRatio, 'f', 2)),
                         1 });
        } else {
            out.append({ QStringLiteral("量能"),
                         QStringLiteral("量能平稳：量比 %1，未出现明显的主动买/卖信号。")
                             .arg(QString::number(m.volRatio, 'f', 2)),
                         0 });
        }
    }

    // 5) 持有周期：打板基线是 T+1，超过 5 个交易日视为逻辑失效
    {
        const int natural = c.closed ? t.holdDays() : t.holdingDays(c.in.asOf);
        if (c.closed) {
            out.append({ QStringLiteral("持有周期"),
                         QStringLiteral("本次持有 %1 个自然日（约 %2 个交易日），%3。")
                             .arg(natural).arg(c.holdDays)
                             .arg(c.holdDays <= 2 ? QStringLiteral("符合打板 T+1 兑现节奏")
                                                  : (c.holdDays <= 5 ? QStringLiteral("已转为短波段")
                                                                     : QStringLiteral("超出打板持有窗口，资金效率下降"))),
                         c.holdDays <= 2 ? 1 : (c.holdDays <= 5 ? 0 : -1) });
        } else {
            out.append({ QStringLiteral("持有周期"),
                         QStringLiteral("已持有 %1 个交易日（自然日 %2），%3。")
                             .arg(c.holdDays).arg(natural)
                             .arg(c.holdDays <= 2 ? QStringLiteral("仍在打板兑现窗口内")
                                                  : (c.holdDays <= 5 ? QStringLiteral("进入波段持有阶段")
                                                                     : QStringLiteral("超出策略持有窗口，时间成本放大"))),
                         c.holdDays <= 2 ? 1 : (c.holdDays <= 5 ? 0 : -1) });
        }
    }

    // 6) 费用拖累：必须先把双边费用赚回来
    if (t.buyAmount() > 0.0) {
        const double fee = c.closed ? t.fee() : t.feeAt(c.price);
        const double feePct = fee / t.buyAmount() * 100.0;
        out.append({ QStringLiteral("交易成本"),
                     QStringLiteral("双边费用 %1 元（占成本 %2%）：毛涨幅需先覆盖该比例，才是真正盈利。")
                         .arg(money(fee), QString::number(feePct, 'f', 3)),
                     qAbs(c.pnlPct) < 1.0 ? -1 : 0 });
    }

    // 7) 离场后走势（仅已了结且有后续 K 线）
    if (m.postExitValid) {
        if (m.postExitPct > 5.0) {
            out.append({ QStringLiteral("离场后走势"),
                         QStringLiteral("卖出后该股继续上涨 %1%（现价相对卖出价），卖点偏早；下次可用移动止盈保留部分仓位。")
                             .arg(pct(m.postExitPct)),
                         -1 });
        } else if (m.postExitPct < -5.0) {
            out.append({ QStringLiteral("离场后走势"),
                         QStringLiteral("卖出后该股回落 %1%，本次离场回避了后续回撤。")
                             .arg(pct(-m.postExitPct)),
                         1 });
        } else {
            out.append({ QStringLiteral("离场后走势"),
                         QStringLiteral("卖出后至今涨跌 %1%，卖出位置大致合理。")
                             .arg(pct(m.postExitPct)),
                         0 });
        }
    }

    // 8) 市场情绪：决定修复速度与兑现节奏
    if (c.in.phaseKnown) {
        QString text;
        int impact = 0;
        switch (c.in.phase) {
        case MarketPhase::MainUp:
            text = QStringLiteral("情绪处于主升期：修复窗口相对顺畅，但也要防止高位兑现踩踏。");
            impact = 1;
            break;
        case MarketPhase::Recovery:
            text = QStringLiteral("情绪处于回暖期：反弹持续性有限，修复以 2~3 个交易日为单位观察。");
            impact = 1;
            break;
        case MarketPhase::Climax:
            text = QStringLiteral("情绪处于高潮期：分化加剧，盈利单应优先兑现而不是等更高。");
            impact = 0;
            break;
        case MarketPhase::Retreat:
            text = QStringLiteral("情绪处于退潮期：反弹容易被抛压吞掉，亏损单的修复周期通常拉长 1~2 倍。");
            impact = -2;
            break;
        case MarketPhase::IcePoint:
            text = QStringLiteral("情绪处于冰点：流动性折价，不宜用时间换空间。");
            impact = -2;
            break;
        default:
            impact = 0;
            break;
        }
        if (!text.isEmpty()) {
            out.append({ QStringLiteral("市场情绪"), text, impact });
        }
    }
    return out;
}

// ---------------------------------------------------------------- ② 止盈建议
void buildProfitExit(const Ctx& c, Insight& o)
{
    const TradeRecord& t = c.t;
    const Metrics& m = c.m;

    // 目标位：计划止盈优先；无计划时用筹码压力 / 波动率兜底
    const double planTp = t.planTake > 0.0 ? t.planTake : 0.0;
    const double targetLow = planTp > 0.0
        ? planTp
        : (m.chipPressureLow > c.price ? m.chipPressureLow : c.price * (1.0 + 0.05));
    const double atrPoints = m.atrPct > 0.0 ? qBound(2.0, m.atrPct, 8.0) : 3.0;
    double targetHigh = qMax(targetLow, c.price) * (1.0 + atrPoints / 100.0);
    if (m.chipPressureHigh > targetHigh) targetHigh = m.chipPressureHigh;
    targetHigh = qMax(targetHigh, targetLow * 1.005);

    // 移动止盈：MA5 与 1.5×ATR 取较高者，且必须低于现价
    const double trailPct = m.atrPct > 0.0 ? qBound(2.5, 1.5 * m.atrPct, 8.0) : 3.0;
    double trail = c.price * (1.0 - trailPct / 100.0);
    if (m.ma5 > 0.0 && m.ma5 < c.price) trail = qMax(trail, m.ma5);
    trail = qMin(trail, c.price * 0.995);

    // 保本线：含费成本略上方，盈利单不允许回吐成亏损单
    const double protect = c.breakeven * 1.002;

    // 利润回吐保护：从买入后最高点回撤超过峰值浮盈的 1/3
    const double peakPct = (m.highSinceBuy > 0.0 && t.buyPrice > 0.0)
        ? (m.highSinceBuy / t.buyPrice - 1.0) * 100.0 : 0.0;
    const double giveBack = peakPct > 0.0 ? (peakPct - c.pnlPct) / peakPct : 0.0;
    const bool protectProfit = !c.closed && peakPct >= 2.5 && giveBack >= 1.0 / 3.0;

    // 今日涨停：盘中卖出会丢掉次日溢价
    const bool limitUpToday = c.hasDayChange && c.in.snapshot.prevClose > 0.0
        && c.in.snapshot.price > 0.0
        && Ind::isLimitUp(c.in.snapshot.price, c.in.snapshot.prevClose,
                          boardOfCode(t.code), t.name.contains(QStringLiteral("ST")));

    if (c.closed) {
        o.exitStrategy = QStringLiteral("已了结复盘：把本次卖点固化为下次模板");
        const double achieve = planTp > 0.0 ? (t.sellPrice / planTp - 1.0) * 100.0 : 0.0;
        o.exitSteps.append({
            QStringLiteral("本次兑现"),
            QStringLiteral("%1 卖出 %2").arg(t.sellDate.toString(QStringLiteral("yyyy-MM-dd")), money(t.sellPrice)),
            planTp > 0.0
                ? QStringLiteral("相对计划止盈 %1 达成 %2；实际兑现即最终收益，不再讨论浮动。")
                      .arg(money(planTp), pct(achieve))
                : QStringLiteral("未记录计划止盈，本次按主观判断离场。"),
            false });
        o.exitSteps.append({
            QStringLiteral("下次第一目标兑现 50%"),
            planTp > 0.0 ? QStringLiteral("触及 %1").arg(money(planTp))
                         : QStringLiteral("触及 +%1%（约 %2）").arg(pct(3.0 * atrPoints), money(c.price * (1.0 + 3.0 * atrPoints / 100.0))),
            QStringLiteral("先把一半收益落袋，剩余仓位才有资格谈「让利润奔跑」。"),
            false });
        o.exitSteps.append({
            QStringLiteral("剩余仓位移动止盈"),
            QStringLiteral("跌破 MA5 或 1.5×ATR 追踪线（约 %1）").arg(money(trail)),
            QStringLiteral("趋势不破就持有；均线走平或放量滞涨即减仓。"),
            true });
        o.exitSteps.append({
            QStringLiteral("保本底线"),
            QStringLiteral("跌破 %1（含费成本 %2 上方）无条件清仓")
                .arg(money(protect), money(c.breakeven)),
            QStringLiteral("任何盈利单都不允许回吐成亏损单。"),
            true });
    } else if (limitUpToday) {
        o.exitStrategy = QStringLiteral("涨停封板：次日竞价兑现 + 移动止盈");
        o.exitSteps.append({
            QStringLiteral("今日不动"),
            QStringLiteral("封板未开"),
            QStringLiteral("涨停封板说明买盘未撤，盘中卖出会丢掉次日溢价；以收盘封单与次日竞价为准。"),
            false });
        o.exitSteps.append({
            QStringLiteral("次日竞价兑现 50%"),
            QStringLiteral("高开 ≥3% 且竞价量能正常"),
            QStringLiteral("打板收益主要来自次日溢价，高开兑现一半可锁定大部分收益。"),
            false });
        o.exitSteps.append({
            QStringLiteral("剩余仓位移动止盈"),
            QStringLiteral("跌破 %1（MA5 / 1.5×ATR 取高者）").arg(money(trail)),
            QStringLiteral("封板失败或均线走坏即离场。"),
            true });
        o.exitSteps.append({
            QStringLiteral("保本底线"),
            QStringLiteral("跌破 %1 清仓").arg(money(protect)),
            QStringLiteral("含费成本上方 0.2%，杜绝盈利回吐成亏损。"),
            true });
    } else if (planTp > 0.0 && c.price >= planTp) {
        o.exitStrategy = QStringLiteral("分批兑现 + 移动止盈（已进入计划止盈区）");
        o.exitSteps.append({
            QStringLiteral("立即兑现 50%"),
            QStringLiteral("现价 %1 ≥ 计划止盈 %2（已触发）").arg(money(c.price), money(planTp)),
            QStringLiteral("保守口径的止盈下沿已达成，先锁定一半收益，避免冲高回落。"),
            false });
        o.exitSteps.append({
            QStringLiteral("再兑现 30%"),
            QStringLiteral("触及 %1（筹码压力上沿 / 计划止盈上浮 1×ATR）").arg(money(targetHigh)),
            QStringLiteral("上方筹码密集区是现实的兑现位，冲高遇阻就减。"),
            false });
        o.exitSteps.append({
            QStringLiteral("剩余 20% 移动止盈"),
            QStringLiteral("跌破 %1（MA5 %2 / 1.5×ATR ≈ %3 取高者）")
                .arg(money(trail), m.ma5 > 0.0 ? money(m.ma5) : QStringLiteral("-"),
                     pct(trailPct)),
            QStringLiteral("趋势不破就保留底仓，吃趋势尾段；跌破即全部了结。"),
            true });
        o.exitSteps.append({
            QStringLiteral("保本清仓线"),
            QStringLiteral("跌破 %1（含费成本 %2 上方 0.2%）").arg(money(protect), money(c.breakeven)),
            QStringLiteral("盈利单不允许回吐成亏损单。"),
            true });
    } else if (planTp > 0.0 && c.price >= planTp * 0.97) {
        o.exitStrategy = QStringLiteral("到价分批止盈（临近目标）");
        o.exitSteps.append({
            QStringLiteral("触及即兑现 50%"),
            QStringLiteral("触及计划止盈 %1").arg(money(planTp)),
            QStringLiteral("距目标仅 %1%，挂条件单而不是盯盘追价。")
                .arg(pct((planTp / c.price - 1.0) * 100.0)),
            false });
        o.exitSteps.append({
            QStringLiteral("剩余 50% 移动止盈"),
            QStringLiteral("跌破 %1").arg(money(trail)),
            QStringLiteral("MA5 与 1.5×ATR 取高者，兼顾趋势与回撤。"),
            true });
        o.exitSteps.append({
            QStringLiteral("保本清仓线"),
            QStringLiteral("跌破 %1").arg(money(protect)),
            QStringLiteral("含费成本上方 0.2%，确保不亏。"),
            true });
    } else {
        o.exitStrategy = QStringLiteral("持有待涨 + 保本止盈");
        // 未记录计划止盈时用「筹码压力 / 波动率目标」兜底，避免执行步骤里出现「未记录」这种空话
        const double tp = planTp > 0.0 ? planTp : targetLow;
        const QString tpAnchor = planTp > 0.0 ? QStringLiteral("计划止盈") : QStringLiteral("动态目标");
        o.exitSteps.append({
            QStringLiteral("继续持有"),
            QStringLiteral("现价 %1 未到%2 %3（还差 %4）")
                .arg(money(c.price), tpAnchor, money(tp),
                     pct(qMax(0.0, (tp / c.price - 1.0) * 100.0))),
            QStringLiteral("浮盈未达目标且趋势未坏时，提前离场会错失主升段。"),
            false });
        o.exitSteps.append({
            QStringLiteral("到价兑现 50%"),
            QStringLiteral("触及%1 %2").arg(tpAnchor, money(tp)),
            planTp > 0.0 ? QStringLiteral("按计划兑现，不预测更高价。")
                         : QStringLiteral("无计划止盈时以筹码压力峰为兑现锚点，到达即减半。"),
            false });
        o.exitSteps.append({
            QStringLiteral("保护性止盈"),
            QStringLiteral("跌破 %1（MA5 / 含费成本上方取高者）")
                .arg(money(qMax(protect, m.ma5 > 0.0 && m.ma5 < c.price ? m.ma5 : protect))),
            QStringLiteral("趋势走坏或利润归零前先离场。"),
            true });
        o.exitSteps.append({
            QStringLiteral("时间止盈"),
            QStringLiteral("持有满 5 个交易日仍未触及目标 → 减半"),
            QStringLiteral("打板逻辑的兑现窗口是 T+1～T+3，超期说明逻辑未兑现。"),
            true });
    }

    if (protectProfit) {
        o.exitSteps.prepend({
            QStringLiteral("立即减半（利润回吐保护）"),
            QStringLiteral("浮盈从最高 %1 回落到 %2，回吐 %3%")
                .arg(pct(peakPct), pct(c.pnlPct), QString::number(giveBack * 100.0, 'f', 0)),
            QStringLiteral("回吐超过峰值浮盈的 1/3，说明上攻失败；先减半再观察，避免把利润全部还回去。"),
            true });
    }

    o.exitBasis = QStringLiteral("锚点：计划止盈 %1 ｜ 上方筹码压力 %2 ｜ MA5 %3 ｜ ATR14 %4（移动止盈 ≈ %5）｜ 含费成本 %6")
                      .arg(priceOrDash(planTp),
                           m.chipPressureLow > 0.0
                               ? QStringLiteral("%1~%2").arg(money(m.chipPressureLow), money(m.chipPressureHigh))
                               : QStringLiteral("无"),
                           m.ma5 > 0.0 ? money(m.ma5) : QStringLiteral("无"),
                           m.atrPct > 0.0 ? pct(m.atrPct) : QStringLiteral("无"),
                           money(trail), money(c.breakeven));
    if (!m.hasKline) {
        o.exitBasis += QStringLiteral("；无日 K，移动止盈退化为固定 3% 回撤线。");
    }
}

// ---------------------------------------------------------------- ③ 回本可行性
void buildLossRecovery(const Ctx& c, Insight& o)
{
    const TradeRecord& t = c.t;
    const Metrics& m = c.m;
    RecoveryPlan& rp = o.recovery;
    rp.breakeven = c.breakeven;

    // ---- 已了结：本笔不存在回本路径，只能由后续交易修复 ----
    if (c.closed) {
        rp.feasibility = Feasibility::Closed;
        rp.applicable = false;
        rp.score = 0;
        rp.requiredPct = qAbs(o.pnlPct);   // 同等本金需要赚回的百分比
        rp.verdict = QStringLiteral("已了结：本笔亏损 %1 元（%2%）。同等本金需在后续交易中盈利 %3% 才能抹平；"
                                    "不建议针对该股急于翻本。")
                         .arg(money(o.pnl), QString::number(o.pnlPct, 'f', 2),
                              QString::number(rp.requiredPct, 'f', 2));
        rp.conditions << QStringLiteral("把亏损视作沉没成本：下一笔仍按情绪闸门与计划区间执行，不因为想回本而放宽入场条件。");
        rp.conditions << QStringLiteral("单笔风险预算控制在总资金的 1% 以内；本笔按成本计亏损 %1%，已超出该预算时需降低下一笔仓位。")
                             .arg(pct(qAbs(o.pnlPct)));
        if (t.planStop > 0.0 && t.sellPrice > 0.0 && t.sellPrice < t.planStop) {
            rp.conditions << QStringLiteral("卖出价 %1 已跌破计划止损 %2，属于纪律性离场；复盘时记「执行正确」，不记「卖错」。")
                                 .arg(money(t.sellPrice), money(t.planStop));
        }
        if (m.postExitValid) {
            if (m.postExitPct < -5.0) {
                rp.conditions << QStringLiteral("离场后该股继续下跌 %1%，本次止损有效，保持同样的破位纪律。")
                                     .arg(pct(-m.postExitPct));
            } else if (m.postExitPct > 5.0) {
                rp.conditions << QStringLiteral("离场后该股反弹 %1%，改进点在**买点与仓位**，而不是否定止损。")
                                     .arg(pct(m.postExitPct));
            }
        }
        rp.blockers << QStringLiteral("本笔已了结，不存在「等待回本」的价格路径；任何补仓都必须作为一笔全新的交易重新评估。");
        return;
    }

    // ---- 持仓中：含费回本价与还需涨幅 ----
    rp.requiredPct = c.price > 0.0 ? (c.breakeven / c.price - 1.0) * 100.0 : 0.0;
    if (rp.requiredPct < 0.0) rp.requiredPct = 0.0;

    // ---- 可行性评分：赔率（还需涨幅）× 趋势 × 量能 × 周期 × 情绪 ----
    int score = 55;
    const double need = rp.requiredPct;
    if (need <= 2.0) score += 20;
    else if (need <= 4.0) score += 14;
    else if (need <= 7.0) score += 6;
    else if (need <= 10.0) score -= 4;
    else if (need <= 15.0) score -= 12;
    else score -= 22;

    const bool broken = t.planStop > 0.0 && c.price < t.planStop;
    if (broken) score -= 35;
    else if (t.planStop > 0.0 && c.price < t.planStop * 1.02) score -= 12;

    const double chg = c.hasDayChange ? c.dayChangePct : 0.0;
    if (m.hasKline) {
        if (m.ma5 > 0.0) score += c.price > m.ma5 ? 8 : -3;
        if (m.ma10 > 0.0 && c.price > m.ma10) score += 5;
        if (m.ma20 > 0.0) score += c.price > m.ma20 ? 3 : -5;
        if (m.ma5 > 0.0 && m.ma10 > 0.0) score += m.ma5 > m.ma10 ? 5 : -5;
        if (m.ma20 > 0.0 && m.ma20Prev > 0.0) score += m.ma20 > m.ma20Prev ? 4 : -4;
        if (m.volRatio >= 1.5 && chg > 0.0) score += 7;
        if (m.volRatio >= 1.5 && chg < 0.0) score -= 10;
        if (m.volRatio < 0.8 && chg < 0.0) score -= 3;
    }
    if (c.holdDays <= 2) score += 4;
    else if (c.holdDays > 10) score -= 10;
    else if (c.holdDays > 5) score -= 5;
    if (c.in.phaseKnown) {
        if (c.in.phase == MarketPhase::MainUp || c.in.phase == MarketPhase::Recovery) score += 5;
        else if (c.in.phase == MarketPhase::Retreat || c.in.phase == MarketPhase::IcePoint) score -= 8;
    }
    rp.score = qBound(0, score, 100);

    if (broken) rp.feasibility = Feasibility::Hard;
    else if (rp.score >= 65) rp.feasibility = Feasibility::Feasible;
    else if (rp.score >= 45) rp.feasibility = Feasibility::Conditional;
    else rp.feasibility = Feasibility::Hard;

    // ---- 修复目标：先到最近压力位（<= 回本价），再到含费回本价 ----
    double first = c.breakeven;
    const double floorPrice = c.price * 1.003;
    const double cands[] = { m.ma5, m.ma10, m.ma20, m.chipPressureLow };
    for (double v : cands) {
        if (v > floorPrice && v < first) first = v;
    }
    if (first >= c.breakeven * 0.999) {
        const double atrPoints = m.atrPct > 0.0 ? qBound(1.0, 0.5 * m.atrPct, 4.0) : 1.5;
        first = qMin(c.breakeven, c.price * (1.0 + atrPoints / 100.0));
    }
    rp.targetLow = qMax(first, c.price * 1.001);
    rp.targetHigh = c.breakeven;
    rp.profitTarget = (t.planTake > c.breakeven) ? t.planTake : 0.0;

    // ---- 预计交易日：ATR 折算（修复斜率 0.35×ATR%/日，按趋势与情绪修正）----
    double edge = qMax(0.6, m.atrPct > 0.0 ? m.atrPct * 0.35 : 1.0);
    if (m.hasKline && m.ma5 > 0.0 && c.price > m.ma5) edge *= 1.25;
    if (c.in.phaseKnown && (c.in.phase == MarketPhase::Retreat || c.in.phase == MarketPhase::IcePoint))
        edge *= 0.7;
    if (broken) edge *= 0.6;
    int days = int(std::ceil(need / edge));
    days = qBound(1, days, 60);
    rp.daysLow = qMax(1, int(std::ceil(days * 0.6)));
    rp.daysHigh = qMax(rp.daysLow + 1, int(std::ceil(days * 1.6)));

    // ---- 条件清单：价格 / 量能 / 筹码 / 情绪 / 时间 / 纪律 ----
    if (m.ma5 > 0.0) {
        rp.conditions << QStringLiteral("收盘站上 MA5 %1（现价 %2，差 %3%），且次日不跌回，才算修复启动。")
                             .arg(money(m.ma5), money(c.price),
                                  QString::number((m.ma5 / c.price - 1.0) * 100.0, 'f', 2));
    }
    if (m.ma10 > 0.0) {
        rp.conditions << QStringLiteral("第二目标 MA10 %1；两个交易日内站不上 MA10，修复时间按区间上沿估算。")
                             .arg(money(m.ma10));
    }
    if (m.volRatio > 0.0) {
        rp.conditions << QStringLiteral("反弹日量比需 ≥1.2（当前 %1）；若出现量比 ≥1.5 的放量阴线，视为修复失败。")
                             .arg(QString::number(m.volRatio, 'f', 2));
    }
    if (m.chipPressureLow > 0.0) {
        rp.conditions << QStringLiteral("上方 %1~%2 为筹码压力区，需放量通过才能回到成本；缩量触碰压力位容易二次回落。")
                             .arg(money(m.chipPressureLow), money(m.chipPressureHigh));
    }
    if (c.in.phaseKnown) {
        switch (c.in.phase) {
        case MarketPhase::MainUp:
            rp.conditions << QStringLiteral("情绪处于主升期，修复窗口相对顺畅，可给到区间上沿的时间。");
            break;
        case MarketPhase::Recovery:
            rp.conditions << QStringLiteral("情绪处于回暖期，反弹持续性有限，按 2~3 个交易日为一档观察。");
            break;
        case MarketPhase::Climax:
            rp.conditions << QStringLiteral("情绪处于高潮期，个股分化加剧；回本后优先减仓，不赌更高价。");
            break;
        case MarketPhase::Retreat:
        case MarketPhase::IcePoint:
            rp.conditions << QStringLiteral("情绪处于%1，历史上修复周期会拉长 1~2 倍；不建议用时间换空间。")
                                 .arg(c.in.phase == MarketPhase::Retreat ? QStringLiteral("退潮期")
                                                                         : QStringLiteral("冰点"));
            break;
        default:
            break;
        }
    }
    {
        const QDate deadline = c.in.asOf.isValid() ? c.in.asOf.addDays(7) : QDate::currentDate().addDays(7);
        rp.conditions << QStringLiteral("T+5（约 %1）仍未收复 MA10 或量能持续萎缩，视为回本失败，按纪律减仓。")
                             .arg(deadline.toString(QStringLiteral("MM-dd")));
    }
    if (t.planStop > 0.0) {
        rp.conditions << QStringLiteral("收盘跌破计划止损 %1 → 放弃回本，按纪律离场；禁止补仓摊薄成本。")
                             .arg(money(t.planStop));
    } else {
        rp.conditions << QStringLiteral("未记录计划止损：建议以现价下方 1×ATR（约 %1）作为纪律线，跌破即离场。")
                             .arg(money(qMax(0.01, c.price - (m.atr > 0.0 ? m.atr : c.price * 0.05))));
    }

    // ---- 硬约束：直接影响档位，不再用「条件」弱化 ----
    if (broken) {
        rp.blockers << QStringLiteral("现价 %1 已跌破计划止损 %2（%3%）：买入逻辑被破坏，止损纪律优先于回本。")
                           .arg(money(c.price), money(t.planStop),
                                pct((c.price / t.planStop - 1.0) * 100.0));
    }
    if (m.volRatio >= 1.5 && chg < 0.0) {
        rp.blockers << QStringLiteral("放量下跌（量比 %1，当日 %2）：主动性抛压，修复概率下调。")
                           .arg(QString::number(m.volRatio, 'f', 2), pct(chg));
    }
    if (c.holdDays > 10) {
        rp.blockers << QStringLiteral("已持有 %1 个交易日，远超打板策略的持有窗口，时间成本持续放大。")
                           .arg(c.holdDays);
    }
    if (!m.hasKline) {
        rp.blockers << QStringLiteral("缺少日 K：目标位与预计天数按费用口径与默认 ATR 折算，精度下降。");
    }

    // ---- 一句话判断 ----
    if (rp.feasibility == Feasibility::Feasible) {
        rp.verdict = QStringLiteral("回本可行：含费成本 %1（还需 +%2%），第一目标 %3，预计 %4~%5 个交易日；守住 %6 是前提。")
                         .arg(money(c.breakeven), QString::number(need, 'f', 2), money(rp.targetLow))
                         .arg(rp.daysLow).arg(rp.daysHigh)
                         .arg(t.planStop > 0.0 ? money(t.planStop) : QStringLiteral("纪律线"));
    } else if (rp.feasibility == Feasibility::Conditional) {
        rp.verdict = QStringLiteral("有条件回本：还需 +%1%，需先收复 %2 并放量到 1.2 倍，预计 %3~%4 个交易日；量能持续萎缩则窗口后移。")
                         .arg(QString::number(need, 'f', 2),
                              m.ma5 > 0.0 ? QStringLiteral("MA5 %1").arg(money(m.ma5))
                                          : QStringLiteral("第一压力位 %1").arg(money(rp.targetLow)))
                         .arg(rp.daysLow).arg(rp.daysHigh);
    } else if (broken) {
        rp.verdict = QStringLiteral("回本困难：已跌破计划止损 %1，本笔应优先执行纪律而非等待回本；若坚持持有，需先重新站上 %2 才谈修复。")
                         .arg(money(t.planStop),
                              m.ma5 > 0.0 && m.ma5 > c.price ? money(m.ma5) : money(rp.targetLow));
    } else {
        rp.verdict = QStringLiteral("回本困难：还需 +%1%，超出当前波动可支撑的修复速度（ATR %2）；建议减仓降低风险敞口，而不是等一次性回本。")
                         .arg(QString::number(need, 'f', 2),
                              m.atrPct > 0.0 ? pct(m.atrPct) : QStringLiteral("未知"));
    }
}

} // namespace

// ---------------------------------------------------------------- 对外接口
QString verdictName(Verdict v)
{
    switch (v) {
    case Verdict::Profit: return QStringLiteral("盈利");
    case Verdict::Loss:   return QStringLiteral("亏损");
    case Verdict::Flat:   return QStringLiteral("持平");
    default:              return QStringLiteral("待评估");
    }
}

QString feasibilityName(Feasibility f)
{
    switch (f) {
    case Feasibility::NotNeeded:   return QStringLiteral("无需回本");
    case Feasibility::Feasible:    return QStringLiteral("回本可行");
    case Feasibility::Conditional: return QStringLiteral("有条件回本");
    case Feasibility::Hard:        return QStringLiteral("回本困难");
    case Feasibility::Closed:      return QStringLiteral("已了结");
    default:                       return QStringLiteral("数据不足");
    }
}

Insight analyze(const Input& in)
{
    Insight o;
    const TradeRecord& t = in.trade;
    o.closed = t.closed();
    o.breakeven = breakevenPrice(t);

    const QDate asOf = in.asOf.isValid() ? in.asOf : QDate::currentDate();
    Input input = in;
    input.asOf = asOf;

    if (t.buyPrice <= 0.0 || t.qty <= 0) {
        o.valid = false;
        o.stateTag = QStringLiteral("记录不完整");
        o.headline = QStringLiteral("买入价或数量缺失，无法评估盈亏；请补全记录后重新诊断。");
        o.recovery.feasibility = Feasibility::Unknown;
        o.recovery.applicable = false;
        o.recovery.verdict = QStringLiteral("数据不足：缺少买入价或数量。");
        o.trace = QStringLiteral("口径：记录不完整，未进入推理。");
        return o;
    }

    o.valid = true;
    o.price = o.closed ? t.sellPrice : (in.hasQuote ? in.snapshot.price : 0.0);
    o.priced = o.closed || (in.hasQuote && o.price > 0.0);

    // 无行情：明确标注，不用昨收 / 成本冒充现价，也不编造盈亏
    if (!o.priced) {
        o.verdict = Verdict::Unknown;
        o.stateTag = QStringLiteral("待行情");
        o.headline = QStringLiteral("现价未就绪（停牌或行情未推送），暂不评估盈亏；计划止盈 %1 / 止损 %2 保持不变。")
                         .arg(priceOrDash(t.planTake), priceOrDash(t.planStop));
        o.reasons.append({ QStringLiteral("数据状态"),
                           QStringLiteral("未拿到该股现价，按约定不做 0 元估算；接入行情后本笔会自动重算。"),
                           0 });
        o.exitStrategy = QStringLiteral("计划挂单（待行情确认）");
        if (t.planTake > 0.0) {
            o.exitSteps.append({ QStringLiteral("到价兑现"),
                                 QStringLiteral("触及计划止盈 %1 兑现 50%").arg(money(t.planTake)),
                                 QStringLiteral("有行情前先按计划价挂条件单，不做主观改价。"), false });
        }
        if (t.planStop > 0.0) {
            o.exitSteps.append({ QStringLiteral("破位离场"),
                                 QStringLiteral("跌破计划止损 %1 清仓").arg(money(t.planStop)),
                                 QStringLiteral("纪律线不因缺少行情而失效。"), true });
        }
        o.recovery.feasibility = Feasibility::Unknown;
        o.recovery.applicable = false;      // 无现价：不展示回本价 / 修复区间，避免出现 0.00 假数据
        o.recovery.breakeven = o.breakeven;
        o.recovery.verdict = QStringLiteral("现价缺失：含费回本价 %1 仍可计算，但无法折算还需涨幅与修复时间。")
                                 .arg(money(o.breakeven));
        o.riskNotes << QStringLiteral("停牌或行情未就绪：本笔不参与浮动盈亏统计。");
        o.trace = QStringLiteral("口径：无现价，仅展示计划价与纪律线。");
        return o;
    }

    o.pnl = t.pnlAt(o.price);
    o.pnlPct = t.pnlPctAt(o.price);
    if (o.pnl > 0.005) o.verdict = Verdict::Profit;
    else if (o.pnl < -0.005) o.verdict = Verdict::Loss;
    else o.verdict = Verdict::Flat;

    const Metrics m = computeMetrics(input, o.price);

    Ctx c{ t, input, m, o.price, o.closed, o.priced, 0, o.breakeven, false, 0.0, false, o.pnl, o.pnlPct };
    c.holdDays = o.closed
        ? (m.barsHeld > 0 ? m.barsHeld : t.holdDays())
        : (m.barsSinceBuy > 0
               ? m.barsSinceBuy
               : qMax(0, t.buyDate.isValid() ? int(t.buyDate.daysTo(asOf)) : 0));
    c.brokenStop = t.planStop > 0.0 && o.price < t.planStop;
    c.hasDayChange = in.hasQuote && in.snapshot.prevClose > 0.0;
    if (c.hasDayChange) c.dayChangePct = in.snapshot.changePct;

    o.reasons = buildReasons(c);

    // ---- 盈亏结论 ----
    if (o.verdict == Verdict::Profit) {
        o.stateTag = o.closed ? QStringLiteral("已了结·盈利复盘")
                              : (t.planTake > 0.0 && o.price >= t.planTake
                                     ? QStringLiteral("浮盈·分批止盈")
                                     : QStringLiteral("浮盈·持有待涨"));
        buildProfitExit(c, o);
        o.headline = o.closed
            ? QStringLiteral("已了结盈利 %1 元（+%2%）：持有 %3 个交易日，%4。")
                  .arg(money(o.pnl), QString::number(o.pnlPct, 'f', 2))
                  .arg(c.holdDays)
                  .arg(t.executedAsPlanned() ? QStringLiteral("入场价落在计划区间内")
                                             : QStringLiteral("入场偏离计划，收益主要来自行情而非执行力"))
            : QStringLiteral("浮盈 %1 元（+%2%）：%3，建议按「%4」执行。")
                  .arg(money(o.pnl), QString::number(o.pnlPct, 'f', 2))
                  .arg(t.planTake > 0.0 && o.price >= t.planTake
                           ? QStringLiteral("已进入计划止盈区")
                           : QStringLiteral("尚未触及计划止盈"))
                  .arg(o.exitStrategy);
        o.recovery.feasibility = Feasibility::NotNeeded;
        o.recovery.applicable = false;
        o.recovery.score = 100;
        o.recovery.breakeven = c.breakeven;
        o.recovery.requiredPct = 0.0;
        o.recovery.verdict = o.closed
            ? QStringLiteral("无需回本：本笔已实现盈利 %1 元；复盘重点是卖点与仓位，而不是回本。").arg(money(o.pnl))
            : QStringLiteral("无需回本：本笔浮盈 %1 元；要守住的是含费成本 %2，跌破即离场，杜绝盈利单变亏损单。")
                  .arg(money(o.pnl), money(c.breakeven));
        o.recovery.conditions << QStringLiteral("止盈的第一原则是「不把盈利还回去」：保本线挂在含费成本 %1 上方。")
                                     .arg(money(c.breakeven));
        o.recovery.conditions << QStringLiteral("分批兑现而不是一次清仓：先用 50% 锁定收益，剩余仓位用移动止盈吃趋势。");
        if (o.closed) {
            o.recovery.conditions << QStringLiteral("已了结的交易不再受价格波动影响；把本次卖点与计划止盈的偏差记入复盘。");
        }
    } else if (o.verdict == Verdict::Loss) {
        buildLossRecovery(c, o);
        o.stateTag = o.closed ? QStringLiteral("已了结·亏损复盘")
                              : (c.brokenStop ? QStringLiteral("浮亏·已破位")
                                              : (o.recovery.feasibility == Feasibility::Feasible
                                                     ? QStringLiteral("浮亏·可修复")
                                                     : (o.recovery.feasibility == Feasibility::Conditional
                                                            ? QStringLiteral("浮亏·有条件")
                                                            : QStringLiteral("浮亏·难度大"))));
        o.headline = o.closed
            ? QStringLiteral("已了结亏损 %1 元（%2%）：需后续同等本金盈利 %3% 才能抹平；本笔的重点是找出入场/止损的执行偏差。")
                  .arg(money(o.pnl), QString::number(o.pnlPct, 'f', 2),
                       QString::number(qAbs(o.pnlPct), 'f', 2))
            : QStringLiteral("浮亏 %1 元（%2%）：%3")
                  .arg(money(o.pnl), QString::number(o.pnlPct, 'f', 2),
                       o.recovery.verdict);
        o.exitStrategy = c.brokenStop ? QStringLiteral("止损纪律：破位离场，不等待回本")
                                      : QStringLiteral("减仓与止损纪律（先控制风险，再谈回本）");
        if (c.brokenStop) {
            o.exitSteps.append({ QStringLiteral("收盘确认破位"),
                                 QStringLiteral("现价 %1 < 计划止损 %2").arg(money(o.price), money(t.planStop)),
                                 QStringLiteral("跌破计划止损即买入逻辑被破坏，不以「还没回本」为由继续持有。"),
                                 true });
            o.exitSteps.append({ QStringLiteral("清仓或降至观察仓"),
                                 QStringLiteral("次日不能收回 %1 则清仓").arg(money(t.planStop)),
                                 QStringLiteral("打板亏损单的第一目标是止住回撤，不是等回本。"),
                                 true });
        } else {
            o.exitSteps.append({ QStringLiteral("持有观察"),
                                 QStringLiteral("守住 %1；站上 %2 后只做重新评估，不加仓摊薄")
                                     .arg(t.planStop > 0.0 ? money(t.planStop) : money(o.price * 0.95),
                                          m.ma5 > 0.0 ? money(m.ma5) : money(o.recovery.targetLow)),
                                 QStringLiteral("亏损单的第一原则是不扩大风险敞口；结构修复后才谈下一步。"),
                                 false });
            o.exitSteps.append({ QStringLiteral("减半条件"),
                                 QStringLiteral("跌破 %1 或出现量比 ≥1.5 的放量阴线")
                                     .arg(t.planStop > 0.0 ? money(t.planStop) : money(o.price * 0.97)),
                                 QStringLiteral("抛压主动时先降风险敞口，避免小亏拖成大亏。"),
                                 true });
            o.exitSteps.append({ QStringLiteral("时间止损"),
                                 QStringLiteral("持有满 5 个交易日仍未收复 MA10 → 减半"),
                                 QStringLiteral("打板逻辑的修复窗口有限，超期即承认判断错误。"),
                                 true });
        }
        o.exitBasis = QStringLiteral("锚点：含费成本 %1 ｜ 计划止损 %2 ｜ MA5 %3 ｜ ATR14 %4 ｜ 上方筹码压力 %5")
                          .arg(money(c.breakeven), priceOrDash(t.planStop),
                               m.ma5 > 0.0 ? money(m.ma5) : QStringLiteral("无"),
                               m.atrPct > 0.0 ? pct(m.atrPct) : QStringLiteral("无"),
                               m.chipPressureLow > 0.0
                                   ? QStringLiteral("%1~%2").arg(money(m.chipPressureLow), money(m.chipPressureHigh))
                                   : QStringLiteral("无"));
    } else {
        o.stateTag = QStringLiteral("持平");
        o.headline = QStringLiteral("持平（%1 元 / %2%）：扣除双边费用后基本打平，按保本纪律处理。")
                         .arg(money(o.pnl), QString::number(o.pnlPct, 'f', 2));
        o.recovery.feasibility = Feasibility::NotNeeded;
        o.recovery.applicable = false;
        o.recovery.score = 60;
        o.recovery.breakeven = c.breakeven;
        o.recovery.verdict = QStringLiteral("无需回本：现价 %1 与含费成本 %2 基本重合，优先考虑把仓位换到更有把握的标的。")
                                 .arg(money(o.price), money(c.breakeven));
        o.exitStrategy = QStringLiteral("保本处理：不恋战");
        o.exitSteps.append({ QStringLiteral("触及成本即离场"),
                             QStringLiteral("现价 ≥ %1 时清仓").arg(money(c.breakeven)),
                             QStringLiteral("持平单继续持有的机会成本高于收益。"), false });
        o.exitBasis = QStringLiteral("含费成本 %1 为唯一参照，无趋势优势时不加仓。").arg(money(c.breakeven));
    }

    // ---- 风险提示：纪律 / 数据 / 执行偏差 ----
    if (c.brokenStop) {
        o.riskNotes << QStringLiteral("已跌破计划止损 %1：止损纪律未执行或未记录，是本笔最大的可控风险。")
                           .arg(money(t.planStop));
    }
    if (!t.executedAsPlanned() && t.planBuy > 0.0) {
        o.riskNotes << QStringLiteral("买入价相对计划中枢偏离超过 3%：执行偏差会直接改变盈亏比，复盘需单列。");
    }
    if (!o.closed && c.holdDays > 5) {
        o.riskNotes << QStringLiteral("持有 %1 个交易日，超出打板兑现窗口；继续等待需要按波段逻辑重新评估，而不是沿用打板假设。")
                           .arg(c.holdDays);
    }
    if (!m.hasKline) {
        o.riskNotes << QStringLiteral("无日 K：趋势 / 量能 / ATR 维度降级，止盈止损按计划价与费用口径给出。");
    }
    if (o.pnlPct < 0.0 && o.pnlPct > -1.0) {
        o.riskNotes << QStringLiteral("亏损幅度小于 1%：很可能由双边费用造成，方向判断本身并未明显错误。");
    }
    if (m.postExitValid && m.postExitPct > 5.0) {
        o.riskNotes << QStringLiteral("离场后仍有 %1% 涨幅：下次同类标的可用移动止盈保留 1/3 仓位。").arg(pct(m.postExitPct));
    }

    // ---- 数据口径（可核对）----
    o.trace = QStringLiteral("数据：%1日 K %2 根 · %3 %4%5 ｜ 口径：佣金万2.5双边 + 卖出印花税千1，含费回本价 %6")
                  .arg(m.rawPrices ? QString() : QStringLiteral("前复权（未能与成交价同口径）"),
                       m.hasKline ? QString::number(m.bars) : QStringLiteral("0"))
                  .arg(o.closed ? QStringLiteral("卖出价") : QStringLiteral("现价"),
                       money(o.price),
                       c.hasDayChange ? QStringLiteral("（当日 %1）").arg(pct(c.dayChangePct)) : QString())
                  .arg(money(c.breakeven));
    return o;
}

} // namespace TradeAI
