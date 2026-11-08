#include "review/DecisionReview.h"

#include "core/ChipDistribution.h"
#include "core/TradePlan.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace DecisionReview {
namespace {

const char* kYes = "是";
const char* kNo  = "否";

QString f(double v, int d = 2) { return QString::number(v, 'f', d); }
QString pct(double v, int d = 2) { return f(v, d) + QStringLiteral("%"); }
QString yn(bool b) { return QString::fromUtf8(b ? kYes : kNo); }

QString dateText(const QDate& d)
{
    return d.isValid() ? d.toString(Qt::ISODate) : QStringLiteral("—");
}

QJsonArray evidencesToJson(const QVector<Evidence>& v)
{
    QJsonArray a;
    for (const Evidence& e : v) a.append(e.toJson());
    return a;
}

QVector<Evidence> evidencesFromJson(const QJsonValue& v)
{
    QVector<Evidence> out;
    for (const QJsonValue& item : v.toArray()) out.append(Evidence::fromJson(item.toObject()));
    return out;
}

QJsonArray stringsToJson(const QStringList& v)
{
    QJsonArray a;
    for (const QString& s : v) a.append(s);
    return a;
}

QStringList stringsFromJson(const QJsonValue& v)
{
    QStringList out;
    for (const QJsonValue& s : v.toArray()) out << s.toString();
    return out;
}

Evidence unknown(const QString& name, const QString& role, const QString& rule,
                 const QString& source, const QString& why)
{
    Evidence e;
    e.name = name;
    e.role = role;
    e.rule = rule;
    e.source = source;
    e.value = QStringLiteral("数据缺失");
    e.caveat = why;
    e.known = false;
    e.passed = false;
    return e;
}

QString planPrices(const TradePlan& p)
{
    return QStringLiteral("买 %1~%2 / 止盈 %3 / 止损 %4")
        .arg(f(p.buyLow), f(p.buyHigh), f(p.takeProfitLow), f(p.stopLossHigh));
}

const KlineSeries* klineFor(const Input& in, const QString& code)
{
    auto it = in.klines.constFind(code);
    return it == in.klines.constEnd() ? nullptr : &it.value();
}

// 日 K 来源窗口统一写法，保证每条证据都能回溯到「哪份数据、截到哪天、几根样本」
QString klineSource(const QDate& date, int sample, bool raw)
{
    return QStringLiteral("日K%1 · 截至 %2 · 近 %3 根（本地缓存）")
        .arg(raw ? QStringLiteral("不复权") : QStringLiteral("前复权"))
        .arg(dateText(date), QString::number(sample));
}

} // namespace

// ---------------------------------------------------------------- 固定文案
QString purposeText()
{
    return QStringLiteral(
        "这里只回答收盘后的三个问题：① 当时触发买入 / 离场条件的核心条件分别是什么、数值多少、"
        "门槛多少、数据从哪来；② 信号之后实际走势与计划预期差了多少；③ 没执行的信号、持仓的隐性风险、"
        "板块资金流，事后各自是什么客观结果。"
        "全部结论都带可核对的数值与判定式；数据缺失一律标注缺失，不补 0、不推测。"
        "本页不预测涨跌、不生成新的买卖或仓位指令、不修改任何交易阈值；"
        "「跳过的机会」只是事后统计，不评价当时是否该做，也不构成任何下一步建议。");
}

QStringList criteriaLines()
{
    QStringList l;
    l << QStringLiteral("信号执行标记：recommend_plan.actionable = true（闸门放行且未被竞价 / 风控降级）");
    l << QStringLiteral("信号档位：signalLevel ≥ 2 且 positionPct > 0 才算给出仓位（0 无 / 1 观察 / 2 试仓 / 3 建仓 / 4 主攻）");
    l << QStringLiteral("情绪硬闸门：tradeAllowed = true（跌停家数、炸板率等硬否决条件未触发；原始输入见当日情绪留档）");
    l << QStringLiteral("仓位上限：plan.positionPct ≤ sentiment.positionCapPct");
    l << QStringLiteral("多因子过滤：factorReject 为空（未被组合规则淘汰）");
    l << QStringLiteral("计划区间自洽：buyHigh < takeProfitLow 且 stopLossHigh < buyLow");
    l << QStringLiteral("均线收敛度 = (max(MA5,MA10,MA20) − min) / MA20 × 100%；收敛判定 ≤ %1%（前复权日K，近 20 根）")
         .arg(f(Criteria::kMaConvergePct));
    l << QStringLiteral("RSI（Wilder，与图表副图同口径）：RSI%1 ≥ %2 判极端超买，≥ %3 判超买；RSI 不是现行买入信号的触发字段")
         .arg(Criteria::kRsiPeriod).arg(f(Criteria::kRsiExtreme, 1)).arg(f(Criteria::kRsiHigh, 1));
    l << QStringLiteral("资金流入：主力净流入 > %1 亿元（个股详情接口 fflow，有则用）；"
                        "无该字段时改用封单额/成交额 ≥ %2 的量能代理并明确标注，两者都缺即标数据缺失")
         .arg(f(Criteria::kFlowPositiveYi, 1)).arg(f(Criteria::kSealProxyMin));
    l << QStringLiteral("假企稳 = 均线收敛度 ≤ %1% 且 最近 %2 个 %3 日窗口低点严格递减 且 末窗低点低于首窗（斜率 < 0%）")
         .arg(f(Criteria::kMaConvergePct))
         .arg(Criteria::kSwingSamples).arg(Criteria::kSwingWindow);
    l << QStringLiteral("上方多层阻力 = 当前价上方、筹码占比 ≥ %1 的密集峰 ≥ %2 层（不复权日K重建成本分布）")
         .arg(pct(Criteria::kChipMinShare * 100.0, 0)).arg(Criteria::kChipLayers);
    l << QStringLiteral("事后观察窗口 = 信号日之后最多 %1 根日 K（不含信号日）；MFE = 最高/基准−1，MAE = 最低/基准−1")
         .arg(Criteria::kOutcomeHorizon);
    l << QStringLiteral("目标偏差 = (窗口最高 − 计划止盈下沿) / 计划止盈下沿 × 100%；"
                        "触及止盈 = 最高 ≥ 止盈下沿；触及止损 = 最低 ≤ 止损上沿");
    l << QStringLiteral("跳过判定：交易日志中不存在 code 相同且 buyDate ∈ [信号日, 信号日+1 自然日] 的买入记录");
    l << QStringLiteral("假设收益（仅事后对照）= 末收盘×(1−卖出费率) / (计划买入中枢×(1+买入费率)) − 1；"
                        "不含滑点、成交概率、T+1 限制与资金占用");
    l << QStringLiteral("领涨 / 领跌 = 当日板块涨幅降序前 %1 / 后 %1；资金流取东财 f62（元→亿元）与 f184（占成交额 %）")
         .arg(Criteria::kSectorTopN);
    return l;
}

