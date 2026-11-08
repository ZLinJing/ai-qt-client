#include "strategy/MultiFactorEngine.h"

#include "core/Indicators.h"
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace {

struct DimRaw {
    double  norm = 0.0;       // 维度得分 0~1（仅由可用子项加权）
    double  coverage = 0.0;   // 可用子项权重占比 0~1
    QString detail;           // 原始值 / 口径 / 缺失说明
};

struct Accum {
    double  sum = 0.0;
    double  wsum = 0.0;
    QString detail;

    void put(double w, double value, const QString& label)
    {
        if (w <= 0.0) return;
        sum += w * qBound(0.0, value, 1.0);
        wsum += w;
        if (!label.isEmpty()) detail += label + QStringLiteral("；");
    }

    DimRaw result(double totalWeight) const
    {
        DimRaw r;
        r.norm = wsum > 0.0 ? sum / wsum : 0.0;
        r.coverage = totalWeight > 0.0 ? qBound(0.0, wsum / totalWeight, 1.0) : 0.0;
        r.detail = detail;
        return r;
    }
};

// 梯形打分：[lo, hi] 内满分，向 softLo / softHi 线性衰减到 0
double bandScore(double v, double lo, double hi, double softLo, double softHi)
{
    if (v < lo) return qBound(0.0, (v - softLo) / qMax(1e-9, lo - softLo), 1.0);
    if (v > hi) return qBound(0.0, (softHi - v) / qMax(1e-9, softHi - hi), 1.0);
    return 1.0;
}

double clip01(double v) { return qBound(0.0, v, 1.0); }

double amtOf(const Candle& c)
{
    return c.amount > 0.0 ? c.amount : c.volume * 100.0 * c.close;   // 成交额缺失时用 量(手)×100×收盘 近似
}

QVector<double> closesOf(const QVector<Candle>& cs)
{
    QVector<double> out;
    out.reserve(cs.size());
    for (const Candle& c : cs) out.append(c.close);
    return out;
}

// ---------------- 维度 1：打板核心（保留原 Scorer 8 因子） ----------------
DimRaw calcDaban(const FactorInput& in)
{
    DimRaw r;
    r.norm = clip01(in.dabanScore / 100.0);
    r.coverage = 1.0;
    r.detail = QStringLiteral("原打板评分 %1 分（换手/量能/封板时点/封单/题材/高度/涨停基因/均线）")
                   .arg(in.dabanScore, 0, 'f', 1);
    return r;
}

// ---------------- 维度 2：趋势 ----------------
DimRaw calcTrend(const FactorInput& in)
{
    const QVector<Candle>& cs = in.kline.candles;
    Accum a;
    a.put(0.40, in.candidate.maBull, QStringLiteral("均线多头 %1").arg(in.candidate.maBull, 0, 'f', 2));
    if (cs.size() >= 20) {
        const QVector<double> s = Ind::sma(closesOf(cs), 20);
        const double ma = s.last();
        if (ma > 0.0)
            a.put(0.30, (cs.last().close / ma - 1.0) / 0.10,
                  QStringLiteral("收盘/MA20 偏离 %1%").arg((cs.last().close / ma - 1.0) * 100.0, 0, 'f', 1));
    }
    if (cs.size() >= 60) {
        const QVector<double> s = Ind::sma(closesOf(cs), 60);
        const double ma = s.last();
        if (ma > 0.0)
            a.put(0.30, (cs.last().close / ma - 1.0) / 0.20,
                  QStringLiteral("收盘/MA60 偏离 %1%").arg((cs.last().close / ma - 1.0) * 100.0, 0, 'f', 1));
    }
    DimRaw r = a.result(1.0);
    if (cs.size() < 20) r.detail += QStringLiteral("K线不足 20 根，长周期子项未参与；");
    return r;
}

// ---------------- 维度 3：动量 ----------------
DimRaw calcMomentum(const FactorInput& in)
{
    const QVector<Candle>& cs = in.kline.candles;
    Accum a;
    if (cs.size() >= 21) {
        const double ret = cs.last().close / cs.at(cs.size() - 21).close - 1.0;
        a.put(0.35, ret / 0.20, QStringLiteral("近20日涨幅 %1%").arg(ret * 100.0, 0, 'f', 1));
    }
    if (cs.size() >= 6) {
        const double ret = cs.last().close / cs.at(cs.size() - 6).close - 1.0;
        a.put(0.25, ret / 0.08, QStringLiteral("近5日涨幅 %1%").arg(ret * 100.0, 0, 'f', 1));
    }
    a.put(0.20, Ind::volRatioScore(in.candidate.volRatio5, 1.5, 3.0),
          QStringLiteral("量比 %1").arg(in.candidate.volRatio5, 0, 'f', 2));
    a.put(0.20, in.candidate.limitCount20 / 5.0,
          QStringLiteral("20日涨停 %1 次").arg(in.candidate.limitCount20, 0, 'f', 0));
    return a.result(1.0);
}

// ---------------- 维度 4：资金流向（代理口径） ----------------
DimRaw calcCapitalFlow(const FactorInput& in)
{
    const LimitEntry& e = in.candidate.entry;
    const QVector<Candle>& cs = in.kline.candles;
    Accum a;
    const double seal = 0.60 * clip01(e.sealToAmountRatio() / 0.20)
                      + 0.40 * clip01(e.sealToFloatCapRatio() / 0.01);
    a.put(0.35, seal, QStringLiteral("封单强度 封单/成交 %1% · 封单/流通 %2%")
                          .arg(e.sealToAmountRatio() * 100.0, 0, 'f', 1)
                          .arg(e.sealToFloatCapRatio() * 100.0, 0, 'f', 2));
    if (cs.size() >= 20) {
        double sum = 0.0;
        for (int i = cs.size() - 20; i < cs.size(); ++i) sum += amtOf(cs.at(i));
        const double avg = sum / 20.0;
        const double cur = amtOf(cs.last());
        if (avg > 0.0)
            a.put(0.30, (cur / avg) / 2.0,
                  QStringLiteral("成交额/20日均额 %1 倍").arg(cur / avg, 0, 'f', 2));
    }
    const Ind::TurnoverBand band = Ind::turnoverBandFor(e.floatCap / 1e8, e.board);
    if (band.dangerTurnover > 0.0)
        a.put(0.20, Ind::turnoverScore(e.turnover, band),
              QStringLiteral("换手 %1%").arg(e.turnover, 0, 'f', 1));
    if (!cs.isEmpty() && cs.last().high > cs.last().low) {
        const Candle& c = cs.last();
        a.put(0.15, (c.close - c.low) / (c.high - c.low),
              QStringLiteral("收盘承接位置 %1%")
                  .arg((c.close - c.low) / (c.high - c.low) * 100.0, 0, 'f', 0));
    }
    DimRaw r = a.result(1.0);
    r.detail += QStringLiteral("资金流向为代理口径（免费行情接口无主力净流入字段）；");
    return r;
}

// ---------------- 维度 5：基本面 ----------------
DimRaw calcFundamental(const FactorInput& in)
{
    const LimitEntry& e = in.candidate.entry;
    const Snapshot& s = in.snapshot;
    Accum a;
    const double capYi = e.floatCap / 1e8;
    a.put(0.25, bandScore(capYi, 20.0, 300.0, 5.0, 1000.0),
          QStringLiteral("流通市值 %1 亿").arg(capYi, 0, 'f', 1));
    a.put(0.15, clip01(e.turnover / 12.0),
          QStringLiteral("换手活跃度 %1%").arg(e.turnover, 0, 'f', 1));
    if (s.isValid()) {
        if (s.pe > 0.0)
            a.put(0.25, s.pe <= 25.0 ? 1.0 : clip01(1.0 - (s.pe - 25.0) / 50.0),
                  QStringLiteral("PE %1").arg(s.pe, 0, 'f', 1));
        else
            a.put(0.25, 0.0, QStringLiteral("PE<=0 业绩亏损"));
        if (s.pb > 0.0)
            a.put(0.20, s.pb <= 3.0 ? 1.0 : clip01(1.0 - (s.pb - 3.0) / 6.0),
                  QStringLiteral("PB %1").arg(s.pb, 0, 'f', 2));
        a.put(0.15, s.isST() ? 0.0 : 1.0, s.isST() ? QStringLiteral("ST/退市风险") : QStringLiteral("非 ST"));
    } else {
        a.detail += QStringLiteral("历史快照缺失：PE/PB/ST 子项未参与，权重自动重分配；");
    }
    return a.result(1.0);
}

// ---------------- 维度 6：波动率（风险调节） ----------------
DimRaw calcVolatility(const FactorInput& in)
{
    const QVector<Candle>& cs = in.kline.candles;
    Accum a;
    if (cs.size() >= 15) {
        double trSum = 0.0;
        for (int i = cs.size() - 14; i < cs.size(); ++i) {
            const Candle& c = cs.at(i);
            const double pc = cs.at(i - 1).close;
            trSum += qMax(c.high - c.low,
                          qMax(std::fabs(c.high - pc), std::fabs(c.low - pc)));
        }
        const double atrPct = trSum / 14.0 / qMax(1e-9, cs.last().close);
        a.put(0.50, bandScore(atrPct, 0.02, 0.05, 0.005, 0.12),
              QStringLiteral("ATR14 %1%").arg(atrPct * 100.0, 0, 'f', 2));
    }
    if (cs.size() >= 21) {
        QVector<double> rets;
        for (int i = cs.size() - 20; i < cs.size(); ++i)
            rets.append(cs.at(i).close / qMax(1e-9, cs.at(i - 1).close) - 1.0);
        double mean = 0.0;
        for (double r : rets) mean += r;
        mean /= rets.size();
        double var = 0.0;
        for (double r : rets) var += (r - mean) * (r - mean);
        const double stdPct = std::sqrt(var / rets.size());
        a.put(0.30, bandScore(stdPct, 0.015, 0.04, 0.003, 0.08),
              QStringLiteral("20日收益标准差 %1%").arg(stdPct * 100.0, 0, 'f', 2));

        double peak = 0.0, dd = 0.0;
        for (int i = cs.size() - 20; i < cs.size(); ++i) {
            peak = qMax(peak, cs.at(i).close);
            dd = qMax(dd, (peak - cs.at(i).close) / qMax(1e-9, peak));
        }
        a.put(0.20, clip01(1.0 - dd / 0.25),
              QStringLiteral("20日最大回撤 %1%").arg(dd * 100.0, 0, 'f', 1));
    }
    return a.result(1.0);
}

} // namespace