// ---------------------------------------------------------------- 纯指标
namespace Metric {

QVector<double> rsi(const QVector<double>& close, int period)
{
    QVector<double> out(close.size(), qQNaN());
    if (close.size() <= period || period <= 0) return out;
    double avgGain = 0.0, avgLoss = 0.0;
    for (int i = 1; i < close.size(); ++i) {
        const double diff = close.at(i) - close.at(i - 1);
        const double gain = qMax(0.0, diff);
        const double loss = qMax(0.0, -diff);
        if (i <= period) {
            avgGain += gain;
            avgLoss += loss;
            if (i == period) {
                avgGain /= period;
                avgLoss /= period;
                out[i] = (avgGain + avgLoss) > 0.0 ? avgGain / (avgGain + avgLoss) * 100.0 : 50.0;
            }
        } else {
            avgGain = (avgGain * (period - 1) + gain) / period;
            avgLoss = (avgLoss * (period - 1) + loss) / period;
            out[i] = (avgGain + avgLoss) > 0.0 ? avgGain / (avgGain + avgLoss) * 100.0 : 50.0;
        }
    }
    return out;
}

QVector<double> closes(const KlineSeries& k, const QDate& asOf, int need)
{
    const QVector<Candle>& v = k.sliceTo(asOf).candles;
    QVector<double> out;
    const int from = qMax(0, v.size() - qMax(need, 1));
    for (int i = from; i < v.size(); ++i) out.append(v.at(i).close);
    return out;
}

MaInfo ma(const KlineSeries& k, const QDate& asOf)
{
    MaInfo m;
    const QVector<double> c = closes(k, asOf, 20);
    m.sample = c.size();
    if (c.size() < 20) return m;
    auto avgOf = [&c](int n) {
        double sum = 0.0;
        for (int i = c.size() - n; i < c.size(); ++i) sum += c.at(i);
        return sum / n;
    };
    m.ma5 = avgOf(5);
    m.ma10 = avgOf(10);
    m.ma20 = avgOf(20);
    const double hi = qMax(m.ma5, qMax(m.ma10, m.ma20));
    const double lo = qMin(m.ma5, qMin(m.ma10, m.ma20));
    m.spreadPct = m.ma20 > 0.0 ? (hi - lo) / m.ma20 * 100.0 : 0.0;
    m.converged = m.spreadPct <= Criteria::kMaConvergePct;
    m.known = true;
    return m;
}

LowInfo swingLows(const KlineSeries& k, const QDate& asOf)
{
    LowInfo o;
    o.window = Criteria::kSwingWindow;
    const QVector<Candle> v = k.sliceTo(asOf).candles;
    const int need = Criteria::kSwingWindow * Criteria::kSwingSamples;
    if (v.size() < need) return o;
    for (int w = 0; w < Criteria::kSwingSamples; ++w) {
        double low = v.at(v.size() - need + w * Criteria::kSwingWindow).low;
        for (int i = 1; i < Criteria::kSwingWindow; ++i)
            low = qMin(low, v.at(v.size() - need + w * Criteria::kSwingWindow + i).low);
        o.lows.append(low);
    }
    o.declining = true;
    for (int i = 1; i < o.lows.size(); ++i)
        if (!(o.lows.at(i) < o.lows.at(i - 1))) o.declining = false;
    o.slopePct = o.lows.first() > 0.0
                     ? (o.lows.last() - o.lows.first()) / o.lows.first() * 100.0
                     : 0.0;
    o.known = true;
    return o;
}

ChipInfo chipLayers(const KlineSeries& k, const QDate& asOf, double price)
{
    ChipInfo o;
    const KlineSeries cut = k.sliceTo(asOf);
    const QVector<Candle>& raw = cut.hasRaw() ? cut.rawCandles : cut.candles;
    o.sample = raw.size();
    if (raw.size() < 20) return o;
    const double px = price > 0.0 ? price : raw.last().close;
    const Chip::Distribution d = Chip::build(raw, px);
    if (!d.valid) return o;

    QVector<Chip::Peak> above;
    for (const Chip::Peak& p : d.peaks) {
        if (p.center > px && p.share >= Criteria::kChipMinShare) above.append(p);
    }
    std::sort(above.begin(), above.end(), [](const Chip::Peak& a, const Chip::Peak& b) {
        return a.center < b.center;
    });
    double lastCenter = -1.0;
    for (const Chip::Peak& p : above) {
        // 峰区高度重叠时按同一层计，避免把一座宽峰拆成「多层阻力」
        if (lastCenter > 0.0 && qAbs(p.center - lastCenter) / lastCenter < 0.005) continue;
        lastCenter = p.center;
        ++o.layers;
        o.detail << QStringLiteral("%1~%2 元（峰 %3 元 · 占比 %4）")
                        .arg(f(p.low), f(p.high), f(p.center), pct(p.share * 100.0, 1));
    }
    o.known = true;
    return o;
}

FollowUp followUp(const KlineSeries& k, const QDate& signalDate, int horizon)
{
    FollowUp o;
    // 事后走势刻意不按复盘日截断：复盘历史决策日时，用的正是信号日之后的日 K。
    // 指标类计算（MA / RSI / 筹码）仍一律 sliceTo(asOf)，两者口径分开，避免未来函数。
    const QVector<Candle>& v = k.hasRaw() ? k.rawCandles : k.candles;
    if (v.isEmpty() || horizon <= 0) return o;

    int baseIdx = -1;
    for (int i = v.size() - 1; i >= 0; --i) {
        if (v.at(i).date.isValid() && v.at(i).date <= signalDate) { baseIdx = i; break; }
    }
    if (baseIdx < 0) return o;
    o.base = v.at(baseIdx).close;
    if (o.base <= 0.0) return o;

    o.maxHigh = o.minLow = o.lastClose = o.base;
    for (int i = baseIdx + 1; i < v.size() && o.bars < horizon; ++i) {
        if (v.at(i).date.isValid() && v.at(i).date <= signalDate) continue;
        if (o.bars == 0) o.firstDate = v.at(i).date;
        o.maxHigh = qMax(o.maxHigh, v.at(i).high > 0.0 ? v.at(i).high : v.at(i).close);
        o.minLow = qMin(o.minLow, v.at(i).low > 0.0 ? v.at(i).low : v.at(i).close);
        o.lastClose = v.at(i).close;
        o.lastDate = v.at(i).date;
        ++o.bars;
    }
    o.known = o.bars > 0;
    return o;
}

} // namespace Metric

// ---------------------------------------------------------------- 序列化
QJsonObject Evidence::toJson() const
{
    QJsonObject o;
    o["name"] = name;   o["role"] = role;      o["value"] = value;
    o["rule"] = rule;   o["known"] = known;    o["passed"] = passed;
    o["source"] = source; o["caveat"] = caveat;
    return o;
}
Evidence Evidence::fromJson(const QJsonObject& o)
{
    Evidence e;
    e.name = o.value("name").toString();   e.role = o.value("role").toString();
    e.value = o.value("value").toString(); e.rule = o.value("rule").toString();
    e.known = o.value("known").toBool();   e.passed = o.value("passed").toBool();
    e.source = o.value("source").toString(); e.caveat = o.value("caveat").toString();
    return e;
}

QJsonObject SectorFlowRow::toJson() const
{
    QJsonObject o;
    o["code"] = code; o["name"] = name; o["changePct"] = changePct;
    o["mainNetYi"] = mainNetYi; o["mainPct"] = mainPct;
    o["flowKnown"] = flowKnown; o["tag"] = tag;
    return o;
}
SectorFlowRow SectorFlowRow::fromJson(const QJsonObject& o)
{
    SectorFlowRow r;
    r.code = o.value("code").toString(); r.name = o.value("name").toString();
    r.changePct = o.value("changePct").toDouble(); r.mainNetYi = o.value("mainNetYi").toDouble();
    r.mainPct = o.value("mainPct").toDouble(); r.flowKnown = o.value("flowKnown").toBool();
    r.tag = o.value("tag").toString();
    return r;
}

QJsonObject SignalTrace::toJson() const
{
    QJsonObject o;
    o["date"] = date.toString(Qt::ISODate); o["code"] = code; o["name"] = name;
    o["side"] = side; o["origin"] = origin; o["level"] = level;
    o["expectation"] = expectation; o["conditions"] = evidencesToJson(conditions);
    o["knownCount"] = knownCount; o["passedCount"] = passedCount;
    o["complete"] = complete; o["verdict"] = verdict;
    return o;
}
SignalTrace SignalTrace::fromJson(const QJsonObject& o)
{
    SignalTrace s;
    s.date = QDate::fromString(o.value("date").toString(), Qt::ISODate);
    s.code = o.value("code").toString(); s.name = o.value("name").toString();
    s.side = o.value("side").toString(); s.origin = o.value("origin").toString();
    s.level = o.value("level").toString(); s.expectation = o.value("expectation").toString();
    s.conditions = evidencesFromJson(o.value("conditions"));
    s.knownCount = o.value("knownCount").toInt(); s.passedCount = o.value("passedCount").toInt();
    s.complete = o.value("complete").toBool(); s.verdict = o.value("verdict").toString();
    return s;
}

QJsonObject OutcomeDeviation::toJson() const
{
    QJsonObject o;
    o["code"] = code; o["name"] = name;
    o["signalDate"] = signalDate.toString(Qt::ISODate);
    o["lastBarDate"] = lastBarDate.toString(Qt::ISODate);
    o["bars"] = bars; o["basePrice"] = basePrice; o["targetPrice"] = targetPrice;
    o["stopPrice"] = stopPrice; o["maxHigh"] = maxHigh; o["minLow"] = minLow;
    o["lastClose"] = lastClose; o["mfePct"] = mfePct; o["maePct"] = maePct;
    o["closePct"] = closePct; o["targetDeviationPct"] = targetDeviationPct;
    o["targetTouched"] = targetTouched; o["stopTouched"] = stopTouched;
    o["known"] = known; o["dataNote"] = dataNote; o["summary"] = summary;
    o["checks"] = evidencesToJson(checks);
    return o;
}
OutcomeDeviation OutcomeDeviation::fromJson(const QJsonObject& o)
{
    OutcomeDeviation d;
    d.code = o.value("code").toString(); d.name = o.value("name").toString();
    d.signalDate = QDate::fromString(o.value("signalDate").toString(), Qt::ISODate);
    d.lastBarDate = QDate::fromString(o.value("lastBarDate").toString(), Qt::ISODate);
    d.bars = o.value("bars").toInt(); d.basePrice = o.value("basePrice").toDouble();
    d.targetPrice = o.value("targetPrice").toDouble(); d.stopPrice = o.value("stopPrice").toDouble();
    d.maxHigh = o.value("maxHigh").toDouble(); d.minLow = o.value("minLow").toDouble();
    d.lastClose = o.value("lastClose").toDouble(); d.mfePct = o.value("mfePct").toDouble();
    d.maePct = o.value("maePct").toDouble(); d.closePct = o.value("closePct").toDouble();
    d.targetDeviationPct = o.value("targetDeviationPct").toDouble();
    d.targetTouched = o.value("targetTouched").toBool(); d.stopTouched = o.value("stopTouched").toBool();
    d.known = o.value("known").toBool(); d.dataNote = o.value("dataNote").toString();
    d.summary = o.value("summary").toString(); d.checks = evidencesFromJson(o.value("checks"));
    return d;
}

QJsonObject SkippedOpportunity::toJson() const
{
    QJsonObject o;
    o["code"] = code; o["name"] = name;
    o["signalDate"] = signalDate.toString(Qt::ISODate);
    o["skipRule"] = skipRule; o["buyMid"] = buyMid;
    o["targetPrice"] = targetPrice; o["stopPrice"] = stopPrice;
    o["bars"] = bars; o["lastBarDate"] = lastBarDate.toString(Qt::ISODate);
    o["maxHigh"] = maxHigh; o["minLow"] = minLow; o["lastClose"] = lastClose;
    o["mfePct"] = mfePct; o["maePct"] = maePct; o["closePct"] = closePct;
    o["hypotheticalPct"] = hypotheticalPct;
    o["targetTouched"] = targetTouched; o["stopTouched"] = stopTouched;
    o["known"] = known; o["basis"] = basis; o["caveat"] = caveat; o["dataNote"] = dataNote;
    return o;
}
SkippedOpportunity SkippedOpportunity::fromJson(const QJsonObject& o)
{
    SkippedOpportunity s;
    s.code = o.value("code").toString(); s.name = o.value("name").toString();
    s.signalDate = QDate::fromString(o.value("signalDate").toString(), Qt::ISODate);
    s.skipRule = o.value("skipRule").toString(); s.buyMid = o.value("buyMid").toDouble();
    s.targetPrice = o.value("targetPrice").toDouble(); s.stopPrice = o.value("stopPrice").toDouble();
    s.bars = o.value("bars").toInt();
    s.lastBarDate = QDate::fromString(o.value("lastBarDate").toString(), Qt::ISODate);
    s.maxHigh = o.value("maxHigh").toDouble(); s.minLow = o.value("minLow").toDouble();
    s.lastClose = o.value("lastClose").toDouble(); s.mfePct = o.value("mfePct").toDouble();
    s.maePct = o.value("maePct").toDouble(); s.closePct = o.value("closePct").toDouble();
    s.hypotheticalPct = o.value("hypotheticalPct").toDouble();
    s.targetTouched = o.value("targetTouched").toBool(); s.stopTouched = o.value("stopTouched").toBool();
    s.known = o.value("known").toBool(); s.basis = o.value("basis").toString();
    s.caveat = o.value("caveat").toString(); s.dataNote = o.value("dataNote").toString();
    return s;
}

QJsonObject HoldingRisk::toJson() const
{
    QJsonObject o;
    o["code"] = code; o["name"] = name; o["type"] = type; o["level"] = level;
    o["triggered"] = triggered; o["known"] = known;
    o["conditions"] = evidencesToJson(conditions); o["conclusion"] = conclusion;
    return o;
}
HoldingRisk HoldingRisk::fromJson(const QJsonObject& o)
{
    HoldingRisk r;
    r.code = o.value("code").toString(); r.name = o.value("name").toString();
    r.type = o.value("type").toString(); r.level = o.value("level").toString();
    r.triggered = o.value("triggered").toBool(); r.known = o.value("known").toBool();
    r.conditions = evidencesFromJson(o.value("conditions"));
    r.conclusion = o.value("conclusion").toString();
    return r;
}

QJsonObject SectorRotation::toJson() const
{
    QJsonObject o;
    o["date"] = date.toString(Qt::ISODate);
    o["fetchedAt"] = fetchedAt.toString(Qt::ISODate);
    o["available"] = available; o["reason"] = reason; o["source"] = source;
    o["criteria"] = criteria;
    QJsonArray a;
    for (const SectorFlowRow& r : rows) a.append(r.toJson());
    o["rows"] = a;
    return o;
}
SectorRotation SectorRotation::fromJson(const QJsonObject& o)
{
    SectorRotation s;
    s.date = QDate::fromString(o.value("date").toString(), Qt::ISODate);
    s.fetchedAt = QDateTime::fromString(o.value("fetchedAt").toString(), Qt::ISODate);
    s.available = o.value("available").toBool(); s.reason = o.value("reason").toString();
    s.source = o.value("source").toString(); s.criteria = o.value("criteria").toString();
    for (const QJsonValue& v : o.value("rows").toArray())
        s.rows.append(SectorFlowRow::fromJson(v.toObject()));
    return s;
}

QJsonObject Report::toJson() const
{
    QJsonObject o;
    o["date"] = date.toString(Qt::ISODate);
    o["generatedAt"] = generatedAt.toString(Qt::ISODate);
    o["valid"] = valid; o["purpose"] = purpose;
    o["gaps"] = stringsToJson(gaps); o["criteria"] = stringsToJson(criteria);
    QJsonArray a;
    for (const SignalTrace& s : signalTraces) a.append(s.toJson());
    o["signals"] = a;
    a = QJsonArray();
    for (const OutcomeDeviation& d : outcomes) a.append(d.toJson());
    o["outcomes"] = a;
    a = QJsonArray();
    for (const SkippedOpportunity& s : skipped) a.append(s.toJson());
    o["skipped"] = a;
    a = QJsonArray();
    for (const HoldingRisk& r : risks) a.append(r.toJson());
    o["risks"] = a;
    o["sectors"] = sectors.toJson();
    return o;
}
Report Report::fromJson(const QJsonObject& o)
{
    Report r;
    r.date = QDate::fromString(o.value("date").toString(), Qt::ISODate);
    r.generatedAt = QDateTime::fromString(o.value("generatedAt").toString(), Qt::ISODate);
    r.valid = o.value("valid").toBool(); r.purpose = o.value("purpose").toString();
    r.gaps = stringsFromJson(o.value("gaps")); r.criteria = stringsFromJson(o.value("criteria"));
    for (const QJsonValue& v : o.value("signals").toArray())
        r.signalTraces.append(SignalTrace::fromJson(v.toObject()));
    for (const QJsonValue& v : o.value("outcomes").toArray())
        r.outcomes.append(OutcomeDeviation::fromJson(v.toObject()));
    for (const QJsonValue& v : o.value("skipped").toArray())
        r.skipped.append(SkippedOpportunity::fromJson(v.toObject()));
    for (const QJsonValue& v : o.value("risks").toArray())
        r.risks.append(HoldingRisk::fromJson(v.toObject()));
    r.sectors = SectorRotation::fromJson(o.value("sectors").toObject());
    return r;
}