FactorEvaluation MultiFactorEngine::evaluate(const FactorInput& in, const FactorConfig& cfg)
{
    FactorEvaluation ev;
    ev.code = in.candidate.entry.code;
    ev.name = in.candidate.entry.name;
    ev.dabanScore = in.dabanScore;

    DimRaw raws[kFactorDimensionCount];
    raws[int(FactorDimension::Daban)]       = calcDaban(in);
    raws[int(FactorDimension::Trend)]       = calcTrend(in);
    raws[int(FactorDimension::Momentum)]    = calcMomentum(in);
    raws[int(FactorDimension::CapitalFlow)] = calcCapitalFlow(in);
    raws[int(FactorDimension::Fundamental)] = calcFundamental(in);
    raws[int(FactorDimension::Volatility)]  = calcVolatility(in);

    QVector<FactorContribution> contribs;
    double wsum = 0.0, weighted = 0.0, minNorm = 1.0;

    for (int i = 0; i < kFactorDimensionCount; ++i) {
        const DimensionConfig dc = cfg.dimAt(i);
        if (!dc.enabled || dc.weight <= 0.0) continue;
        const DimRaw& r = raws[i];

        FactorContribution fc;
        fc.dimension = i;
        fc.name = factorDimensionName(i);
        fc.normalized = r.norm;
        fc.coverage = r.coverage;
        fc.detail = r.detail;

        if (r.coverage <= 0.0) {
            fc.available = false;
            fc.detail += QStringLiteral("数据不足，该维度未参与评分；");
            contribs.append(fc);
            if (cfg.requireAllAvailable && ev.rejectReason.isEmpty())
                ev.rejectReason = QStringLiteral("%1 数据不足").arg(fc.name);
            continue;
        }
        fc.available = true;

        // 过滤规则：低于门槛即淘汰，记录首个原因
        if ((dc.mode == FactorMode::Filter || dc.mode == FactorMode::Both)
            && r.norm < dc.threshold) {
            fc.detail += QStringLiteral("未过门槛 %1；").arg(dc.threshold, 0, 'f', 2);
            contribs.append(fc);
            if (ev.rejectReason.isEmpty())
                ev.rejectReason = QStringLiteral("%1 %2 < 门槛 %3")
                                      .arg(fc.name).arg(r.norm, 0, 'f', 2)
                                      .arg(dc.threshold, 0, 'f', 2);
            continue;
        }
        contribs.append(fc);

        if (dc.mode == FactorMode::Filter) continue;   // 只过滤、不计分
        wsum += dc.weight;
        weighted += dc.weight * r.norm;
        minNorm = qMin(minNorm, r.norm);
    }

    if (!ev.rejectReason.isEmpty()) {
        ev.passed = false;
        ev.contributions = contribs;
        ev.summary = QStringLiteral("多因子过滤：%1").arg(ev.rejectReason);
        return ev;
    }
    if (wsum <= 0.0) {
        ev.passed = false;
        ev.rejectReason = QStringLiteral("无可用计分维度");
        ev.contributions = contribs;
        ev.summary = ev.rejectReason;
        return ev;
    }

    const bool penaltyMode = (cfg.rule == CombineRule::MinPenalty);
    const double composite = penaltyMode
        ? (0.75 * weighted / wsum + 0.25 * minNorm) * 100.0
        : weighted / wsum * 100.0;

    double leadContribution = -1.0;
    QString leadName;
    for (FactorContribution& fc : contribs) {
        if (!fc.available || fc.dimension >= kFactorDimensionCount) continue;
        const DimensionConfig dc = cfg.dimAt(fc.dimension);
        if (!dc.enabled || dc.weight <= 0.0 || dc.mode == FactorMode::Filter) continue;
        fc.weight = dc.weight / wsum;
        fc.contribution = (penaltyMode ? 0.75 : 1.0) * fc.weight * fc.normalized * 100.0;
        fc.share = composite > 0.0 ? fc.contribution / composite * 100.0 : 0.0;
        if (fc.contribution > leadContribution) {
            leadContribution = fc.contribution;
            leadName = fc.name;
        }
    }
    if (penaltyMode) {
        FactorContribution p;
        p.dimension = kFactorDimensionCount;   // 虚拟项：短板补偿
        p.name = QStringLiteral("短板补偿");
        p.normalized = minNorm;
        p.weight = 0.25;
        p.contribution = 0.25 * minNorm * 100.0;
        p.share = composite > 0.0 ? p.contribution / composite * 100.0 : 0.0;
        p.available = true;
        p.detail = QStringLiteral("木桶规则：最低维度得分 %1 的 25% 加权").arg(minNorm, 0, 'f', 2);
        contribs.append(p);
    }

    ev.composite = qBound(0.0, composite, 100.0);
    ev.contributions = contribs;
    if (composite < cfg.minComposite) {
        ev.passed = false;
        ev.rejectReason = QStringLiteral("综合分 %1 < 门槛 %2")
                              .arg(composite, 0, 'f', 1).arg(cfg.minComposite, 0, 'f', 1);
    }
    ev.summary = QStringLiteral("综合 %1 分 ｜ 主导 %2（%3 分）｜ 打板 %4 分")
                     .arg(ev.composite, 0, 'f', 1).arg(leadName.isEmpty() ? QStringLiteral("-") : leadName)
                     .arg(qMax(0.0, leadContribution), 0, 'f', 1)
                     .arg(in.dabanScore, 0, 'f', 1);
    return ev;
}