// ---------------------------------------------------------------- 复盘构建
namespace {

QString levelText(int level)
{
    return QStringLiteral("%1（%2 级）").arg(signalLevelName(level)).arg(level);
}

// 当时可复算的 K 线指标（均线收敛 / RSI），逐条标清「是否现行触发字段」
void appendKlineEvidences(QVector<Evidence>& out, const Input& in, const QString& code)
{
    const KlineSeries* k = klineFor(in, code);
    const QString srcMa = klineSource(in.asOf, 20, false);
    const QString srcRsi = klineSource(in.asOf, 24 + 1, false);
    if (!k || !k->valid()) {
        out << unknown(QStringLiteral("均线收敛度"), QStringLiteral("观测指标"),
                       QStringLiteral("(max−min)/MA20×100 ≤ %1%").arg(f(Criteria::kMaConvergePct)),
                       srcMa, QStringLiteral("该标的当日日 K 未缓存，不推测均线值"));
        out << unknown(QStringLiteral("RSI（入场时过热度）"), QStringLiteral("观测指标"),
                       QStringLiteral("RSI%1 ≥ %2 判极端超买")
                           .arg(Criteria::kRsiPeriod).arg(f(Criteria::kRsiExtreme, 1)),
                       srcRsi, QStringLiteral("该标的当日日 K 未缓存，不推测 RSI 值"));
        return;
    }

    const Metric::MaInfo m = Metric::ma(*k, in.asOf);
    if (m.known) {
        Evidence e;
        e.name = QStringLiteral("均线收敛度");
        e.role = QStringLiteral("观测指标");
        e.value = QStringLiteral("MA5 %1 / MA10 %2 / MA20 %3 · 收敛度 %4")
                      .arg(f(m.ma5), f(m.ma10), f(m.ma20), pct(m.spreadPct));
        e.rule = QStringLiteral("(max−min)/MA20×100 ≤ %1%").arg(f(Criteria::kMaConvergePct));
        e.known = true; e.passed = m.converged; e.source = srcMa;
        e.caveat = QStringLiteral("现行买入信号不由该指标触发；此处只核对当时的形态数值");
        out << e;
    } else {
        out << unknown(QStringLiteral("均线收敛度"), QStringLiteral("观测指标"),
                       QStringLiteral("(max−min)/MA20×100 ≤ %1%").arg(f(Criteria::kMaConvergePct)),
                       srcMa, QStringLiteral("前复权日 K 不足 20 根（实际 %1 根）").arg(m.sample));
    }

    const QVector<double> c6 = Metric::closes(*k, in.asOf, 60);
    const QVector<double> r6 = Metric::rsi(c6, Criteria::kRsiPeriod);
    const QVector<double> r12 = Metric::rsi(c6, 12);
    const QVector<double> r24 = Metric::rsi(c6, 24);
    const auto last = [](const QVector<double>& v) {
        return v.isEmpty() ? qQNaN() : v.last();
    };
    const double v6 = last(r6), v12 = last(r12), v24 = last(r24);
    if (!std::isnan(v6)) {
        Evidence e;
        e.name = QStringLiteral("RSI（入场时过热度）");
        e.role = QStringLiteral("观测指标");
        const auto txt = [](double v) { return std::isnan(v) ? QStringLiteral("—") : f(v, 1); };
        e.value = QStringLiteral("RSI6 %1 / RSI12 %2 / RSI24 %3")
                      .arg(txt(v6), txt(v12), txt(v24));
        e.rule = QStringLiteral("RSI%1 ≥ %2 判极端超买；≥ %3 判超买")
                     .arg(Criteria::kRsiPeriod).arg(f(Criteria::kRsiExtreme, 1))
                     .arg(f(Criteria::kRsiHigh, 1));
        e.known = true; e.passed = v6 >= Criteria::kRsiExtreme; e.source = srcRsi;
        e.caveat = QStringLiteral("Wilder 口径，与图表副图一致；RSI 不是现行买入信号的触发字段");
        out << e;
    } else {
        out << unknown(QStringLiteral("RSI（入场时过热度）"), QStringLiteral("观测指标"),
                       QStringLiteral("RSI%1 ≥ %2 判极端超买")
                           .arg(Criteria::kRsiPeriod).arg(f(Criteria::kRsiExtreme, 1)),
                       srcRsi, QStringLiteral("日 K 样本不足，RSI 无法计算（不补默认值）"));
    }
}

void appendFlowEvidence(QVector<Evidence>& out, const Input& in, const QString& code)
{
    const auto it = in.mainNetYi.constFind(code);
    if (it != in.mainNetYi.constEnd()) {
        Evidence e;
        e.name = QStringLiteral("资金流入");
        e.role = QStringLiteral("观测指标");
        e.value = QStringLiteral("%1 亿元（主力净流入）").arg(f(*it, 2));
        e.rule = QStringLiteral("主力净流入 > %1 亿元").arg(f(Criteria::kFlowPositiveYi, 1));
        e.known = true; e.passed = *it > Criteria::kFlowPositiveYi;
        e.source = in.mainNetSource.value(code, QStringLiteral("个股详情 · 资金流日线"));
        e.caveat = QStringLiteral("个股主力净流入为事后可核对的客观数值，不是现行买入信号的触发字段");
        out << e;
        return;
    }
    const auto p = in.sealToAmount.constFind(code);
    if (p != in.sealToAmount.constEnd()) {
        Evidence e;
        e.name = QStringLiteral("资金流入（量能代理）");
        e.role = QStringLiteral("观测指标");
        e.value = QStringLiteral("封单额/成交额 = %1").arg(f(*p, 3));
        e.rule = QStringLiteral("封单额/成交额 ≥ %1（代理门槛）").arg(f(Criteria::kSealProxyMin));
        e.known = true; e.passed = *p >= Criteria::kSealProxyMin;
        e.source = QStringLiteral("涨停池 entry.sealOrder / entry.amount · %1").arg(dateText(in.date));
        e.caveat = QStringLiteral("免费选股接口无主力净流入字段，此处是明确标注的代理口径，不等同真实资金流");
        out << e;
        return;
    }
    out << unknown(QStringLiteral("资金流入"), QStringLiteral("观测指标"),
                   QStringLiteral("主力净流入 > %1 亿元").arg(f(Criteria::kFlowPositiveYi, 1)),
                   QStringLiteral("个股资金流日线 / 涨停池封单额"),
                   QStringLiteral("当日个股资金流与封单强度均未留档；历史日期不回补、不填 0"));
}

SignalTrace buildBuySignal(const TradePlan& p, const Input& in)
{
    SignalTrace s;
    s.date = in.date; s.code = p.code; s.name = p.name;
    s.side = p.actionable ? QStringLiteral("买入信号 · 可执行") : QStringLiteral("买入信号 · 观察/未放行");
    s.origin = QStringLiteral("当日计划快照 recommend_plan(%1)").arg(dateText(in.date));
    s.level = levelText(p.signalLevel);
    s.expectation = QStringLiteral("%1；仓位 %2；盈亏比 %3；成交概率 %4")
                        .arg(planPrices(p), pct(p.positionPct, 1), f(p.riskReward),
                             pct(p.fillProbability * 100.0, 0));

    auto& c = s.conditions;
    Evidence gate;
    gate.name = QStringLiteral("执行标记（闸门）"); gate.role = QStringLiteral("触发条件");
    gate.value = p.actionable ? QStringLiteral("可执行") : QStringLiteral("观察名单");
    gate.rule = QStringLiteral("actionable = true（闸门放行且未被竞价 / 风控降级）");
    gate.known = true; gate.passed = p.actionable;
    gate.source = QStringLiteral("TradePlan.actionable · %1").arg(s.origin);
    gate.caveat = p.note.isEmpty() ? QStringLiteral("计划快照字段；闸门原始输入见当日情绪留档") : p.note;
    c << gate;

    Evidence lv;
    lv.name = QStringLiteral("信号档位 / 仓位"); lv.role = QStringLiteral("触发条件");
    lv.value = QStringLiteral("%1 · 建议仓位 %2").arg(levelText(p.signalLevel), pct(p.positionPct, 1));
    lv.rule = QStringLiteral("signalLevel ≥ 2 且 positionPct > 0");
    lv.known = true; lv.passed = p.signalLevel >= 2 && p.positionPct > 0.0;
    lv.source = QStringLiteral("TradePlan.signalLevel / positionPct · 情绪八门条件生成");
    lv.caveat = QStringLiteral("0 无 / 1 观察 / 2 试仓 / 3 建仓 / 4 主攻；档位由当日情绪引擎给出");
    c << lv;

    if (in.sentimentKnown) {
        Evidence em;
        em.name = QStringLiteral("情绪硬闸门"); em.role = QStringLiteral("触发条件");
        em.value = QStringLiteral("复合情绪 %1 · tradeAllowed = %2 · 有效总仓上限 %3")
                       .arg(f(in.composite, 1), yn(in.tradeAllowed), pct(in.positionCapPct, 0));
        em.rule = QStringLiteral("tradeAllowed = true（硬否决条件未触发）且 positionPct ≤ 有效总仓上限");
        em.known = true;
        em.passed = in.tradeAllowed && (in.positionCapPct <= 0.0 || p.positionPct <= in.positionCapPct);
        em.source = QStringLiteral("SentimentState · %1 当日留档").arg(dateText(in.date));
        em.caveat = QStringLiteral("跌停家数 / 炸板率 / 晋级率等闸门原文见当日情绪留档，本条只核对结论与仓位上限");
        c << em;
    } else {
        c << unknown(QStringLiteral("情绪硬闸门"), QStringLiteral("触发条件"),
                     QStringLiteral("tradeAllowed = true（硬否决条件未触发）"),
                     QStringLiteral("SentimentState 当日留档"),
                     QStringLiteral("该历史交易日未留档情绪事实；不以今日情绪倒推当时判定"));
    }

    Evidence fr;
    fr.name = QStringLiteral("多因子过滤"); fr.role = QStringLiteral("触发条件");
    fr.value = p.factorReject.isEmpty()
                   ? QStringLiteral("未淘汰 · 综合分 %1（排名 %2）").arg(f(p.compositeScore, 1)).arg(p.factorRank)
                   : p.factorReject;
    fr.rule = QStringLiteral("factorReject 为空（未被组合规则淘汰）");
    fr.known = true; fr.passed = p.factorReject.isEmpty();
    fr.source = QStringLiteral("TradePlan.factorReject / compositeScore / factorRank");
    fr.caveat = QStringLiteral("只核对当时的评分结果，复盘不追加后验分数门槛");
    c << fr;

    Evidence rg;
    rg.name = QStringLiteral("计划区间自洽"); rg.role = QStringLiteral("触发条件");
    rg.value = planPrices(p);
    rg.rule = QStringLiteral("buyHigh < takeProfitLow 且 stopLossHigh < buyLow");
    rg.known = true;
    rg.passed = p.buyHigh > 0.0 && p.takeProfitLow > p.buyHigh && p.stopLossHigh < p.buyLow;
    rg.source = QStringLiteral("TradePlan 三段区间 · %1").arg(s.origin);
    rg.caveat = p.buyBasis.isEmpty() ? QStringLiteral("定价锚点见计划快照 buyBasis / tpBasis / slBasis")
                                     : p.buyBasis;
    c << rg;

    Evidence rr;
    rr.name = QStringLiteral("盈亏比 / 成交概率"); rr.role = QStringLiteral("触发条件");
    rr.value = QStringLiteral("盈亏比 %1 · 成交概率 %2 · 止损幅度 %3")
                   .arg(f(p.riskReward), pct(p.fillProbability * 100.0, 0), pct(p.riskPct));
    rr.rule = QStringLiteral("riskReward > 0 且 fillProbability > 0（仅记录，不设事后门槛）");
    rr.known = true; rr.passed = p.riskReward > 0.0 && p.fillProbability > 0.0;
    rr.source = QStringLiteral("TradePlan.riskReward / fillProbability / riskPct");
    rr.caveat = p.fillNote.isEmpty() ? QStringLiteral("保守口径：止盈下沿兑现") : p.fillNote;
    c << rr;

    appendKlineEvidences(c, in, p.code);
    appendFlowEvidence(c, in, p.code);

    if (p.auctionEvaluated) {
        Evidence au;
        au.name = QStringLiteral("次日竞价结论"); au.role = QStringLiteral("触发条件");
        au.value = QStringLiteral("判定 %1 · 高开 %2 · %3")
                       .arg(p.auctionVerdict).arg(pct(p.auctionOpenPct), p.auctionNote);
        au.rule = QStringLiteral("auctionVerdict = 1（可接力）");
        au.known = true; au.passed = p.auctionVerdict == 1;
        au.source = QStringLiteral("TradePlan.auction* · 9:25 真实竞价");
        au.caveat = p.auctionBasis;
        c << au;
    }

    for (const Evidence& e : c) if (e.known) ++s.knownCount;
    for (const Evidence& e : c) if (e.known && e.passed) ++s.passedCount;
    s.complete = s.knownCount == c.size();
    QStringList missing;
    for (const Evidence& e : c) if (!e.known) missing << e.name;
    s.verdict = s.complete
        ? QStringLiteral("条件数据齐全（%1/%2 条有数据，%3 条成立）")
              .arg(s.knownCount).arg(c.size()).arg(s.passedCount)
        : QStringLiteral("数据不足：%1 缺失（其余 %2/%3 条可核对）")
              .arg(missing.join(QStringLiteral("、"))).arg(s.knownCount).arg(c.size());
    return s;
}

SignalTrace buildExitSignal(const TradeRecord& t, const Input& in, const KlineSeries* k)
{
    SignalTrace s;
    s.date = in.date; s.code = t.code; s.name = t.name;
    s.side = QStringLiteral("离场条件（卖出侧）");
    s.origin = QStringLiteral("交易日志持仓 %1（买入 %2）+ 计划止盈/止损")
                   .arg(dateText(t.buyDate)).arg(f(t.buyPrice));
    s.level = QStringLiteral("持仓中");
    s.expectation = QStringLiteral("计划止损 %1 跌破离场 / 计划止盈 %2 分批兑现；买入价 %3 · %4 股")
                        .arg(f(t.planStop), f(t.planTake), f(t.buyPrice)).arg(t.qty);

    const Metric::FollowUp fu = k ? Metric::followUp(*k, in.date, Criteria::kOutcomeHorizon)
                                  : Metric::FollowUp();
    const QString src = klineSource(in.asOf, Criteria::kOutcomeHorizon, true);
    auto& c = s.conditions;

    if (t.planStop > 0.0 && fu.known) {
        Evidence e;
        e.name = QStringLiteral("止损触发"); e.role = QStringLiteral("触发条件");
        e.value = QStringLiteral("窗口最低 %1 / 计划止损 %2").arg(f(fu.minLow), f(t.planStop));
        e.rule = QStringLiteral("最低价 ≤ 计划止损 %1 → 无条件离场").arg(f(t.planStop));
        e.known = true; e.passed = fu.minLow <= t.planStop; e.source = src;
        e.caveat = QStringLiteral("按日 K 最低价判定；盘中是否实际执行以交易日志为准");
        c << e;
    } else {
        c << unknown(QStringLiteral("止损触发"), QStringLiteral("触发条件"),
                     QStringLiteral("最低价 ≤ 计划止损 → 无条件离场"), src,
                     t.planStop <= 0.0 ? QStringLiteral("该笔未记录计划止损价")
                                       : QStringLiteral("窗口内无日 K，无法核对"));
    }

    if (t.planTake > 0.0 && fu.known) {
        Evidence e;
        e.name = QStringLiteral("止盈触发"); e.role = QStringLiteral("触发条件");
        e.value = QStringLiteral("窗口最高 %1 / 计划止盈 %2").arg(f(fu.maxHigh), f(t.planTake));
        e.rule = QStringLiteral("最高价 ≥ 计划止盈 %1 → 分批兑现").arg(f(t.planTake));
        e.known = true; e.passed = fu.maxHigh >= t.planTake; e.source = src;
        e.caveat = QStringLiteral("按日 K 最高价判定，不假定一定成交在最高价");
        c << e;
    } else {
        c << unknown(QStringLiteral("止盈触发"), QStringLiteral("触发条件"),
                     QStringLiteral("最高价 ≥ 计划止盈 → 分批兑现"), src,
                     t.planTake <= 0.0 ? QStringLiteral("该笔未记录计划止盈价")
                                       : QStringLiteral("窗口内无日 K，无法核对"));
    }

    appendKlineEvidences(c, in, t.code);

    for (const Evidence& e : c) if (e.known) ++s.knownCount;
    for (const Evidence& e : c) if (e.known && e.passed) ++s.passedCount;
    s.complete = s.knownCount == c.size();
    QStringList missing;
    for (const Evidence& e : c) if (!e.known) missing << e.name;
    s.verdict = s.complete
        ? QStringLiteral("条件数据齐全（%1/%2 条有数据，%3 条成立）")
              .arg(s.knownCount).arg(c.size()).arg(s.passedCount)
        : QStringLiteral("数据不足：%1 缺失（其余 %2/%3 条可核对）")
              .arg(missing.join(QStringLiteral("、"))).arg(s.knownCount).arg(c.size());
    return s;
}

OutcomeDeviation buildOutcome(const TradePlan& p, const Input& in)
{
    OutcomeDeviation d;
    d.code = p.code; d.name = p.name; d.signalDate = in.date;
    d.targetPrice = p.takeProfitLow; d.stopPrice = p.stopLossHigh;

    const KlineSeries* k = klineFor(in, p.code);
    const Metric::FollowUp fu = k ? Metric::followUp(*k, in.date, Criteria::kOutcomeHorizon)
                                  : Metric::FollowUp();
    if (!fu.known) {
        d.dataNote = k ? QStringLiteral("信号日之后暂无日 K：收盘当日复盘时观察窗口未闭合，下一交易日起可回看")
                       : QStringLiteral("该标的日 K 未缓存，无法计算事后偏差（不以计划价代替实际走势）");
        d.summary = d.dataNote;
        Evidence e = unknown(QStringLiteral("事后观察窗口"), QStringLiteral("事后核对"),
                             QStringLiteral("信号日后 ≤ %1 根日 K").arg(Criteria::kOutcomeHorizon),
                             klineSource(in.asOf, Criteria::kOutcomeHorizon, true), d.dataNote);
        d.checks << e;
        return d;
    }

    d.known = true;
    d.bars = fu.bars; d.lastBarDate = fu.lastDate; d.basePrice = fu.base;
    d.maxHigh = fu.maxHigh; d.minLow = fu.minLow; d.lastClose = fu.lastClose;
    d.mfePct = (fu.maxHigh / fu.base - 1.0) * 100.0;
    d.maePct = (fu.minLow / fu.base - 1.0) * 100.0;
    d.closePct = (fu.lastClose / fu.base - 1.0) * 100.0;
    d.targetDeviationPct = d.targetPrice > 0.0
                               ? (fu.maxHigh - d.targetPrice) / d.targetPrice * 100.0 : 0.0;
    d.targetTouched = d.targetPrice > 0.0 && fu.maxHigh >= d.targetPrice;
    d.stopTouched = d.stopPrice > 0.0 && fu.minLow <= d.stopPrice;

    d.summary = QStringLiteral(
        "信号日收盘 %1 → 后续 %2 根（%3~%4）最高 %5（%6）/ 最低 %7（%8）/ 末收 %9（%10）；"
        "计划止盈 %11 %12（目标偏差 %13），计划止损 %14 %15。")
        .arg(f(d.basePrice)).arg(d.bars).arg(dateText(fu.firstDate), dateText(d.lastBarDate))
        .arg(f(d.maxHigh), pct(d.mfePct), f(d.minLow), pct(d.maePct),
             f(d.lastClose), pct(d.closePct), f(d.targetPrice))
        .arg(d.targetTouched ? QStringLiteral("已触及") : QStringLiteral("未触及"))
        .arg(pct(d.targetDeviationPct), f(d.stopPrice))
        .arg(d.stopTouched ? QStringLiteral("已触及") : QStringLiteral("未触及"));

    const QString src = klineSource(in.asOf, d.bars, true);
    auto add = [&](const QString& name, const QString& value, const QString& rule,
                   bool passed, const QString& caveat) {
        Evidence e;
        e.name = name; e.role = QStringLiteral("事后核对"); e.value = value; e.rule = rule;
        e.known = true; e.passed = passed; e.source = src; e.caveat = caveat;
        d.checks << e;
    };
    add(QStringLiteral("观察窗口"),
        QStringLiteral("%1 根（%2 ~ %3）").arg(d.bars).arg(dateText(fu.firstDate), dateText(d.lastBarDate)),
        QStringLiteral("信号日后最多 %1 根，不含信号日").arg(Criteria::kOutcomeHorizon),
        d.bars > 0, QStringLiteral("窗口未闭合时只展示已有交易日，不外推"));
    add(QStringLiteral("是否触及计划止盈"),
        QStringLiteral("最高 %1 / 止盈下沿 %2").arg(f(d.maxHigh), f(d.targetPrice)),
        QStringLiteral("最高 ≥ 计划止盈下沿"), d.targetTouched,
        QStringLiteral("触及只说明价格到过，不代表按该价成交"));
    add(QStringLiteral("是否触及计划止损"),
        QStringLiteral("最低 %1 / 止损上沿 %2").arg(f(d.minLow), f(d.stopPrice)),
        QStringLiteral("最低 ≤ 计划止损上沿"), d.stopTouched,
        QStringLiteral("按日 K 最低价核对，不等同实际卖出价"));
    add(QStringLiteral("与策略预期的偏差"),
        QStringLiteral("目标偏差 %1 · MFE %2 · MAE %3 · 窗口末 %4")
            .arg(pct(d.targetDeviationPct), pct(d.mfePct), pct(d.maePct), pct(d.closePct)),
        QStringLiteral("目标偏差 = (窗口最高 − 止盈下沿)/止盈下沿×100%；≥0 表示达到预期目标"),
        d.targetDeviationPct >= 0.0,
        QStringLiteral("偏差只对计划里已写明的价格计算，不引入新的目标价"));
    return d;
}

bool executed(const Input& in, const QString& code)
{
    const QDate until = in.date.addDays(1);
    for (const TradeRecord& t : in.trades) {
        if (t.code == code && t.buyDate.isValid()
            && t.buyDate >= in.date && t.buyDate <= until) return true;
    }
    return false;
}

SkippedOpportunity buildSkipped(const TradePlan& p, const Input& in)
{
    SkippedOpportunity s;
    s.code = p.code; s.name = p.name; s.signalDate = in.date;
    s.buyMid = p.buyMid(); s.targetPrice = p.takeProfitLow; s.stopPrice = p.stopLossHigh;
    s.skipRule = QStringLiteral("交易日志中不存在 code=%1 且 buyDate ∈ [%2, %3] 的买入记录")
                     .arg(p.code, dateText(in.date), dateText(in.date.addDays(1)));
    s.caveat = QStringLiteral(
        "事后统计：只回答「当时没执行的那只后来走了多远」，不评价当时跳过是否合理，"
        "也不构成任何下一步建议；观察窗口闭合后才可核对。");
    s.basis = QStringLiteral(
        "按计划买入中枢 %1 成交、窗口末收盘卖出，扣买入佣金 %2 与卖出费用 %3；"
        "不含滑点、成交概率、T+1 限制与资金占用，仅作事后对照。")
        .arg(f(s.buyMid, 2), pct(TradeRecord::buyFeeRate() * 100.0, 3),
             pct(TradeRecord::sellFeeRate() * 100.0, 3));

    const KlineSeries* k = klineFor(in, p.code);
    const Metric::FollowUp fu = k ? Metric::followUp(*k, in.date, Criteria::kOutcomeHorizon)
                                  : Metric::FollowUp();
    if (!fu.known) {
        s.dataNote = k ? QStringLiteral("信号日后暂无日 K，窗口未闭合：此处不给「错过的幅度」")
                       : QStringLiteral("该标的日 K 未缓存：不推测后续表现");
        return s;
    }
    s.known = true;
    s.bars = fu.bars; s.lastBarDate = fu.lastDate;
    s.maxHigh = fu.maxHigh; s.minLow = fu.minLow; s.lastClose = fu.lastClose;
    s.mfePct = (fu.maxHigh / s.buyMid - 1.0) * 100.0;
    s.maePct = (fu.minLow / s.buyMid - 1.0) * 100.0;
    s.closePct = (fu.lastClose / s.buyMid - 1.0) * 100.0;
    s.targetTouched = s.targetPrice > 0.0 && fu.maxHigh >= s.targetPrice;
    s.stopTouched = s.stopPrice > 0.0 && fu.minLow <= s.stopPrice;
    if (s.buyMid > 0.0) {
        s.hypotheticalPct = (fu.lastClose * (1.0 - TradeRecord::sellFeeRate())
                             / (s.buyMid * (1.0 + TradeRecord::buyFeeRate())) - 1.0) * 100.0;
    }
    return s;
}

HoldingRisk buildRsiRisk(const TradeRecord& t, const Input& in, const KlineSeries* k)
{
    HoldingRisk r;
    r.code = t.code; r.name = t.name; r.type = QStringLiteral("RSI 极端超买");
    const QString src = klineSource(in.asOf, 25, false);
    if (!k || !k->valid()) {
        r.level = QStringLiteral("数据不足");
        r.conclusion = QStringLiteral("该标的日 K 未缓存，无法计算 RSI（不填默认值）");
        r.conditions << unknown(QStringLiteral("RSI6"), QStringLiteral("风险标记"),
                                QStringLiteral("RSI6 ≥ %1").arg(f(Criteria::kRsiExtreme, 1)),
                                src, r.conclusion);
        return r;
    }
    const QVector<double> cl = Metric::closes(*k, in.asOf, 60);
    const QVector<double> rr = Metric::rsi(cl, Criteria::kRsiPeriod);
    const double v6 = rr.isEmpty() ? qQNaN() : rr.last();
    if (std::isnan(v6)) {
        r.level = QStringLiteral("数据不足");
        r.conclusion = QStringLiteral("日 K 样本不足 %1 根，RSI 无法计算")
                           .arg(Criteria::kRsiPeriod + 1);
        r.conditions << unknown(QStringLiteral("RSI6"), QStringLiteral("风险标记"),
                                QStringLiteral("RSI6 ≥ %1").arg(f(Criteria::kRsiExtreme, 1)),
                                src, r.conclusion);
        return r;
    }
    Evidence e;
    e.name = QStringLiteral("RSI6（Wilder）"); e.role = QStringLiteral("风险标记");
    e.value = f(v6, 1);
    e.rule = QStringLiteral("RSI6 ≥ %1 判极端超买；≥ %2 判超买")
                 .arg(f(Criteria::kRsiExtreme, 1)).arg(f(Criteria::kRsiHigh, 1));
    e.known = true; e.passed = v6 >= Criteria::kRsiExtreme; e.source = src;
    e.caveat = QStringLiteral("与图表副图 RSI(6/12/24) 同口径；数值只描述过热程度，不预测回落");
    r.conditions << e;
    r.known = true; r.triggered = e.passed;
    r.level = r.triggered ? QStringLiteral("触发") : QStringLiteral("未触发");
    r.conclusion = QStringLiteral("RSI6 = %1，%2极端超买线 %3")
                       .arg(f(v6, 1), r.triggered ? QStringLiteral("已达到") : QStringLiteral("未达到"),
                            f(Criteria::kRsiExtreme, 1));
    return r;
}

HoldingRisk buildFakeStabilizeRisk(const TradeRecord& t, const Input& in, const KlineSeries* k)
{
    HoldingRisk r;
    r.code = t.code; r.name = t.name; r.type = QStringLiteral("低点下移 · 假企稳");
    const QString srcMa = klineSource(in.asOf, 20, false);
    const QString srcLow = klineSource(in.asOf, Criteria::kSwingWindow * Criteria::kSwingSamples, false);
    const Metric::MaInfo m = k ? Metric::ma(*k, in.asOf) : Metric::MaInfo();
    const Metric::LowInfo lo = k ? Metric::swingLows(*k, in.asOf) : Metric::LowInfo();

    if (!m.known || !lo.known) {
        r.level = QStringLiteral("数据不足");
        r.conclusion = QStringLiteral("均线需 ≥20 根、低点序列需 ≥%1 根日 K，当前不足，不做假企稳判定")
                           .arg(Criteria::kSwingWindow * Criteria::kSwingSamples);
        r.conditions << unknown(QStringLiteral("均线收敛度"), QStringLiteral("风险标记"),
                                QStringLiteral("≤ %1%").arg(f(Criteria::kMaConvergePct)),
                                srcMa, r.conclusion);
        r.conditions << unknown(QStringLiteral("窗口低点序列"), QStringLiteral("风险标记"),
                                QStringLiteral("最近 %1 个 %2 日窗口低点严格递减")
                                    .arg(Criteria::kSwingSamples).arg(Criteria::kSwingWindow),
                                srcLow, r.conclusion);
        return r;
    }
    Evidence em;
    em.name = QStringLiteral("均线收敛度"); em.role = QStringLiteral("风险标记");
    em.value = QStringLiteral("MA5 %1 / MA10 %2 / MA20 %3 · %4")
                   .arg(f(m.ma5), f(m.ma10), f(m.ma20), pct(m.spreadPct));
    em.rule = QStringLiteral("(max−min)/MA20×100 ≤ %1%（均线看似收敛）")
                  .arg(f(Criteria::kMaConvergePct));
    em.known = true; em.passed = m.converged; em.source = srcMa;
    em.caveat = QStringLiteral("收敛只描述均线间距，不代表趋势已转向");
    r.conditions << em;

    QStringList lowTxt;
    for (double v : lo.lows) lowTxt << f(v);
    Evidence el;
    el.name = QStringLiteral("窗口低点序列"); el.role = QStringLiteral("风险标记");
    el.value = QStringLiteral("%1（由旧到新，%2 日/窗）· 斜率 %3")
                   .arg(lowTxt.join(QStringLiteral(" → ")))
                   .arg(lo.window).arg(pct(lo.slopePct));
    el.rule = QStringLiteral("最近 %1 个窗口低点严格递减 且 斜率 < 0%")
                  .arg(Criteria::kSwingSamples);
    el.known = true; el.passed = lo.declining && lo.slopePct < 0.0; el.source = srcLow;
    el.caveat = QStringLiteral("低点取每个 %1 根窗口的最低价（前复权）").arg(lo.window);
    r.conditions << el;

    r.known = true;
    r.triggered = em.passed && el.passed;
    r.level = r.triggered ? QStringLiteral("触发") : QStringLiteral("未触发");
    r.conclusion = r.triggered
        ? QStringLiteral("均线收敛度 %1（≤%2%）看似企稳，但最近 %3 个窗口低点 %4（斜率 %5）仍在下移 → 假企稳形态")
              .arg(pct(m.spreadPct), f(Criteria::kMaConvergePct))
              .arg(Criteria::kSwingSamples).arg(lowTxt.join(QStringLiteral(" → ")), pct(lo.slopePct))
        : QStringLiteral("未同时满足「收敛 %1（阈值 ≤%2%）」与「低点递减（%3）」，不标记假企稳")
              .arg(pct(m.spreadPct), f(Criteria::kMaConvergePct), lowTxt.join(QStringLiteral(" → ")));
    return r;
}

HoldingRisk buildChipRisk(const TradeRecord& t, const Input& in, const KlineSeries* k, double price)
{
    HoldingRisk r;
    r.code = t.code; r.name = t.name; r.type = QStringLiteral("上方多层阻力");
    const QString src = klineSource(in.asOf, 60, true);
    const Metric::ChipInfo ci = k ? Metric::chipLayers(*k, in.asOf, price) : Metric::ChipInfo();
    if (!ci.known) {
        r.level = QStringLiteral("数据不足");
        r.conclusion = QStringLiteral("不复权日 K 不足 20 根，无法重建筹码分布（不以均线代替压力位）");
        r.conditions << unknown(QStringLiteral("上方压力峰"), QStringLiteral("风险标记"),
                                QStringLiteral("占比 ≥ %1 的峰 ≥ %2 层")
                                    .arg(pct(Criteria::kChipMinShare * 100.0, 0))
                                    .arg(Criteria::kChipLayers),
                                src, r.conclusion);
        return r;
    }
    Evidence e1;
    e1.name = QStringLiteral("有效压力峰"); e1.role = QStringLiteral("风险标记");
    e1.value = ci.detail.isEmpty() ? QStringLiteral("无") : ci.detail.join(QStringLiteral(" ｜ "));
    e1.rule = QStringLiteral("峰顶价 > 现价 且 筹码占比 ≥ %1")
                  .arg(pct(Criteria::kChipMinShare * 100.0, 0));
    e1.known = true; e1.passed = !ci.detail.isEmpty(); e1.source = src;
    e1.caveat = QStringLiteral("由不复权日 K 按成交量三角分布重建，逐日衰减 0.985，与计划定价同口径");
    r.conditions << e1;

    Evidence e2;
    e2.name = QStringLiteral("压力层数"); e2.role = QStringLiteral("风险标记");
    e2.value = QStringLiteral("%1 层（现价 %2）").arg(ci.layers).arg(f(price));
    e2.rule = QStringLiteral("层数 ≥ %1 → 上方存在多层阻力").arg(Criteria::kChipLayers);
    e2.known = true; e2.passed = ci.layers >= Criteria::kChipLayers; e2.source = src;
    e2.caveat = QStringLiteral("相邻峰顶价差 < 0.5% 视为同一层，避免把宽峰重复计数");
    r.conditions << e2;

    r.known = true; r.triggered = e2.passed;
    r.level = r.triggered ? QStringLiteral("触发") : QStringLiteral("未触发");
    r.conclusion = r.triggered
        ? QStringLiteral("现价 %1 上方有 %2 层有效压力峰：%3")
              .arg(f(price)).arg(ci.layers).arg(ci.detail.join(QStringLiteral(" ｜ ")))
        : QStringLiteral("现价 %1 上方有效压力峰 %2 层（阈值 %3 层），不标记多层阻力")
              .arg(f(price)).arg(ci.layers).arg(Criteria::kChipLayers);
    return r;
}

SectorRotation buildSectors(const Input& in)
{
    SectorRotation s;
    s.date = in.date;
    s.fetchedAt = in.sectorFetchedAt;
    s.source = in.sectorSource.isEmpty()
                   ? QStringLiteral("东方财富板块资金流 clist：f3 涨跌幅 / f62 主力净流入净额（元→亿元）/ f184 主力净占比")
                   : in.sectorSource;
    s.criteria = QStringLiteral(
        "领涨 = 当日板块涨幅降序前 %1；领跌 = 后 %1；资金流只取接口原值并标明单位，"
        "不推断题材持续性、不预测次日轮动。")
        .arg(Criteria::kSectorTopN);
    s.rows = in.sectorRows;
    std::sort(s.rows.begin(), s.rows.end(), [](const SectorFlowRow& a, const SectorFlowRow& b) {
        return a.changePct > b.changePct;
    });
    const int n = s.rows.size();
    const int top = qMin(Criteria::kSectorTopN, n);
    const int bottomFrom = qMax(top, n - Criteria::kSectorTopN);
    for (int i = 0; i < n; ++i) {
        s.rows[i].tag = i < top ? QStringLiteral("领涨")
                              : (i >= bottomFrom ? QStringLiteral("领跌") : QStringLiteral("中间"));
    }
    s.available = n > 0 && in.sectorReason.isEmpty();
    s.reason = s.available ? QString()
                           : (in.sectorReason.isEmpty()
                                  ? QStringLiteral("当日板块资金流未取到：不以题材涨幅代替资金流数据")
                                  : in.sectorReason);
    return s;
}

} // namespace

Report build(const Input& in)
{
    Report r;
    r.date = in.date;
    r.generatedAt = QDateTime::currentDateTime();
    r.valid = in.date.isValid();
    r.purpose = purposeText();
    r.criteria = criteriaLines();

    QStringList missingPlans, missingK, missingFlow;
    for (const TradePlan& p : in.plans) {
        r.signalTraces << buildBuySignal(p, in);
        r.outcomes << buildOutcome(p, in);
        if (p.actionable && !executed(in, p.code)) r.skipped << buildSkipped(p, in);
        const KlineSeries* k = klineFor(in, p.code);
        if (!k || !k->valid()) missingK << p.code;
        if (!in.mainNetYi.contains(p.code) && !in.sealToAmount.contains(p.code))
            missingFlow << p.code;
    }

    for (const TradeRecord& t : in.trades) {
        if (t.closed()) continue;
        const KlineSeries* k = klineFor(in, t.code);
        r.signalTraces << buildExitSignal(t, in, k);
        double price = 0.0;
        if (k) {
            const QVector<Candle>& raw = k->hasRaw() ? k->rawCandles : k->candles;
            if (!raw.isEmpty()) price = raw.last().close;
        }
        r.risks << buildRsiRisk(t, in, k);
        r.risks << buildFakeStabilizeRisk(t, in, k);
        r.risks << buildChipRisk(t, in, k, price);
        if (!k || !k->valid()) missingK << t.code;
    }

    r.sectors = buildSectors(in);
    missingK.removeDuplicates();
    missingFlow.removeDuplicates();
    if (in.plans.isEmpty()) missingPlans << dateText(in.date);
    if (!in.sentimentKnown)
        r.gaps << QStringLiteral("情绪硬闸门：%1 未留档当日情绪事实，相关条件标「数据缺失」")
                      .arg(dateText(in.date));
    if (!in.plans.isEmpty())
        r.gaps << QStringLiteral("计划快照：%1 共 %2 条（可执行 %3 条）")
                      .arg(dateText(in.date)).arg(in.plans.size())
                      .arg(std::count_if(in.plans.begin(), in.plans.end(),
                                         [](const TradePlan& p) { return p.actionable; }));
    if (!missingPlans.isEmpty())
        r.gaps << QStringLiteral("计划快照：%1 无当日 recommend_plan 记录，信号溯源为空（不补造信号）")
                      .arg(missingPlans.join(QStringLiteral("、")));
    if (!missingK.isEmpty())
        r.gaps << QStringLiteral("日 K 未缓存：%1 —— 均线/RSI/筹码/事后走势一律标「数据缺失」")
                      .arg(missingK.join(QStringLiteral("、")));
    if (!missingFlow.isEmpty())
        r.gaps << QStringLiteral("个股资金流未留档：%1 —— 不以 0 或估算值填充")
                      .arg(missingFlow.join(QStringLiteral("、")));
    if (!r.sectors.available) r.gaps << QStringLiteral("板块轮动：%1").arg(r.sectors.reason);
    bool windowOpen = false;
    for (const OutcomeDeviation& d : r.outcomes) if (!d.known) windowOpen = true;
    if (windowOpen)
        r.gaps << QStringLiteral("事后窗口：部分标的信号日后暂无日 K（收盘当日窗口未闭合），下一交易日起可回看");
    return r;
}

} // namespace DecisionReview