FactorReport MultiFactorEngine::rank(const QVector<FactorInput>& inputs, const FactorConfig& cfg)
{
    FactorReport rep;
    rep.configDigest = cfg.digest();
    rep.avgContribution.fill(0.0, kFactorDimensionCount);
    rep.avgShare.fill(0.0, kFactorDimensionCount);
    rep.availableCount.fill(0, kFactorDimensionCount);

    QVector<FactorEvaluation> passed;
    for (const FactorInput& in : inputs) {
        const FactorEvaluation ev = evaluate(in, cfg);
        if (ev.passed) passed.append(ev);
        else rep.rejected.append(ev);
    }
    std::sort(passed.begin(), passed.end(),
              [](const FactorEvaluation& a, const FactorEvaluation& b) {
                  if (qFuzzyCompare(a.composite, b.composite)) return a.code < b.code;
                  return a.composite > b.composite;
              });
    for (int i = 0; i < passed.size(); ++i) passed[i].rank = i + 1;
    if (passed.size() > cfg.topN) passed.resize(cfg.topN);
    rep.ranking = passed;

    const int n = qMax(1, rep.ranking.size());
    for (const FactorEvaluation& ev : rep.ranking) {
        for (const FactorContribution& fc : ev.contributions) {
            if (!fc.available || fc.dimension >= kFactorDimensionCount) continue;
            rep.avgContribution[fc.dimension] += fc.contribution;
            rep.avgShare[fc.dimension] += fc.share;
            ++rep.availableCount[fc.dimension];
        }
    }
    for (int i = 0; i < kFactorDimensionCount; ++i) {
        rep.avgContribution[i] /= n;
        rep.avgShare[i] /= n;
    }
    rep.notes = QStringLiteral("贡献度 = 归一化权重 × 维度得分 × 100，各维度贡献之和等于综合分；"
                               "平均贡献按入榜标的数 %1 只取均值。数据缺失子项自动剔除并按可用子项重新归一。")
                    .arg(rep.ranking.size());
    return rep;
}

QVector<FactorInput> MultiFactorEngine::makeInputs(const QVector<Candidate>& candidates,
                                                   const QHash<QString, Snapshot>& snapshots,
                                                   const QHash<QString, KlineSeries>& klines,
                                                   const QDate& asOf)
{
    QVector<FactorInput> out;
    out.reserve(candidates.size());
    for (const Candidate& c : candidates) {
        FactorInput in;
        in.candidate = c;
        in.dabanScore = c.score;
        const auto sit = snapshots.constFind(c.entry.code);
        if (sit != snapshots.constEnd()) in.snapshot = sit.value();
        const auto kit = klines.constFind(c.entry.code);
        if (kit != klines.constEnd())
            in.kline = asOf.isValid() ? kit.value().sliceTo(asOf) : kit.value();
        out.append(in);
    }
    return out;
}
