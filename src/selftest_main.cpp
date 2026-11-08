#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QTextStream>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QObject>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <algorithm>

#include "core/Types.h"
#include "core/Indicators.h"
#include "core/TradePlan.h"
#include "core/Auction.h"
#include "core/RiskFlags.h"
#include <functional>
#include "data/DataHub.h"
#include "strategy/MarketRegime.h"
#include "strategy/FilterPipeline.h"
#include "strategy/RecommendEngine.h"
#include "sentiment/SentimentEngine.h"
#include "sentiment/LongSignalEngine.h"
#include "notify/NotifyConfig.h"
#include "notify/PushService.h"
#include "notify/SmtpClient.h"
#include "backtest/BacktestRunner.h"
#include "review/ReviewEngine.h"
#include "review/DecisionReview.h"
#include "storage/Database.h"
#include "core/Diagnostics.h"
#include "strategy/FactorConfig.h"
#include "strategy/MultiFactorEngine.h"

namespace {

QTextStream& out()
{
    static QTextStream ts(stdout);
    return ts;
}

// 断言失败累计：决定进程退出码，避免「打印了 FAIL 但仍返回 0」让 CI 永远绿
int g_failCount = 0;

void line(const QString& s = QString())
{
    if (s.startsWith(QStringLiteral("[FAIL]"))) ++g_failCount;
    out() << s << Qt::endl;
}

void section(const QString& title)
{
    line();
    line(QStringLiteral("========== %1 ==========").arg(title));
}

// 可选：指定要拉取的交易日（用于验证非交易日 / 历史日期场景）
QDate g_requestDate;

// 等待某个条件成立，或超时返回 false
bool waitFor(const std::function<bool()>& pred, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs) {
        QEventLoop loop;
        QTimer::singleShot(120, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return pred();
}

} // namespace

static int runSelfTest(QCoreApplication& app)
{
    QElapsedTimer timer;
    timer.start();

    section(QStringLiteral("环境自检"));
    line(QStringLiteral("Qt 版本: %1").arg(QString::fromLatin1(qVersion())));

    // ---------------- 数据层 ----------------
    section(QStringLiteral("1. 数据层：拉取涨停/炸板/跌停池"));
    DataHub hub;
    hub.init();

    bool poolOk = false;
    QDate actualDate = g_requestDate.isValid() ? g_requestDate : QDate::currentDate();
    QObject::connect(&hub, &DataHub::limitPoolUpdated,
                     [&](const QDate& d, const QVector<LimitEntry>& up,
                         const QVector<LimitEntry>& zb, const QVector<LimitEntry>& dt) {
        actualDate = d;
        line(QStringLiteral("交易日        : %1").arg(d.toString(Qt::ISODate)));
        line(QStringLiteral("涨停家数      : %1").arg(up.size()));
        line(QStringLiteral("炸板家数      : %1").arg(zb.size()));
        line(QStringLiteral("跌停家数      : %1").arg(dt.size()));
        if (!up.isEmpty()) {
            const LimitEntry& e = up.first();
            line(QStringLiteral("样本校验      : %1 %2  价=%3 元  换手=%4%  "
                                "流通市值=%5亿  首封=%6  板数=%7  板块=%8")
                     .arg(e.code, e.name)
                     .arg(e.price, 0, 'f', 2)
                     .arg(e.turnover, 0, 'f', 2)
                     .arg(e.floatCap / 1e8, 0, 'f', 1)
                     .arg(e.firstSealTime.toString("HH:mm:ss"))
                     .arg(e.boardCount).arg(e.sector));
        }
        poolOk = true;
    });
    QObject::connect(&hub, &DataHub::statusMessage,
                     [&](const QString& s) { line(QStringLiteral("  · %1").arg(s)); });

    hub.refreshLimitPool(g_requestDate.isValid() ? g_requestDate : QDate::currentDate());

    {
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        deadline.setInterval(30000);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        // 数据到位即退出等待
        QTimer poll;
        poll.setInterval(200);
        QObject::connect(&poll, &QTimer::timeout, [&]() {
            if (poolOk) { loop.quit(); }
        });
        poll.start();
        deadline.start();
        loop.exec();
    }
    if (!poolOk) {
        line(QStringLiteral("[FAIL] 数据层未能取得涨停池"));
        return 2;
    }
    line(QStringLiteral("[PASS] 数据层连通，耗时 %1 ms").arg(timer.elapsed()));

    // ---------------- 昨日涨停池（先行指标） ----------------
    section(QStringLiteral("1b. 昨日涨停池：连板晋级率 / 昨日涨停今日溢价"));
    QVector<LimitEntry> prevUp;
    bool prevGot = false;
    QObject::connect(&hub, &DataHub::prevLimitPoolUpdated,
                     [&](const QDate&, const QVector<LimitEntry>& list) {
        if (list.isEmpty()) return;      // 周末/节假日：等下一次回退
        prevUp = list;
        prevGot = true;
    });
    QDate probeDate = actualDate.addDays(-1);
    for (int i = 0; i < 5 && !prevGot; ++i) {
        hub.refreshPrevLimitPool(probeDate);
        if (waitFor([&] { return prevGot; }, 6000)) break;
        probeDate = probeDate.addDays(-1);
    }
    line(QStringLiteral("昨日交易日    : %1").arg(prevGot ? probeDate.toString(Qt::ISODate)
                                                          : QStringLiteral("未取得")));
    line(QStringLiteral("昨日涨停家数  : %1").arg(prevUp.size()));

    // ---------------- 全市场快照（溢价率的分母来源）----------------
    bool snapOk = false;
    QObject::connect(&hub, &DataHub::snapshotUpdated,
                     [&](const QVector<Snapshot>& list) { snapOk = !list.isEmpty(); });
    hub.refreshSnapshot();
    waitFor([&] { return snapOk; }, 40000);
    line(QStringLiteral("全市场快照    : %1 只（%2）")
         .arg(hub.snapshots().size())
         .arg(snapOk ? QStringLiteral("已就绪") : QStringLiteral("超时/失败")));
    {
        // 校验快照索引的 key 与涨停池代码能对上（否则溢价率永远算不出来）
        int covered = 0;
        for (const LimitEntry& e : prevUp)
            if (hub.snapshots().contains(e.code)) ++covered;
        const QString sampleKey = hub.snapshots().isEmpty()
                                      ? QStringLiteral("-")
                                      : hub.snapshots().constBegin().key();
        line(QStringLiteral("快照代码对齐  : 昨日涨停覆盖 %1/%2（快照样本 key=%3）")
                 .arg(covered).arg(prevUp.size()).arg(sampleKey));
        if (!prevUp.isEmpty()) {
            const Snapshot s = hub.snapshots().value(prevUp.first().code);
            line(QStringLiteral("快照字段样本  : %1 %2 价=%3 昨收=%4 涨幅=%5% 有效=%6")
                     .arg(s.code, s.name)
                     .arg(s.price, 0, 'f', 2).arg(s.prevClose, 0, 'f', 2)
                     .arg(s.changePct, 0, 'f', 2)
                     .arg(s.isValid() ? QStringLiteral("是") : QStringLiteral("否")));
        }
    }
    line(QStringLiteral("[%1] 昨日池 + 快照拉取完成，耗时 %2 ms")
         .arg(prevGot && snapOk ? QStringLiteral("PASS") : QStringLiteral("WARN"))
         .arg(timer.elapsed()));

    // ---------------- 概念题材（板块效应主口径）----------------
    bool themesOk = false;
    QObject::connect(&hub, &DataHub::themesUpdated,
                     [&](const QHash<QString, QStringList>&, const QHash<QString, double>&) {
                         themesOk = true;
                     });
    hub.refreshThemes(true);
    waitFor([&] { return themesOk; }, 30000);
    line(QStringLiteral("概念题材      : %1 只标的 / %2 个热门题材（%3）")
         .arg(hub.themesOfStock().size())
         .arg(hub.themeChangePct().size())
         .arg(themesOk ? QStringLiteral("已就绪") : QStringLiteral("失败，退回行业分类")));
    {
        // 题材覆盖率：涨停股中有多少能归入已拉取的题材（覆盖不足会让板块效应失效）
        int covered = 0;
        const QVector<LimitEntry> pool = hub.limitUp();
        for (const LimitEntry& e : pool)
            if (!hub.themesOfStock().value(e.code).isEmpty()) ++covered;
        line(QStringLiteral("题材覆盖      : %1/%2 只涨停股有题材归属").arg(covered).arg(pool.size()));
    }

    // ---------------- 策略层 ----------------
    section(QStringLiteral("2. 策略层：情绪闸门 + 五层筛选"));

    const QVector<LimitEntry>& up = hub.limitUp();
    const QVector<LimitEntry>& zb = hub.brokenPool();
    const QVector<LimitEntry>& dt = hub.limitDownPool();

    MarketRegime reg;
    const QDate tradeDate = actualDate;
    RegimeEngine::computeBase(reg, up, zb, dt, tradeDate);
    RegimeEngine::applyYesterday(reg, prevUp, up, hub.snapshots());
    RegimeEngine::evaluateOn(reg);
    line(QStringLiteral("情绪阶段      : %1").arg(marketPhaseName(reg.phase)));
    line(QStringLiteral("炸板率        : %1%").arg(reg.brokenRate * 100, 0, 'f', 1));
    line(QStringLiteral("最高连板      : %1 板").arg(reg.maxBoardHeight));
    line(QStringLiteral("昨日涨停家数  : %1 家").arg(reg.prevLimitUpCount));
    line(QStringLiteral("连板晋级率    : %1%（昨日连板今日仍涨停的比例）")
         .arg(reg.advanceRate * 100, 0, 'f', 1));
    line(QStringLiteral("首板晋级率    : %1%").arg(reg.firstBoardAdvanceRate * 100, 0, 'f', 1));
    line(QStringLiteral("昨日涨停溢价  : %1%（采样 %2/%3，%4）")
         .arg(reg.prevLimitUpAvgPct, 0, 'f', 2)
         .arg(reg.premiumSampleCount).arg(reg.prevLimitUpCount)
         .arg(reg.premiumKnown ? QStringLiteral("可信") : QStringLiteral("样本不足，仅提示不参与判定")));
    line(QStringLiteral("闸门判定      : %1").arg(reg.tradeAllowed ? QStringLiteral("允许出手")
                                                              : QStringLiteral("禁止出手")));
    line(QStringLiteral("闸门说明      : %1").arg(reg.note));

    FilterPipeline pipeline;
    FilterContext ctx;
    ctx.regime = reg;
    ctx.sectorCount = FilterPipeline::buildSectorCounts(up);
    ctx.themesOfStock = hub.themesOfStock();
    ctx.themeChangePct = hub.themeChangePct();

    const QVector<Candidate> cands = pipeline.run(up, ctx);
    const FilterPipeline::Funnel& f = pipeline.lastFunnel();
    line(QStringLiteral("筛选漏斗      : 输入 %1 → 硬排除 %2 → 板块 %3 → 量价 %4 → 输出 %5")
         .arg(f.input).arg(f.afterHardFilter).arg(f.afterSector)
         .arg(f.afterQuant).arg(f.output));

    if (!cands.isEmpty()) {
        line();
        line(QStringLiteral("候选清单（评分降序）:"));
        line(QStringLiteral("  %1 %2 %3 %4 %5 %6 %7 %8")
             .arg(QStringLiteral("代码"), -7)
             .arg(QStringLiteral("名称"), -10)
             .arg(QStringLiteral("评级"), -4)
             .arg(QStringLiteral("评分"), 5)
             .arg(QStringLiteral("板"), 3)
             .arg(QStringLiteral("换手%"), 7)
             .arg(QStringLiteral("题材(身位)"), -16)
             .arg(QStringLiteral("风险"), -6));
        for (const Candidate& c : cands) {
            const QString theme = c.entry.theme.isEmpty() ? c.entry.sector : c.entry.theme;
            const QString rank = c.entry.themeRank > 0
                ? (c.entry.themeLeader ? QStringLiteral("·龙头")
                                       : QStringLiteral("·第%1").arg(c.entry.themeRank))
                : QString();
            line(QStringLiteral("  %1 %2 %3 %4 %5 %6 %7 %8")
                 .arg(c.entry.code, -7)
                 .arg(c.entry.name, -10)
                 .arg(c.grade, -4)
                 .arg(QString::number(c.score, 'f', 1), 5)
                 .arg(QString::number(c.entry.boardCount), 3)
                 .arg(QString::number(c.entry.turnover, 'f', 1), 7)
                 .arg((theme + rank), -16)
                 .arg(QString::number(c.riskFlags.size()), 6));
        }
    } else {
        line(QStringLiteral("  （本轮无候选 —— 情绪闸门或分层阈值拦截）"));
        line();
        line(QStringLiteral("淘汰原因抽样（前 12 条）:"));
        int shown = 0;
        for (const LimitEntry& e : up) {
            const Candidate c = pipeline.evaluate(e, ctx);
            if (!c.rejectReason.isEmpty() && shown < 12) {
                line(QStringLiteral("  %1 %2  → %3")
                     .arg(e.code, -7).arg(e.name, -10).arg(c.rejectReason));
                ++shown;
            }
        }
    }
    // ---- K 线到位后重算（GUI 的真实路径：候选 → 拉日K → 重跑管线）----
    QHash<QString, KlineSeries> klines;
    if (!cands.isEmpty()) {
        QObject::connect(&hub, &DataHub::klineUpdated,
                         [&](const QString& code, const KlineSeries& s) { klines.insert(code, s); });
        QStringList need;
        for (const Candidate& c : cands) need << c.entry.code;
        // 额外补几只被淘汰的，方便对比
        for (int i = 0; i < up.size() && need.size() < 8; ++i)
            if (!need.contains(up.at(i).code)) need << up.at(i).code;

        hub.refreshKlines(need, 120);
        waitFor([&] { return klines.size() >= need.size(); }, 12000);

        int withRaw = 0;
        for (auto it = klines.constBegin(); it != klines.constEnd(); ++it)
            if (it.value().hasRaw()) ++withRaw;
        line(QStringLiteral("日K（含不复权）: %1/%2 只（%3 只带 rawCandles）%4")
                 .arg(klines.size()).arg(need.size()).arg(withRaw)
                 .arg(klines.isEmpty() ? QStringLiteral("  [WARN] 腾讯日K接口不可达（环境限制），跳过K线重算")
                                       : QString()));

        FilterContext ctxK = ctx;
        ctxK.klines = klines;
        const QVector<Candidate> candsK = pipeline.run(up, ctxK);
        const FilterPipeline::Funnel& fk = pipeline.lastFunnel();
        line(QStringLiteral("含K线重算漏斗: 输入 %1 → 硬排除 %2 → 板块 %3 → 量价 %4 → 输出 %5")
                 .arg(fk.input).arg(fk.afterHardFilter).arg(fk.afterSector)
                 .arg(fk.afterQuant).arg(fk.output));
        if (candsK.isEmpty() && !cands.isEmpty()) {
            line(QStringLiteral("[WARN] K 线到位后候选被清空，逐只打印淘汰原因："));
            for (const LimitEntry& e : up) {
                const Candidate c = pipeline.evaluate(e, ctxK);
                if (c.rejectReason.startsWith(QStringLiteral("首板量比"))
                    || c.rejectReason.startsWith(QStringLiteral("近 "))
                    || c.rejectReason.startsWith(QStringLiteral("前一日"))) {
                    line(QStringLiteral("  %1 %2 → %3").arg(e.code, -7).arg(e.name, -10)
                             .arg(c.rejectReason));
                }
            }
        }
    }
    line(QStringLiteral("[PASS] 策略层执行完毕，耗时 %1 ms").arg(timer.elapsed()));

    // ---------------- 情绪引擎（六维复合指数 / 阈值 / 信号） ----------------
    section(QStringLiteral("2b. 情绪引擎：六维指标 / 阈值 / 做多信号"));

    const SentimentState sent = SentimentEngine::run(
        tradeDate, up, zb, dt, prevUp, hub.snapshots(), hub.themesOfStock(),
        QVector<QPair<QDate, double>>());

    line(QStringLiteral("复合情绪指数  : %1 / 100（%2 档位）")
             .arg(sent.composite, 0, 'f', 1).arg(sentimentTierName(sent.tier)));
    line(QStringLiteral("情绪阶段      : %1").arg(marketPhaseName(sent.phase)));
    line(QStringLiteral("有效仓位上限  : %1%").arg(sent.positionCapPct, 0, 'f', 0));
    line(QStringLiteral("数据可信度    : %1")
             .arg(sent.degraded ? QStringLiteral("降级（存在缺失分项，仓位已折减）")
                                : QStringLiteral("完整")));
    line();
    line(QStringLiteral("六维分项（权重归一后加权）:"));
    int avail = 0;
    for (const SentimentSubScore& sub : sent.subScores) {
        if (sub.available) ++avail;
        line(QStringLiteral("  %1 %2 %3 分  [权重 %4]  %5")
                 .arg(sub.key)
                 .arg(sub.name, -10)
                 .arg(QString::number(sub.score, 'f', 1), 6)
                 .arg(sub.weight, 0, 'f', 2)
                 .arg(sub.detail));
    }
    line(QStringLiteral("结论          : %1").arg(sent.note));

    // ---- 阈值与档位的判定链（合成事实，确定性断言）----
    auto makeFact = [](int lu, int zb, int dt, int maxH, double adv, double prem,
                       double upRatio, double med, int topTheme) {
        SentimentFact f;
        f.date = QDate::currentDate();
        f.limitUpCount = lu;
        f.brokenCount = zb;
        f.limitDownCount = dt;
        f.firstBoardCount = lu * 7 / 10;
        f.multiBoardCount = lu - f.firstBoardCount;
        f.highBoardCount = qMax(0, lu / 10);
        f.maxBoardHeight = maxH;
        f.sealBefore1030 = lu * 6 / 10;
        f.sealAfter1400 = lu / 20;
        f.avgSealToAmount = 0.18;
        f.avgSealToCap = 0.02;
        f.avgTurnover = 15.0;
        f.avgBreakCount = 0.9;
        f.brokenRate = double(zb) / double(qMax(1, lu + zb));
        f.advanceRate = adv;
        f.firstBoardAdvanceRate = adv * 0.6;
        f.premiumPct = prem;
        f.prevLimitUpCount = qMax(10, lu * 6 / 10);
        f.premiumSamples = f.prevLimitUpCount;
        f.premiumKnown = true;
        f.upCount = int(upRatio * 5000);
        f.downCount = 5000 - f.upCount;
        f.sampleCount = 5000;
        f.snapshotLimitLike = lu;      // 正常场景：快照与池子一致
        f.upRatio = upRatio;
        f.medianChangePct = med;
        f.breadthKnown = true;
        f.quotesAligned = true;
        f.themeCount = 7;
        f.topThemeCount = topTheme;
        f.themeFocus = double(topTheme) / double(qMax(1, lu));
        f.themeCoveredStocks = lu * 8 / 10;
        f.themeKnown = true;
        return f;
    };

    const SentimentState cIce = SentimentEngine::evaluate(
        makeFact(18, 24, 14, 2, 0.05, -4.5, 0.20, -2.5, 2));
    const SentimentState cWarm = SentimentEngine::evaluate(
        makeFact(30, 18, 2, 3, 0.22, 0.0, 0.45, 0.0, 4));
    const SentimentState cHot = SentimentEngine::evaluate(
        makeFact(62, 14, 1, 4, 0.42, 2.0, 0.58, 0.8, 7));
    // 退潮确认：指数尚可但较 5 日均值快速回落
    const SentimentState cFall = SentimentEngine::evaluate(
        makeFact(32, 20, 3, 3, 0.22, 0.2, 0.45, -0.2, 3),
        { qMakePair(QDate::currentDate().addDays(-2), 58.0),
          qMakePair(QDate::currentDate().addDays(-1), 56.0) });

    auto describe = [](const SentimentState& s) {
        return QStringLiteral("复合 %1 → %2")
            .arg(s.composite, 0, 'f', 1)
            .arg(sentimentStatusLabel(s.composite, s.positionCapPct, s.tradeAllowed));
    };

    line();
    line(QStringLiteral("判定链校验（合成事实）:"));
    line(QStringLiteral("  冰点（跌停14 / 炸板率57%）: ") + describe(cIce));
    line(QStringLiteral("  回暖（涨停30 / 炸板率37%）: ") + describe(cWarm));
    line(QStringLiteral("  主升（涨停62 / 炸板率18%）: ") + describe(cHot));
    line(QStringLiteral("  退潮（较5日均值 %1）  : ").arg(cFall.trend, 0, 'f', 1) + describe(cFall));

    // 数据交叉校验：池子说 60 家涨停，快照里只有 5 家涨幅≥9.7% → 判数据不可信
    SentimentFact badFact = makeFact(60, 14, 1, 4, 0.42, 2.0, 0.58, 0.8, 7);
    badFact.snapshotLimitLike = 5;
    const SentimentState cBad = SentimentEngine::evaluate(badFact);
    line(QStringLiteral("  数据校验（池 60 / 快照 5）: ") + describe(cBad));

    const bool chainOk = (avail >= 5)
        && cBad.dataSuspect && !cBad.tradeAllowed && cBad.positionCapPct == 0.0
        && cWarm.tier == SentimentTier::Probe
        && !cIce.tradeAllowed && cIce.tier == SentimentTier::NoTrade
        && cIce.positionCapPct == 0.0
        && cWarm.tradeAllowed && cWarm.positionCapPct > 0.0
        && cHot.tradeAllowed && cHot.composite > cWarm.composite
        && cHot.positionCapPct >= cWarm.positionCapPct
        && cFall.phase == MarketPhase::Retreat && cFall.positionCapPct == 0.0;
    line(chainOk ? QStringLiteral("[PASS] 阈值链正确：冰点归零 / 回暖试仓 / 主升加仓 / 趋势回落判退潮")
                 : QStringLiteral("[FAIL] 情绪阈值链异常"));

    // ---- 做多信号与仓位控制 ----
    const QVector<LongSignal> signalList = LongSignalEngine::generate(
        pipeline.lastPassedPreGate(), QHash<QString, KlineSeries>(), sent);
    line();
    line(QStringLiteral("做多信号      : %1 条（可执行 %2）")
             .arg(signalList.size())
             .arg(std::count_if(signalList.cbegin(), signalList.cend(), [](const LongSignal& s) {
                 return s.action != SignalAction::Watch && s.action != SignalAction::None;
             })));
    for (const LongSignal& s : signalList) {
        line(QStringLiteral("  %1 %2 %3 触发 %4~%5 失效 %6 仓位 %7% ｜ %8")
                 .arg(s.code, -7).arg(s.name, -10)
                 .arg(signalActionName(s.action), -4)
                 .arg(s.triggerLow, 0, 'f', 2).arg(s.triggerHigh, 0, 'f', 2)
                 .arg(s.invalidation, 0, 'f', 2)
                 .arg(s.positionPct, 0, 'f', 1)
                 .arg(s.rationale.left(46)));
    }

    // ---- 选股范围：情绪域的高度窗口 ----
    {
        LimitEntry high; high.code = QStringLiteral("600001"); high.boardCount = 5;
        LimitEntry low;  low.code  = QStringLiteral("600002"); low.boardCount  = 2;
        QString highReason, lowReason, coldReason;
        const bool highPass = SentimentEngine::passUniverse(high, cHot, highReason);
        const bool lowPass  = SentimentEngine::passUniverse(low, cHot, lowReason);
        // 禁止开仓时不做结构性收窄（否则观察名单会被清空）
        const bool coldPass = SentimentEngine::passUniverse(high, cIce, coldReason);
        line();
        line(QStringLiteral("情绪域校验    : 主升期 5 板 %1 / 2 板 %2 / 禁止开仓期 5 板 %3")
                 .arg(highPass ? QStringLiteral("通过") : QStringLiteral("剔除"))
                 .arg(lowPass ? QStringLiteral("通过") : QStringLiteral("剔除"))
                 .arg(coldPass ? QStringLiteral("保留为观察") : QStringLiteral("剔除")));
        if (!highPass) line(QStringLiteral("  剔除原因    : %1").arg(highReason));
        line((!highPass && lowPass && coldPass)
                 ? QStringLiteral("[PASS] 情绪域按阶段收窄，但不在禁止开仓时清空观察名单")
                 : QStringLiteral("[FAIL] 情绪域收窄逻辑异常"));
    }

    // ---------------- 荐股计划层 ----------------
    section(QStringLiteral("3. 荐股计划层：买入/止盈/止损区间计算"));

    // 生产路径：下游消费的是情绪引擎产出的 MarketRegime（与 GUI 完全一致）。
    // 第 2 节仍用旧 RegimeEngine 跑一遍，用于证明「原流程未被改动」。
    RecommendEngine recEngine;
    const RecommendBundle bundle =
        recEngine.build(cands, QHash<QString, KlineSeries>(), sent.regime, &pipeline);
    line(QStringLiteral("情绪口径      : %1（复合 %2 / %3 / 上限 %4%）")
             .arg(marketPhaseName(sent.regime.phase))
             .arg(sent.composite, 0, 'f', 1)
             .arg(sentimentTierName(sent.tier))
             .arg(sent.positionCapPct, 0, 'f', 0));

    // ---- 情绪信号写回计划：验证仓位约束（不改变 RecommendEngine 的接口）----
    {
        RecommendBundle applied = bundle;
        LongSignalEngine::apply(applied, signalList, sent);

        double totalPos = 0.0;
        int overSingle = 0;
        int badWatch = 0;
        for (const TradePlan& p : applied.plans) {
            totalPos += p.positionPct;
            if (p.positionPct > 20.0 + 1e-6) ++overSingle;   // 单票上限 20%
            // 观察标的仓位必须为 0
            if (!p.actionable && p.positionPct != 0.0) ++badWatch;
        }
        const bool capOk = totalPos <= sent.positionCapPct + 1e-6;
        const bool signalOk = capOk && overSingle == 0 && badWatch == 0;
        line();
        line(QStringLiteral("信号写回校验  : 可执行 %1 只 · 合计仓位 %2% / 上限 %3% · 超单票 %4 只")
                 .arg(std::count_if(applied.plans.cbegin(), applied.plans.cend(),
                                    [](const TradePlan& p) { return p.actionable; }))
                 .arg(totalPos, 0, 'f', 1)
                 .arg(sent.positionCapPct, 0, 'f', 0)
                 .arg(overSingle));
        line(QStringLiteral("  顶部结论    : %1").arg(applied.notice.left(140)));
        line(signalOk ? QStringLiteral("[PASS] 仓位控制生效（总仓 ≤ 情绪上限 · 单票 ≤ 上限 · 观察不动仓）")
                      : QStringLiteral("[FAIL] 仓位控制越界"));
    }

    line(QStringLiteral("闸门状态      : %1")
         .arg(sent.regime.tradeAllowed ? QStringLiteral("放行 → 可执行荐股")
                                        : QStringLiteral("拦截 → 观察名单")));
    line(QStringLiteral("输出标的      : %1 只（可执行 %2 / 观察 %3）")
         .arg(bundle.plans.size())
         .arg(std::count_if(bundle.plans.cbegin(), bundle.plans.cend(),
                            [](const TradePlan& p) { return p.actionable; }))
         .arg(std::count_if(bundle.plans.cbegin(), bundle.plans.cend(),
                            [](const TradePlan& p) { return !p.actionable; })));

    if (!bundle.plans.isEmpty()) {
        line();
        line(QStringLiteral("荐股清单（代码 / 买入 / 止盈 / 止损 / 盈亏比 / 仓位 / 竞价）:"));
        for (const TradePlan& p : bundle.plans) {
            const QString auction = p.auctionEvaluated
                ? QStringLiteral("%1(%2%)")
                      .arg(Auction::verdictName(static_cast<AuctionAdvice::Verdict>(p.auctionVerdict)))
                      .arg(p.auctionOpenPct, 0, 'f', 2)
                : QStringLiteral("接%1~%2").arg(p.auctionBuyLow, 0, 'f', 2)
                                            .arg(p.auctionBuyHigh, 0, 'f', 2);
            line(QStringLiteral("  %1 %2  买 %3~%4  止 %5~%6  损 %7~%8  RR %9  仓 %10%  %11  竞价 %12")
                 .arg(p.code, -7)
                 .arg(p.name, -10)
                 .arg(p.buyLow, 0, 'f', 2).arg(p.buyHigh, 0, 'f', 2)
                 .arg(p.takeProfitLow, 0, 'f', 2).arg(p.takeProfitHigh, 0, 'f', 2)
                 .arg(p.stopLossLow, 0, 'f', 2).arg(p.stopLossHigh, 0, 'f', 2)
                 .arg(p.riskReward, 0, 'f', 2)
                 .arg(p.positionPct, 0, 'f', 1)
                 .arg(p.actionable ? QStringLiteral("可执行") : QStringLiteral("观察"))
                 .arg(auction));
            // 筹码 / 量能口径：定价的第二套锚点，抽样打印便于人工核对
            if (p.chipSupportHigh > 0.0 || p.volRatio > 0.0) {
                line(QStringLiteral("       筹码 支撑 %1~%2 ｜ 压力 %3~%4 ｜ 获利盘 %5% ｜ 量比 %6 ｜ %7")
                     .arg(p.chipSupportLow, 0, 'f', 2).arg(p.chipSupportHigh, 0, 'f', 2)
                     .arg(p.chipPressureLow, 0, 'f', 2).arg(p.chipPressureHigh, 0, 'f', 2)
                     .arg(p.chipProfitRatio, 0, 'f', 1)
                     .arg(p.volRatio, 0, 'f', 2)
                     .arg(p.supportQuality.isEmpty() ? QStringLiteral("承接质量未判定")
                                                    : p.supportQuality));
            }
        }
        // 区间自洽性校验：买 < 止，损 < 买
        int bad = 0;
        for (const TradePlan& p : bundle.plans) {
            if (!(p.buyLow <= p.buyHigh && p.buyHigh < p.takeProfitLow
                  && p.stopLossLow <= p.stopLossHigh && p.stopLossHigh < p.buyLow)) {
                ++bad;
            }
        }
        line();
        line(QStringLiteral("区间自洽校验  : %1")
             .arg(bad == 0 ? QStringLiteral("全部通过（买<止，损<买）")
                           : QStringLiteral("%1 只异常").arg(bad)));
        if (bad == 0) line(QStringLiteral("[PASS] 交易计划区间自洽"));
        else line(QStringLiteral("[FAIL] 存在区间逻辑异常"));
    } else {
        line(QStringLiteral("  （无可装配标的）"));
        line(QStringLiteral("[WARN] 本轮未产出交易计划 —— 通常意味着情绪闸门拦截且无合格观察标的"));
    }

    // ---- 「打开即见」契约校验 ----
    // 用户诉求：点开就必须看到已算好的荐股（代码 / 买入区间 / 止盈区间 / 止损区间）。
    // 这里用两种情绪状态各跑一次，断言：
    //   ① 闸门放行 → 若产出计划，则至少 1 只可执行，且三段区间自洽
    //   ② 闸门拦截 → 仍产出观察名单（有代码 + 三段区间），且仓位一律为 0
    // 弱市里个股门槛可能全数淘汰（放行也 0 只），属合法结果，不能因此判失败。
    {
        line();
        line(QStringLiteral("「打开即见」契约校验:"));

        // (1) 强制放行
        MarketRegime regOk = sent.regime;
        regOk.tradeAllowed = true;
        regOk.phase = MarketPhase::MainUp;
        const RecommendBundle bOk =
            recEngine.build(cands, QHash<QString, KlineSeries>(), regOk, &pipeline);
        int okActionable = 0;
        int okBad = 0;
        for (const TradePlan& p : bOk.plans) {
            if (p.actionable) ++okActionable;
            if (p.code.isEmpty() || p.buyHigh <= 0.0 || p.takeProfitHigh <= 0.0
                || p.stopLossHigh <= 0.0 || p.buyHigh >= p.takeProfitLow
                || p.stopLossHigh >= p.buyLow) ++okBad;      // 三段区间必须自洽
        }
        line(QStringLiteral("  闸门放行场景  : %1 只（可执行 %2，区间异常 %3）")
             .arg(bOk.plans.size()).arg(okActionable).arg(okBad));

        // (2) 强制拦截
        MarketRegime regNo = sent.regime;
        regNo.tradeAllowed = false;
        regNo.phase = MarketPhase::Retreat;
        const RecommendBundle bNo =
            recEngine.build(cands, QHash<QString, KlineSeries>(), regNo, &pipeline);
        int obsCount = 0;
        int obsBad = 0;
        for (const TradePlan& p : bNo.plans) {
            if (p.actionable) continue;
            ++obsCount;
            if (p.positionPct != 0.0) ++obsBad;          // 观察名单不得给仓位
            if (p.code.isEmpty() || p.buyHigh <= 0.0 || p.takeProfitHigh <= 0.0
                || p.stopLossHigh <= 0.0) ++obsBad;        // 三段区间必须齐备
        }
        line(QStringLiteral("  闸门拦截场景  : %1 只（观察 %2，仓位/区间异常 %3）")
             .arg(bNo.plans.size()).arg(obsCount).arg(obsBad));

        const bool contractOk = (okBad == 0) && (obsBad == 0)
                             && (bOk.plans.isEmpty() || okActionable > 0);
        if (bOk.plans.isEmpty()) {
            line(QStringLiteral("  [WARN] 放行场景无标的：当日个股门槛全数淘汰，非契约失败"));
        }
        if (contractOk) {
            line(QStringLiteral("[PASS] 两种情绪状态下均产出「代码+买入/止盈/止损」的完整清单"));
        } else {
            line(QStringLiteral("[FAIL] 「打开即见」契约未满足"));
        }
    }

    // ---------------- 多因子评分：实盘装配链 vs 回测直接链 ----------------
    section(QStringLiteral("3b. 多维度做多因子评分（实盘/回测一致性）"));
    {
        const FactorConfig cfg = FactorConfig::defaults();
        const QHash<QString, Snapshot> emptySnaps;
        const QHash<QString, KlineSeries> emptyKlines;

        // 回测路径：直接调用统一引擎
        const QVector<FactorInput> inputs = MultiFactorEngine::makeInputs(
            pipeline.lastPassedPreGate(), emptySnaps, emptyKlines, sent.regime.date);
        const FactorReport direct = MultiFactorEngine::rank(inputs, cfg);

        // 实盘路径：经 RecommendEngine::build 内部调用同一个 rank()
        recEngine.setFactorConfig(cfg);
        recEngine.setFactorData(&emptySnaps, &emptyKlines, sent.regime.date);
        const RecommendBundle bGui = recEngine.build(cands, emptyKlines, sent.regime, &pipeline);
        line(QStringLiteral("配置指纹    : %1 ｜ %2").arg(direct.configDigest, cfg.summary()));
        line(QStringLiteral("回测链入榜  : %1 只 ｜ 实盘链计划 %2 只")
                 .arg(direct.ranking.size()).arg(bGui.plans.size()));

        bool same = (recEngine.lastFactorReport().configDigest == direct.configDigest);
        int compared = 0;
        for (const TradePlan& p : bGui.plans) {
            for (const FactorEvaluation& ev : direct.ranking) {
                if (ev.code != p.code) continue;
                ++compared;
                if (qAbs(ev.composite - p.compositeScore) > 1e-6) same = false;
            }
        }
        line(QStringLiteral("逐只比对    : %1 只综合评分一致").arg(compared));
        // 贡献度守恒：各维度贡献之和必须等于综合分（加权求和规则）
        bool additive = true;
        for (const FactorEvaluation& ev : direct.ranking) {
            double sum = 0.0;
            for (const FactorContribution& fc : ev.contributions) sum += fc.contribution;
            if (qAbs(sum - ev.composite) > 1e-6) additive = false;
        }
        line(additive ? QStringLiteral("[PASS] 维度贡献之和等于综合评分")
                      : QStringLiteral("[FAIL] 维度贡献度分解不守恒"));
        line(same ? QStringLiteral("[PASS] 实盘与回测共用同一多因子引擎，综合评分一致")
                  : QStringLiteral("[FAIL] 实盘/回测综合评分不一致"));
        recEngine.setFactorData(nullptr, nullptr, QDate());   // 局部数据已离开作用域，清空指针
    }

    // ---------------- 指标层单元校验 ----------------
    section(QStringLiteral("4. 指标层：涨停价与涨停判定"));
    line(QStringLiteral("主板涨停价(昨收10.00)  : %1 元").arg(
        Ind::calcLimitUpPrice(10.00, Board::MainBoard), 0, 'f', 2));
    line(QStringLiteral("创业板涨停价(昨收10.00): %1 元").arg(
        Ind::calcLimitUpPrice(10.00, Board::ChiNext), 0, 'f', 2));
    line(QStringLiteral("主板 11.00 是否涨停     : %1").arg(
        Ind::isLimitUp(11.00, 10.00, Board::MainBoard) ? QStringLiteral("是") : QStringLiteral("否")));
    line(QStringLiteral("主板 10.50 是否涨停     : %1").arg(
        Ind::isLimitUp(10.50, 10.00, Board::MainBoard) ? QStringLiteral("是") : QStringLiteral("否")));
    line(QStringLiteral("创业板 12.00 是否涨停   : %1").arg(
        Ind::isLimitUp(12.00, 10.00, Board::ChiNext) ? QStringLiteral("是") : QStringLiteral("否")));

    // ---------------- 竞价 / 风险 ----------------
    section(QStringLiteral("5. 次日竞价规则 与 风险标记"));

    LimitEntry sample;
    sample.code = QStringLiteral("600000");
    sample.name = QStringLiteral("样本股");
    sample.board = Board::MainBoard;
    sample.boardCount = 2;
    sample.limitPrice = 11.00;
    sample.amount = 5.0e8;

    const AuctionRule arule = Auction::ruleFor(sample);
    line(QStringLiteral("二板竞价规则  : 高开 %1%~%2%，低开 %3% 放弃")
         .arg(arule.minOpenPct, 0, 'f', 1).arg(arule.maxOpenPct, 0, 'f', 1)
         .arg(arule.abandonPct, 0, 'f', 1));

    struct Case { const char* name; double open; double amount; };
    const Case cases[] = {
        { "高开 3.6% 竞价额充足", 11.40, 3.0e7 },
        { "高开 9.1% 溢价吃光  ", 12.00, 3.0e7 },
        { "低开 -5.5% 破位     ", 10.40, 3.0e7 },
    };
    int auctionPass = 0;
    for (const Case& c : cases) {
        const AuctionAdvice adv = Auction::evaluate(arule, true, c.open, 11.00, c.amount, 5.0e8, true);
        line(QStringLiteral("  %1 → %2（高开 %3%）")
             .arg(QString::fromUtf8(c.name))
             .arg(Auction::verdictName(adv.verdict))
             .arg(adv.openPct, 0, 'f', 2));
        if (adv.verdict != AuctionAdvice::Unknown) ++auctionPass;
    }
    line(auctionPass == 3 ? QStringLiteral("[PASS] 竞价三档判定均生效")
                          : QStringLiteral("[FAIL] 竞价判定异常"));

    Snapshot riskSnap;
    riskSnap.code = sample.code;
    riskSnap.name = sample.name;
    riskSnap.price = 11.0;
    riskSnap.pe = -12.5;            // 亏损股
    sample.boardCount = 6;          // 高位
    const QStringList flags = Risk::evaluate(sample, riskSnap, nullptr);
    line(QStringLiteral("风险标记      : %1 条").arg(flags.size()));
    for (const QString& f : flags) line(QStringLiteral("  · %1").arg(f));
    line(flags.isEmpty() ? QStringLiteral("[FAIL] 风险标记未命中")
                         : QStringLiteral("[PASS] 风险标记生效"));

    // ---------------- 交易日志统计 ----------------
    section(QStringLiteral("5b. 交易日志：胜率 / 期望 / 回撤"));
    QVector<TradeRecord> demo;
    auto mkTrade = [](const QString& code, double buy, double sell, int qty, int hold) {
        TradeRecord r;
        r.code = code; r.name = code;
        r.buyDate = QDate::currentDate().addDays(-hold - 1);
        r.sellDate = r.buyDate.addDays(hold);
        r.buyPrice = buy; r.sellPrice = sell; r.qty = qty;
        r.planBuy = buy; r.planStop = buy * 0.95; r.planTake = buy * 1.05;
        return r;
    };
    demo << mkTrade(QStringLiteral("600001"), 10.00, 10.60, 10000, 1)   // +6%
         << mkTrade(QStringLiteral("600002"), 20.00, 19.30, 10000, 1)   // -3.5%
         << mkTrade(QStringLiteral("600003"), 15.00, 16.20, 10000, 2);  // +8%
    const TradeStats st = computeTradeStats(demo);
    line(QStringLiteral("笔数          : %1（已平仓 %2）").arg(st.total).arg(st.closedCount));
    line(QStringLiteral("胜率          : %1%").arg(st.winRate * 100, 0, 'f', 1));
    line(QStringLiteral("平均盈利/亏损 : %1% / %2%").arg(st.avgWinPct, 0, 'f', 2).arg(st.avgLossPct, 0, 'f', 2));
    line(QStringLiteral("每笔期望      : %1%").arg(st.expectancy, 0, 'f', 2));
    line(QStringLiteral("最大回撤      : %1%").arg(st.maxDrawdown, 0, 'f', 2));
    line(st.closedCount == 3 && st.winCount == 2
             ? QStringLiteral("[PASS] 交易统计口径正确（2 胜 1 负）")
             : QStringLiteral("[FAIL] 交易统计异常"));

    // ---------------- 持仓浮动盈亏（交易日志「持仓中」应显示盈亏） ----------------
    {
        TradeRecord hold;
        hold.code = QStringLiteral("600004");
        hold.name = hold.code;
        hold.buyDate = QDate::currentDate().addDays(-2);
        hold.buyPrice = 12.00;
        hold.qty = 10000;
        const double px = 12.60;

        const double buyAmt = hold.buyAmount();                       // 120000
        const double feeAll = buyAmt * TradeRecord::buyFeeRate()
                            + px * hold.qty * TradeRecord::sellFeeRate();
        const bool floatOk = !hold.closed()
            && qAbs(hold.feeAt(px) - feeAll) < 1e-6
            && qAbs(hold.pnlAt(px) - (px * hold.qty - buyAmt - feeAll)) < 1e-6
            && qAbs(hold.pnlPctAt(px) - hold.pnlAt(px) / buyAmt * 100.0) < 1e-9
            && hold.holdingDays(QDate::currentDate()) == 2;

        // 了结后必须与「已实现」完全一致：同一函数、同一价格，不该有第二套口径
        TradeRecord done = hold;
        done.sellDate = QDate::currentDate();
        done.sellPrice = px;
        const bool sameOk = qAbs(done.pnl() - hold.pnlAt(px)) < 1e-9
                         && qAbs(done.pnlPct() - hold.pnlPctAt(px)) < 1e-9;

        line(QStringLiteral("持仓浮动盈亏  : %1 元 / %2%（成本 %3，现价 %4，含费用 %5）")
                 .arg(hold.pnlAt(px), 0, 'f', 2).arg(hold.pnlPctAt(px), 0, 'f', 2)
                 .arg(buyAmt, 0, 'f', 0).arg(px, 0, 'f', 2).arg(feeAll, 0, 'f', 2));
        line(floatOk && sameOk
                 ? QStringLiteral("[PASS] 浮动盈亏与已实现盈亏同源（持仓中按现价可算）")
                 : QStringLiteral("[FAIL] 浮动盈亏口径异常"));
    }

    // ---------------- 存储层 ----------------
    section(QStringLiteral("6. 存储层：SQLite 读写"));
    Database db;
    const QString dbPath = QDir::current().filePath(QStringLiteral("daban_selftest.db"));
    QFile::remove(dbPath);
    if (!db.open(dbPath)) {
        line(QStringLiteral("[FAIL] 数据库打开失败: %1").arg(db.lastError()));
        return 3;
    }
    line(QStringLiteral("数据库路径    : %1").arg(db.path()));

    // 情绪序列：写入 → 读回（趋势 / 加速度 / 退潮确认的跨日依据）
    const bool sentSaved = db.saveSentiment(sent);
    const QVector<QPair<QDate, double>> sentHist = db.sentimentHistory(tradeDate, 10);
    const SentimentState sentBack = db.loadSentiment(tradeDate);
    const bool sentOk = sentSaved && !sentHist.isEmpty()
                     && sentBack.date == tradeDate
                     && qAbs(sentBack.composite - sent.composite) < 0.001;
    line(QStringLiteral("情绪序列存取  : %1 条（复合 %2 / 档位 %3）")
             .arg(sentHist.size())
             .arg(sentBack.composite, 0, 'f', 1)
             .arg(sentimentTierName(sentBack.tier)));
    line(sentOk ? QStringLiteral("[PASS] 情绪序列可持久化（趋势指标闭环）")
                : QStringLiteral("[FAIL] 情绪序列存取异常"));

    // 键值配置：推送去重指纹的落点（跨会话不重复发信）
    db.setSetting(QStringLiteral("notify.lastDigest"), QStringLiteral("abc123"));
    const bool settingOk = db.getSetting(QStringLiteral("notify.lastDigest")) == QStringLiteral("abc123")
                        && db.getSetting(QStringLiteral("notify.missing"), QStringLiteral("def"))
                               == QStringLiteral("def");
    line(QStringLiteral("键值配置存取  : %1")
             .arg(settingOk ? QStringLiteral("读写一致") : QStringLiteral("异常")));
    line(settingOk ? QStringLiteral("[PASS] 推送去重状态可持久化")
                   : QStringLiteral("[FAIL] 键值配置存取异常"));

    // 荐股计划存取（离线兜底：下次打开直接呈现这份已算好的结果）
    if (!bundle.plans.isEmpty()) {
        db.saveRecommend(bundle);
        const RecommendBundle rb = db.loadRecommend(bundle.tradeDate);
        bool same = (rb.plans.size() == bundle.plans.size());
        // 除区间外，还要核对「决策要素」是否完整往返：
        // 止损幅度 / 风险标记 / 成交概率 / 信号级别 / 题材身位 / 竞价方案。
        // 这些字段一旦漏存，「断网看缓存」时首页会整片空掉（止损幅度显示 -、
        // 竞价显示「接 0.00~0.00」），用户会以为程序坏了。
        int lostFields = 0;
        for (int i = 0; same && i < rb.plans.size(); ++i) {
            const TradePlan& a = bundle.plans[i];
            const TradePlan& b = rb.plans[i];
            if (a.code != b.code || qAbs(a.buyLow - b.buyLow) > 0.001
                || qAbs(a.takeProfitHigh - b.takeProfitHigh) > 0.001
                || qAbs(a.stopLossHigh - b.stopLossHigh) > 0.001) {
                same = false;
            }
            if (qAbs(a.riskPct - b.riskPct) > 0.001)              ++lostFields;
            if (a.riskFlags != b.riskFlags)                       ++lostFields;
            if (qAbs(a.fillProbability - b.fillProbability) > 0.001) ++lostFields;
            if (a.signalLevel != b.signalLevel)                   ++lostFields;
            if (a.theme != b.theme)                               ++lostFields;
            if (a.themeRank != b.themeRank)                       ++lostFields;
            if (a.auctionEvaluated != b.auctionEvaluated)         ++lostFields;
            if (qAbs(a.auctionBuyLow - b.auctionBuyLow) > 0.001)  ++lostFields;
        }
        // 筛选漏斗也要往返：漏斗不落库，断网看缓存时首页就只剩一条空的漏斗条
        const bool funnelOk = (rb.funnel.size() == bundle.funnel.size())
                           && (rb.rejects.size() == bundle.rejects.size());
        line(QStringLiteral("荐股计划存取  : %1 条（%2）")
             .arg(rb.plans.size())
             .arg(same ? QStringLiteral("区间逐项一致") : QStringLiteral("内容不一致")));
        line(QStringLiteral("决策要素往返  : %1")
             .arg(lostFields == 0
                  ? QStringLiteral("止损幅度/风险标记/成交概率/信号/题材/竞价 全部保留")
                  : QStringLiteral("丢失 %1 项").arg(lostFields)));
        line(QStringLiteral("筛选漏斗往返  : %1")
             .arg(funnelOk
                  ? QStringLiteral("%1 层 + %2 条淘汰明细 完整保留")
                        .arg(rb.funnel.size()).arg(rb.rejects.size())
                  : QStringLiteral("漏斗/淘汰明细有丢失")));
        if (same && lostFields == 0 && funnelOk) {
            line(QStringLiteral("[PASS] 荐股计划可持久化（离线兜底可用，决策要素与漏斗无丢失）"));
        } else {
            line(QStringLiteral("[FAIL] 荐股计划存取内容不一致或决策要素丢失"));
        }
        // 多因子结果也要往返：断网看缓存时排名与贡献度不能消失
        bool factorOk = (rb.plans.size() == bundle.plans.size());
        for (int i = 0; factorOk && i < rb.plans.size(); ++i) {
            const TradePlan& a = bundle.plans[i];
            const TradePlan& b = rb.plans[i];
            if (qAbs(a.compositeScore - b.compositeScore) > 1e-6
                || a.factorRank != b.factorRank
                || a.contributions.size() != b.contributions.size()) factorOk = false;
        }
        line(factorOk ? QStringLiteral("[PASS] 多因子综合分/排名/维度贡献度往返完整")
                      : QStringLiteral("[FAIL] 多因子评分缓存往返丢失"));

        // 得分由来也要往返：断网时「查看得分由来」仍要能摊开公式与逐项明细
        bool basisOk = factorOk;
        for (int i = 0; basisOk && i < rb.plans.size(); ++i) {
            const TradePlan& a = bundle.plans[i];
            const TradePlan& b = rb.plans[i];
            if (a.dabanBreakdown.items.size() != b.dabanBreakdown.items.size()
                || a.compositeBreakdown.items.size() != b.compositeBreakdown.items.size()
                || a.dabanBreakdown.formula != b.dabanBreakdown.formula)
                basisOk = false;
        }
        const int dabanItems = rb.plans.isEmpty()
            ? 0 : int(rb.plans.first().dabanBreakdown.items.size());
        const int compItems = rb.plans.isEmpty()
            ? 0 : int(rb.plans.first().compositeBreakdown.items.size());
        line(QStringLiteral("得分由来往返  : 打板 %1 项 / 综合 %2 项").arg(dabanItems).arg(compItems));
        line(basisOk ? QStringLiteral("[PASS] 得分由来可持久化（断网也能核对公式与明细）")
                     : QStringLiteral("[FAIL] 得分由来缓存往返丢失"));
    }

    KlineSeries probe;
    probe.code = up.isEmpty() ? QStringLiteral("600000") : up.first().code;
    Candle bar;
    bar.date = QDate::currentDate();
    bar.open = 10.0; bar.close = 10.5; bar.high = 10.8; bar.low = 9.9;
    bar.volume = 1000.0; bar.amount = 1.0e6;
    probe.candles.append(bar);
    db.saveKline(probe);
    line(QStringLiteral("K线缓存存取   : %1 根").arg(db.loadKline(probe.code).candles.size()));

    // 交易日志：写入 → 编辑（补录了结信息）→ 读回 → 统计（无独立平仓接口）
    TradeRecord jr;
    jr.buyDate = QDate::currentDate();
    jr.code = QStringLiteral("600519");
    jr.name = QStringLiteral("自检样本");
    jr.buyPrice = 10.0;
    jr.qty = 1000;
    jr.planBuy = 10.0;
    jr.planStop = 9.5;
    jr.planTake = 10.5;
    const bool journalAdd = db.addTrade(jr) && jr.id > 0;
    jr.sellDate = QDate::currentDate();
    jr.sellPrice = 10.6;
    jr.note = QStringLiteral("编辑补录了结");
    const bool journalEdit = db.updateTrade(jr);
    const QVector<TradeRecord> jlist = db.trades();
    bool journalOk = journalAdd && journalEdit && !jlist.isEmpty() && jlist.first().closed();
    if (journalOk) journalOk = qAbs(jlist.first().pnlPct() - 5.7) < 1.0;
    line(QStringLiteral("交易日志存取  : %1 条（了结 %2）")
         .arg(jlist.size())
         .arg(jlist.isEmpty() ? QStringLiteral("-")
                              : QStringLiteral("%1%").arg(jlist.first().pnlPct(), 0, 'f', 2)));
    line(journalOk ? QStringLiteral("[PASS] 交易日志可落库、可编辑并计算盈亏")
                   : QStringLiteral("[FAIL] 交易日志存取异常"));

    line(QStringLiteral("[PASS] 存储层执行完毕，耗时 %1 ms").arg(timer.elapsed()));

    // ---------------- 回测结算口径 ----------------
    section(QStringLiteral("7. 回测结算口径（合成 K 线，不走网络）"));
    {
        TradePlan p;
        p.code = QStringLiteral("TEST01");
        p.name = QStringLiteral("样本");
        p.buyLow = 10.0;
        p.buyHigh = 10.5;
        p.stopLossHigh = 9.5;
        p.takeProfitLow = 11.0;

        auto mk = [](const QDate& d0, double o1, double h1, double l1, double c1,
                     double o2, double h2, double l2, double c2) {
            KlineSeries s;
            Candle a;
            a.date = d0; a.open = 10.0; a.high = 10.3; a.low = 9.9; a.close = 10.1; a.volume = 1e5;
            Candle b;
            b.date = d0.addDays(1); b.open = o1; b.high = h1; b.low = l1; b.close = c1; b.volume = 1e5;
            Candle c;
            c.date = d0.addDays(2); c.open = o2; c.high = h2; c.low = l2; c.close = c2; c.volume = 1e5;
            s.candles = { a, b, c };
            s.rawCandles = { a, b, c };
            return s;
        };
        const QDate d0(2026, 1, 5);
        const BacktestTrade win  = BacktestRunner::simulate(
            p, QStringLiteral("试仓"), mk(d0, 10.2, 10.4, 10.1, 10.3, 10.4, 11.2, 10.3, 11.1), d0, 0.8);
        const BacktestTrade stop = BacktestRunner::simulate(
            p, QStringLiteral("试仓"), mk(d0, 10.2, 10.4, 10.1, 10.3, 10.4, 10.5, 9.4, 9.6), d0, 0.8);
        const BacktestTrade gap  = BacktestRunner::simulate(
            p, QStringLiteral("试仓"), mk(d0, 10.2, 10.4, 10.1, 10.3, 9.0, 9.2, 8.9, 9.1), d0, 0.8);
        const BacktestTrade oneWord = BacktestRunner::simulate(
            p, QStringLiteral("试仓"), mk(d0, 10.6, 10.6, 10.6, 10.6, 10.7, 10.9, 10.6, 10.8), d0, 0.8);
        const BacktestTrade highOpen = BacktestRunner::simulate(
            p, QStringLiteral("试仓"), mk(d0, 11.0, 11.4, 10.9, 11.2, 11.2, 11.5, 11.0, 11.3), d0, 0.8);

        line(QStringLiteral("  止盈场景    : 成交 %1 元 → 出场 %2 元 · %3%")
                 .arg(win.entry, 0, 'f', 2).arg(win.exit, 0, 'f', 2).arg(win.pnlPct, 0, 'f', 2));
        line(QStringLiteral("  止损场景    : 成交 %1 元 → 出场 %2 元 · %3%")
                 .arg(stop.entry, 0, 'f', 2).arg(stop.exit, 0, 'f', 2).arg(stop.pnlPct, 0, 'f', 2));
        line(QStringLiteral("  跳空止损    : 出场 %1 元 · %2%")
                 .arg(gap.exit, 0, 'f', 2).arg(gap.pnlPct, 0, 'f', 2));
        line(QStringLiteral("  买不到场景  : 一字板=%1 · 高开=%2")
                 .arg(oneWord.filled ? QStringLiteral("成交") : oneWord.outcome,
                      highOpen.filled ? QStringLiteral("成交") : highOpen.outcome));

        const bool simOk = win.filled && qAbs(win.pnlPct - 7.62) < 0.06
                        && stop.filled && qAbs(stop.pnlPct + 7.08) < 0.06
                        && gap.filled && qAbs(gap.pnlPct + 11.98) < 0.06
                        && !oneWord.filled && !highOpen.filled;
        line(simOk ? QStringLiteral("[PASS] T+1 买入 / T+2 止盈止损 / 买不到判定口径正确")
                   : QStringLiteral("[FAIL] 回测结算口径异常"));
    }

    // ---------------- 复盘助手：学习闭环 ----------------
    section(QStringLiteral("8. 复盘助手：复盘 → 预判 → 校验 → 迭代"));
    bool assistantOk = false;
    {
        auto mkState = [](const QDate& d, int up, int broken, int down, int maxBoard,
                          double adv, double prem, bool premKnown, double comp,
                          SentimentTier tier, bool allowed) {
            SentimentState s;
            s.date = d;
            s.fact.date = d;
            s.fact.limitUpCount = up;
            s.fact.brokenCount = broken;
            s.fact.limitDownCount = down;
            s.fact.maxBoardHeight = maxBoard;
            s.fact.prevLimitUpCount = 40;
            s.fact.brokenRate = (up + broken) > 0 ? double(broken) / (up + broken) : 0.0;
            s.fact.advanceRate = adv;
            s.fact.premiumPct = prem;
            s.fact.premiumKnown = premKnown;
            s.fact.upRatio = 0.5;
            s.fact.breadthKnown = true;
            s.composite = comp;
            s.tier = tier;
            s.phase = allowed ? MarketPhase::MainUp : MarketPhase::Retreat;
            s.tradeAllowed = allowed;
            s.positionCapPct = allowed ? 42.0 : 0.0;
            return s;
        };

        const QDate d1(2026, 10, 29);
        ReviewInput in;
        in.date = d1;
        in.sentiment = mkState(d1, 60, 12, 1, 5, 0.55, 3.0, true, 72.0,
                               SentimentTier::Build, true);
        in.funnelInput = 60;
        in.funnelHard = 20;
        in.funnelSector = 12;
        in.funnelQuant = 6;
        in.funnelOut = 3;

        const ReviewReport rep = ReviewEngine::buildReview(in);
        QVector<AssistantRule> rules = ReviewEngine::defaultRules();
        const Forecast f1 = ReviewEngine::buildForecast(rep, rules, 0.0);

        // 实际大幅低于预判（涨停 30 vs 预判 ~50+）
        const SentimentState bad = mkState(d1.addDays(1), 30, 26, 3, 3, 0.22, -2.5, true, 41.0,
                                           SentimentTier::NoTrade, false);
        const ForecastEval e1 = ReviewEngine::evaluate(f1, bad, in);
        const QVector<RuleUpdate> ups = ReviewEngine::updateRules(rules, f1, e1);
        const double bias = ReviewEngine::updateBias(0.0, f1, e1);

        // 迭代后再出一次预判：区间应整体下移
        const Forecast f2 = ReviewEngine::buildForecast(rep, rules, bias);

        // 命中场景：实际落在新区间
        SentimentState good = mkState(d1.addDays(1), f2.limitUpCenter, 10, 1,
                                      (f2.maxBoardLow + f2.maxBoardHigh) / 2, 0.5,
                                      (f2.advanceLow + f2.advanceHigh) / 2, true, 65.0,
                                      SentimentTier::Build, true);
        good.fact.maxBoardHeight = f2.maxBoardLow;
        const ForecastEval e2 = ReviewEngine::evaluate(f2, good, in);

        line(QStringLiteral("  复盘        : %1").arg(rep.headline));
        line(QStringLiteral("  首次预判    : %1 ｜ 涨停 %2~%3 ｜ 高度 %4~%5 ｜ 置信 %6%")
                 .arg(f1.stance).arg(f1.limitUpLow).arg(f1.limitUpHigh)
                 .arg(f1.maxBoardLow).arg(f1.maxBoardHigh)
                 .arg(f1.confidence * 100, 0, 'f', 0));
        line(QStringLiteral("  偏差评估    : %1 分 · 偏差 %2 条 · 反思 %3 条")
                 .arg(e1.score).arg(e1.deviations.size()).arg(e1.lessons.size()));
        line(QStringLiteral("  规则更新    : %1 条 · 幅度校正 %2")
                 .arg(ups.size()).arg(bias, 0, 'f', 3));
        line(QStringLiteral("  迭代后预判  : 涨停 %1~%2（中心 %3）")
                 .arg(f2.limitUpLow).arg(f2.limitUpHigh).arg(f2.limitUpCenter));
        line(QStringLiteral("  命中评估    : %1 分 · %2")
                 .arg(e2.score).arg(e2.hit ? QStringLiteral("命中") : QStringLiteral("偏差")));

        const bool reviewOk = rep.valid && !rep.html.isEmpty()
                           && rep.facts.value(QStringLiteral("limitUp")).toInt() == 60;
        const bool devOk = !e1.hit && e1.score < 65 && e1.deviations.size() >= 2
                        && !e1.lessons.isEmpty() && !ups.isEmpty() && bias < 0.0;
        const bool iterOk = f2.limitUpCenter < f1.limitUpCenter;
        const bool hitOk = e2.hit && e2.score >= 65;

        line(reviewOk ? QStringLiteral("[PASS] 复盘报告生成（含结构化事实）")
                      : QStringLiteral("[FAIL] 复盘报告异常"));
        line(devOk ? QStringLiteral("[PASS] 偏差归因 + 规则权重下调 + 幅度校正生效")
                   : QStringLiteral("[FAIL] 迭代逻辑异常"));
        line(iterOk ? QStringLiteral("[PASS] 迭代后预判区间下移（学习生效）")
                    : QStringLiteral("[FAIL] 迭代未影响预判"));
        line(hitOk ? QStringLiteral("[PASS] 命中判定与策略沉淀口径正确")
                   : QStringLiteral("[FAIL] 命中判定异常"));

        // 落库闭环：复盘 / 预判 / 评估 / 规则 / 策略 / 教训
        const bool dbOk = db.saveReview(rep)
                       && db.loadReview(d1).headline == rep.headline
                       && db.saveForecast(f2) && db.loadForecast(d1.addDays(1)).valid
                       && db.saveRules(rules) && !db.loadRules().isEmpty()
                       && db.saveForecastEval(e2) && !db.recentEvals(5).isEmpty()
                       && db.touchStrategy(f1.stance, true) && !db.strategies().isEmpty()
                       && db.addLesson(d1, QStringLiteral("反思"), QStringLiteral("自检样本"))
                       && !db.recentLessons(3).isEmpty();
        line(dbOk ? QStringLiteral("[PASS] 复盘/预判/评估/规则/策略/教训 均可落库")
                  : QStringLiteral("[FAIL] 复盘助手落库异常"));

        assistantOk = reviewOk && devOk && iterOk && hitOk && dbOk;
    }

    // ---------------- 8b. 决策复盘：条件证据 / 偏差 / 板块留档可回看 ----------------
    {
        section(QStringLiteral("8b. 决策复盘：信号条件 → 事后偏差 → 留档回看"));
        const QDate dd(2026, 11, 6);
        DecisionReview::Input in;
        in.date = dd; in.asOf = dd;
        TradePlan p;
        p.code = QStringLiteral("000001"); p.name = QStringLiteral("自检标的");
        p.actionable = true; p.signalLevel = 3; p.positionPct = 6.0;
        p.buyLow = 9.50; p.buyHigh = 10.00;
        p.takeProfitLow = 10.50; p.takeProfitHigh = 11.00;
        p.stopLossLow = 9.20; p.stopLossHigh = 9.40;
        p.riskReward = 1.20; p.fillProbability = 0.60;
        in.plans << p;
        KlineSeries k;
        k.code = p.code;
        for (int i = 0; i < 26; ++i) {
            Candle c;
            c.date = dd.addDays(i - 20);
            c.open = 10.0; c.close = 10.0; c.high = 10.05; c.low = 9.95;
            c.volume = 100000.0; c.amount = 10000000.0;
            k.candles << c;
            k.rawCandles << c;
        }
        in.klines.insert(p.code, k);
        DecisionReview::SectorFlowRow row;
        row.code = QStringLiteral("BK0001"); row.name = QStringLiteral("自检板块");
        row.changePct = 3.2; row.mainNetYi = 8.6; row.mainPct = 1.1; row.flowKnown = true;
        in.sectorRows << row;
        in.sectorFetchedAt = QDateTime::currentDateTime();
        in.sectorSource = QStringLiteral("自检：东财 clist f3/f62/f184");

        const DecisionReview::Report dr = DecisionReview::build(in);
        const bool saved = db.saveDecisionReview(dr);
        const DecisionReview::Report loaded = db.loadDecisionReview(dd);
        const bool roundTrip = saved && loaded.valid
                            && loaded.signalTraces.size() == dr.signalTraces.size()
                            && loaded.outcomes.size() == dr.outcomes.size()
                            && loaded.skipped.size() == dr.skipped.size()
                            && loaded.criteria == dr.criteria
                            && qAbs(loaded.outcomes.value(0).mfePct - dr.outcomes.value(0).mfePct) < 1e-6;

        QVector<DecisionReview::SectorFlowRow> rowsIn;
        rowsIn << row;
        const bool flowSaved = db.saveSectorFlows(dd, in.sectorFetchedAt, rowsIn, in.sectorSource);
        QDateTime fetchedAt;
        QString source, reason;
        const QVector<DecisionReview::SectorFlowRow> rowsOut =
            db.loadSectorFlows(dd, &fetchedAt, &source, &reason);
        const QDate failDate = dd.addDays(1);
        const bool failSaved = db.saveSectorFlowFailure(failDate, QStringLiteral("自检：接口失败"));
        QString failReason;
        db.loadSectorFlows(failDate, nullptr, nullptr, &failReason);

        line(QStringLiteral("  结构化报告  : 信号 %1 条 · 条件 %2 条 · 偏差 %3 条 · 跳过 %4 条 · 判定条件 %5 条")
                 .arg(dr.signalTraces.size()).arg(dr.signalTraces.value(0).conditions.size())
                 .arg(dr.outcomes.size()).arg(dr.skipped.size()).arg(dr.criteria.size()));
        line(QStringLiteral("  事后偏差    : 窗口 %1 根 · MFE %2% · MAE %3% · 目标偏差 %4%")
                 .arg(dr.outcomes.value(0).bars)
                 .arg(dr.outcomes.value(0).mfePct, 0, 'f', 2)
                 .arg(dr.outcomes.value(0).maePct, 0, 'f', 2)
                 .arg(dr.outcomes.value(0).targetDeviationPct, 0, 'f', 2));
        line(QStringLiteral("  板块留档    : %1 行 · 失败原因留档 %2")
                 .arg(rowsOut.size()).arg(failReason));

        const bool decisionOk = dr.valid && !dr.signalTraces.isEmpty()
                             && !dr.signalTraces.value(0).conditions.isEmpty()
                             && dr.outcomes.value(0).known && roundTrip
                             && flowSaved && rowsOut.size() == 1 && reason.isEmpty()
                             && failSaved && !failReason.isEmpty();
        line(decisionOk ? QStringLiteral("[PASS] 决策复盘可生成、可核对、可落库回看；板块失败原因也留痕")
                        : QStringLiteral("[FAIL] 决策复盘留档异常"));
        assistantOk = assistantOk && decisionOk;
    }

    section(QStringLiteral("自检总结"));
    line(QStringLiteral("总耗时        : %1 ms").arg(timer.elapsed()));
    line(QStringLiteral("逐项断言      : 详见上文 [PASS] / [FAIL] 明细"));
    line(QStringLiteral("失败断言数    : %1").arg(g_failCount));
    line(QStringLiteral("情绪引擎      : %1（复合 %2 / %3 / 上限 %4%）")
             .arg(chainOk ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
             .arg(sent.composite, 0, 'f', 1)
             .arg(sentimentTierName(sent.tier))
             .arg(sent.positionCapPct, 0, 'f', 0));
    line(QStringLiteral("复盘助手      : %1（市场复盘/预判/偏差归因/规则迭代 + 决策复盘/板块留档闭环）")
             .arg(assistantOk ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
    line();
    line(QStringLiteral("注意：以上为链路自检结果，策略阈值有效性需结合实盘复盘与更大样本统计检验。"));
    out() << (g_failCount == 0 ? "[PASS] 全链路自检通过\n"
                               : "[FAIL] 自检存在失败项（见上文）\n");
    out().flush();

    // 先关闭连接，否则 Windows 下文件仍被占用，删除会失败留下临时库
    db.close();
    QFile::remove(dbPath);
    QFile::remove(dbPath + QStringLiteral("-wal"));
    QFile::remove(dbPath + QStringLiteral("-shm"));
    return g_failCount == 0 ? 0 : 1;   // 真实退出码：CI 据此判定成败
}

// ---------------------------------------------------------------- 邮件预览
// 不发信，只验证「排版层」：主题 / HTML / 各字段是否正确拼进去。
int runMailPreview()
{
    RecommendBundle b;
    b.tradeDate = QDate::currentDate();
    b.positionCapPct = 42.0;
    b.notice = QStringLiteral("复合情绪 68.9 · 有效总仓 ≤42% ｜ 闸门放行，"
                              "共 2 只标的完成分析｜信号层：1 只可执行，合计仓位 9.7%");

    TradePlan p;
    p.code = QStringLiteral("001219");
    p.name = QStringLiteral("青岛食品");
    p.tag = QStringLiteral("5 板");
    p.buyLow = 19.34; p.buyHigh = 19.94;
    p.takeProfitLow = 20.34; p.takeProfitHigh = 21.93;
    p.stopLossLow = 18.66; p.stopLossHigh = 18.94;
    p.riskReward = 1.35; p.positionPct = 9.7; p.signalLevel = 3;
    p.note = QStringLiteral("评分 61.2 · 触发 19.34~19.94");
    p.actionable = true;
    b.plans << p;

    TradePlan w;
    w.code = QStringLiteral("002172");
    w.name = QStringLiteral("澳洋健康");
    w.tag = QStringLiteral("3 板");
    w.buyLow = 5.73; w.buyHigh = 5.91;
    w.takeProfitLow = 6.03; w.takeProfitHigh = 6.50;
    w.stopLossLow = 5.53; w.stopLossHigh = 5.61;
    w.riskReward = 0.84; w.positionPct = 0.0; w.signalLevel = 1;
    w.note = QStringLiteral("观察：未过·评分门：评分 42.8 ≥ 55");
    w.actionable = false;
    b.plans << w;

    SentimentState s;
    s.date = QDate::currentDate();
    s.composite = 68.9;
    s.phase = MarketPhase::MainUp;
    s.tier = SentimentTier::Build;
    s.positionCapPct = 42.0;
    s.tradeAllowed = true;
    s.note = sentimentStatusLabel(s.composite, s.positionCapPct, s.tradeAllowed);
    const struct { const char* k; const char* n; double v; double w; const char* d; } dims[] = {
        { "S1", "赚钱效应", 100.0, 0.30, "溢价 11.08% · 连板晋级 100.0% · 首板晋级 100.0%" },
        { "S2", "封板质量",  67.6, 0.20, "炸板率 39.7% · 封单/成交 34.9% · 10:30 前封板 72%" },
        { "S3", "空间高度",  55.7, 0.15, "最高 5 板 · 连板 9 家 · ≥3 板 3 家" },
        { "S4", "市场宽度",  17.0, 0.15, "上涨占比 15.9% · 中位数 -3.12%" },
        { "S5", "分歧收敛",  75.9, 0.10, "平均开板 1.81 次 · 平均换手 8.7%" },
        { "S6", "主线集中度", 39.2, 0.10, "最强题材 5 家 · 集中度 11%" },
    };
    for (const auto& d : dims) {
        SentimentSubScore ss;
        ss.key = QString::fromUtf8(d.k);
        ss.name = QString::fromUtf8(d.n);
        ss.score = d.v;
        ss.weight = d.w;
        ss.detail = QString::fromUtf8(d.d);
        s.subScores << ss;
    }

    const MailMessage m = PushService::compose(NotifyConfig(), b, s);
    out() << "主题      : " << m.subject << "\n";
    out() << "HTML 长度 : " << m.html.size() << " 字符\n";
    out() << "结构自检  : ";
    const bool ok = m.html.contains(QStringLiteral("青岛食品"))
                 && m.html.contains(QStringLiteral("澳洋健康"))
                 && m.html.contains(QStringLiteral("19.34"))
                 && m.html.contains(QStringLiteral("情绪六维"))
                 && m.html.contains(QStringLiteral("不构成投资建议"))
                 && !m.subject.isEmpty();
    out() << (ok ? "[PASS] 关键字段齐全" : "[FAIL] 排版缺字段") << "\n";
    out() << "---------------- HTML ----------------\n" << m.html << "\n";
    out().flush();
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------- SMTP 探活
// 只做 TCP/TLS 握手 + EHLO，不认证、不发信：用于确认「网络与端口」这一段没问题。
int runSmtpProbe()
{
    const NotifyConfig cfg = NotifyConfig::load();
    out() << "配置来源  : " << NotifyConfig::defaultPath() << "\n";
    out() << "服务器    : " << cfg.host << ":" << cfg.port
          << (cfg.implicitSsl ? " (SSL/TLS)" : " (STARTTLS)") << "\n";
    out() << "发件邮箱  : " << (cfg.username.isEmpty() ? QStringLiteral("未配置")
                                                       : cfg.username) << "\n";
    out() << "授权码    : " << (cfg.password.isEmpty() ? QStringLiteral("未配置")
                                                      : QStringLiteral("已配置（不回显）")) << "\n";
    out() << "---------------- 协议日志 ----------------\n";
    out().flush();

    SmtpClient client;
    QEventLoop loop;
    bool ok = false;
    QString detail;

    QObject::connect(&client, &SmtpClient::transcript, &client, [](const QString& l) {
        out() << "  " << l << "\n";
        out().flush();
    });
    QObject::connect(&client, &SmtpClient::finished, &loop,
                     [&](bool good, const QString& d) { ok = good; detail = d; loop.quit(); });

    client.probe(cfg);
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();

    out() << (ok ? "[PASS] " : "[FAIL] ") << detail << "\n";
    out().flush();
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------- 真实发信
// 用本地 notify_config.json 的账号发一封测试邮件，验证「认证 + 投递」完整链路。
int runMailTest()
{
    const NotifyConfig cfg = NotifyConfig::load();
    QString why;
    if (!cfg.ready(&why)) {
        out() << "[FAIL] 配置不可用：" << why << "\n";
        out() << "       配置文件：" << NotifyConfig::defaultPath() << "\n";
        out().flush();
        return 2;
    }

    out() << "发件邮箱  : " << cfg.username << "\n";
    out() << "收件人    : " << cfg.effectiveRecipients().join(QStringLiteral(", ")) << "\n";
    out() << "服务器    : " << cfg.host << ":" << cfg.port
          << (cfg.implicitSsl ? " (SSL/TLS)" : " (STARTTLS)") << "\n";
    out() << "---------------- 协议日志 ----------------\n";
    out().flush();

    MailMessage msg;
    msg.subject = cfg.subjectPrefix + QStringLiteral(" 邮件通道测试");
    msg.html = QStringLiteral(
                   "<div style=\"font-family:'Microsoft YaHei',Arial,sans-serif;color:#1b2430\">"
                   "<h2 style=\"margin:0 0 8px 0\">做多量化选股系统 · 通道测试</h2>"
                   "<p>收到这封邮件，说明 SMTP 推送链路已经打通："
                   "分析结果会在有可执行信号时自动送达。</p>"
                   "<p style=\"color:#8a93a1;font-size:12px\">发送时间：%1<br/>"
                   "发件账号：%2</p></div>")
                   .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                        cfg.username);

    SmtpClient client;
    QEventLoop loop;
    bool ok = false;
    QString detail;

    QObject::connect(&client, &SmtpClient::transcript, &client, [](const QString& l) {
        out() << "  " << l << "\n";
        out().flush();
    });
    QObject::connect(&client, &SmtpClient::finished, &loop,
                     [&](bool good, const QString& d) { ok = good; detail = d; loop.quit(); });

    client.send(cfg, msg);
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();

    out() << (ok ? "[PASS] " : "[FAIL] ") << detail << "\n";
    out().flush();
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------- 补推最新结果
// 读取 GUI 落库的最新荐股 + 当日情绪，按正式排版推送一封（运维/补发用）。
int runPushLatest()
{
    Database db;
    const QString dbPath = QCoreApplication::applicationDirPath()
                         + QStringLiteral("/daban_quant.db");
    if (!db.open(dbPath)) {
        out() << "[FAIL] 数据库打不开：" << dbPath << "（" << db.lastError() << "）\n";
        return 2;
    }
    const QDate d = db.latestRecommendDate();
    if (!d.isValid()) {
        out() << "[FAIL] 库里还没有荐股结果，请先在客户端跑一次分析\n";
        return 2;
    }
    const RecommendBundle bundle = db.loadRecommend(d);
    const SentimentState state = db.loadSentiment(d);
    out() << "交易日    : " << d.toString(Qt::ISODate) << "\n";
    out() << "标的数    : " << bundle.plans.size() << "\n";
    out() << "情绪      : 复合 " << state.composite << " / " << sentimentTierName(state.tier)
          << " / 上限 " << state.positionCapPct << "%\n";
    out().flush();

    PushService svc;
    svc.setDatabase(&db);
    svc.loadConfig();

    QEventLoop loop;
    bool ok = false;
    QString detail;
    QObject::connect(&svc, &PushService::log, &svc, [](const QString& l) {
        out() << "  " << l << "\n";
        out().flush();
    });
    QObject::connect(&svc, &PushService::finished, &loop,
                     [&](bool good, const QString& info) { ok = good; detail = info; loop.quit(); });

    svc.pushBundle(bundle, state, true);   // manual=true：允许补发观察名单
    QTimer::singleShot(40000, &loop, &QEventLoop::quit);
    loop.exec();

    out() << (ok ? "[PASS] " : "[FAIL] ") << detail << "\n";
    out().flush();
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------- 离线回测
int runBacktest(int days)
{
    BacktestConfig cfg;
    cfg.days = qMax(3, days);
    cfg.endDate = QDate::currentDate();
    cfg.csvPath = QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("backtest_trades.csv"));
    // 与 GUI 读取同一份持久化配置，保证回测与实盘选股口径一致
    Database cfgDb;
    if (cfgDb.open()) {
        cfg.factorConfig = FactorConfig::fromJsonText(
            cfgDb.getSetting(QStringLiteral("factor.config")));
        cfgDb.close();
    }
    out() << "因子配置    : " << cfg.factorConfig.summary() << "\n";

    BacktestRunner runner;
    BacktestResult res;
    QObject::connect(&runner, &BacktestRunner::progress, &runner, [](const QString& t) {
        out() << "  · " << t << "\n";
        out().flush();
    });
    QObject::connect(&runner, &BacktestRunner::finished, &runner,
                     [&res](const BacktestResult& r) { res = r; });

    runner.run(cfg);

    out() << "\n========== 回测结果 ==========\n";
    out() << "交易日    : " << res.days << "\n";
    out() << "信号数    : " << res.signalCount << "（成交 " << res.filled << "，成交率 "
          << (res.signalCount > 0 ? res.filled * 100.0 / res.signalCount : 0.0) << "%）\n";
    out() << "胜率      : " << res.winRate << "%\n";
    out() << "每笔均值  : " << res.avgPnl << "%\n";
    out() << "每信号期望: " << res.expectancy << "%（未成交计 0，实盘口径）\n";
    out() << "累计收益  : " << res.totalPnlPts << " 点（等权，未复利）\n";
    out() << "最大回撤  : " << res.maxDrawdown << " 点\n";

    const QStringList tiers = { QStringLiteral("试仓"), QStringLiteral("建仓"),
                                QStringLiteral("主攻") };
    for (const QString& tier : tiers) {
        const QVector<double> v = res.byTier.value(tier);
        if (v.isEmpty()) continue;
        double sum = 0.0;
        int win = 0;
        for (double x : v) { sum += x; if (x > 0) ++win; }
        out() << QStringLiteral("  %1: %2 笔 · 胜率 %3% · 均值 %4%\n")
                     .arg(tier).arg(v.size())
                     .arg(win * 100.0 / v.size(), 0, 'f', 1)
                     .arg(sum / v.size(), 0, 'f', 2);
    }

    if (!res.blockerStats.isEmpty()) {
        QList<QPair<QString, int>> list;
        for (auto it = res.blockerStats.constBegin(); it != res.blockerStats.constEnd(); ++it)
            list.append(qMakePair(it.key(), it.value()));
        std::sort(list.begin(), list.end(),
                  [](const QPair<QString, int>& a, const QPair<QString, int>& b) {
                      return a.second > b.second;
                  });
        out() << "\n未过门槛统计（用于校准阈值）：\n";
        for (int i = 0; i < qMin(6, list.size()); ++i) {
            out() << QStringLiteral("  %1: %2 次\n").arg(list[i].first).arg(list[i].second);
        }
    }

    out() << "\n口径说明（务必一起看）：\n";
    for (const QString& n : res.notes) out() << "  - " << n << "\n";
    out() << "逐笔明细  : " << cfg.csvPath << "\n";
    out().flush();
    return 0;
}

// ---------------------------------------------------------------- 助手运维入口
// 供 Windows 计划任务 / 服务器 cron 使用：不开 GUI 也能出复盘与简报。
int runAssistantReview()
{
    Database db;
    if (!db.open(QCoreApplication::applicationDirPath() + QStringLiteral("/daban_quant.db"))) {
        out() << "[FAIL] 数据库打不开\n";
        return 2;
    }
    const QDate d = db.latestRecommendDate();
    if (!d.isValid()) {
        out() << "[FAIL] 库里还没有荐股结果，请先跑一次分析\n";
        return 2;
    }
    const RecommendBundle bundle = db.loadRecommend(d);
    const SentimentState st = db.loadSentiment(d);
    if (!st.valid()) {
        out() << "[FAIL] 缺少 " << d.toString(Qt::ISODate) << " 的情绪数据\n";
        return 2;
    }

    ReviewInput in;
    in.date = d;
    in.sentiment = st;
    in.bundle = bundle;

    const ReviewReport r = ReviewEngine::buildReview(in);
    db.saveReview(r);

    out() << "复盘日期  : " << d.toString(Qt::ISODate) << "\n";
    out() << "结论      : " << r.headline << "\n";
    out() << "计划数    : " << bundle.plans.size() << "\n";
    out() << "HTML 长度 : " << r.html.size() << "\n";
    out() << "[PASS] 复盘已生成并落库\n";
    out().flush();
    return 0;
}

int runAssistantBrief(bool send)
{
    Database db;
    if (!db.open(QCoreApplication::applicationDirPath() + QStringLiteral("/daban_quant.db"))) {
        out() << "[FAIL] 数据库打不开\n";
        return 2;
    }
    const QDate y = db.latestReviewDate();
    if (!y.isValid()) {
        out() << "[FAIL] 没有复盘记录，无法生成预判\n";
        return 2;
    }
    const ReviewReport rep = db.loadReview(y);
    QVector<AssistantRule> rules = db.loadRules();
    if (rules.isEmpty()) rules = ReviewEngine::defaultRules();
    const double bias = db.getSetting(QStringLiteral("assistant.bias"), QStringLiteral("0")).toDouble();

    Forecast f = ReviewEngine::buildForecast(rep, rules, bias);
    if (!f.valid) {
        out() << "[FAIL] 预判生成失败\n";
        return 2;
    }
    QDate target = QDate::currentDate();
    while (target.dayOfWeek() > Qt::Friday) target = target.addDays(1);
    f.forDate = target;
    db.saveForecast(f);

    out() << "复盘依据  : " << y.toString(Qt::ISODate) << "\n";
    out() << "预判目标  : " << target.toString(Qt::ISODate) << "\n";
    out() << "立场      : " << f.stance << "\n";
    out() << "涨停区间  : " << f.limitUpLow << "~" << f.limitUpHigh
          << "（中心 " << f.limitUpCenter << "）\n";
    out() << "高度区间  : " << f.maxBoardLow << "~" << f.maxBoardHigh << " 板\n";
    out() << "置信度    : " << f.confidence * 100 << "%\n";

    if (!send) {
        out() << "[PASS] 简报已生成并落库（未发送）\n";
        return 0;
    }

    PushService svc;
    svc.setDatabase(&db);
    svc.loadConfig();
    QEventLoop loop;
    bool ok = false;
    QString detail;
    QObject::connect(&svc, &PushService::log, &svc, [](const QString& l) {
        out() << "  " << l << "\n";
        out().flush();
    });
    QObject::connect(&svc, &PushService::finished, &loop,
                     [&](bool good, const QString& d) { ok = good; detail = d; loop.quit(); });
    svc.sendCustom(QStringLiteral("早盘简报 %1 ｜ %2")
                       .arg(target.toString(QStringLiteral("MM-dd")), f.stance), f.html);
    QTimer::singleShot(40000, &loop, &QEventLoop::quit);
    loop.exec();
    out() << (ok ? "[PASS] " : "[FAIL] ") << detail << "\n";
    out().flush();
    return ok ? 0 : 1;
}

int main(int argc, char* argv[])
{
    // 自检模式用 QCoreApplication（不依赖 GUI 平台插件），
    // 便于在无桌面环境的服务器/CI 上跑通全链路。
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("DabanQuant"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("DabanQuant"));
    Diagnostics::install(false);   // 自检也落文件日志，但不接管崩溃过滤器
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("做多量化选股系统 · 自检"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption selfTestOpt(QStringLiteral("selftest"),
                                   QStringLiteral("无头自检：验证数据/策略/存储整链路"));
    parser.addOption(selfTestOpt);
    QCommandLineOption previewOpt(QStringLiteral("mail-preview"),
                                  QStringLiteral("打印邮件排版预览（不发信）"));
    parser.addOption(previewOpt);
    QCommandLineOption probeOpt(QStringLiteral("smtp-probe"),
                                QStringLiteral("SMTP 连通性探活（不认证、不发信）"));
    parser.addOption(probeOpt);
    QCommandLineOption mailTestOpt(QStringLiteral("mail-test"),
                                   QStringLiteral("按 notify_config.json 真实发送一封测试邮件"));
    parser.addOption(mailTestOpt);
    QCommandLineOption pushLatestOpt(QStringLiteral("push-latest"),
                                     QStringLiteral("把库里最新的荐股结果按正式排版推送出去"));
    parser.addOption(pushLatestOpt);
    QCommandLineOption backtestOpt(QStringLiteral("backtest"),
                                   QStringLiteral("离线回测最近 N 个交易日（默认 20）"),
                                   QStringLiteral("days"), QStringLiteral("20"));
    parser.addOption(backtestOpt);
    QCommandLineOption reviewOpt(QStringLiteral("assistant-review"),
                                 QStringLiteral("用库里最新结果生成收盘复盘（无头，可用于计划任务）"));
    parser.addOption(reviewOpt);
    QCommandLineOption briefOpt(QStringLiteral("assistant-brief"),
                                QStringLiteral("基于最近复盘生成早盘简报（无头）"));
    parser.addOption(briefOpt);
    QCommandLineOption sendOpt(QStringLiteral("send"),
                               QStringLiteral("与 --assistant-brief 搭配：生成后发送邮件"));
    parser.addOption(sendOpt);
    QCommandLineOption dateOpt(QStringList{QStringLiteral("d"), QStringLiteral("date")},
                               QStringLiteral("指定交易日 yyyy-MM-dd"), QStringLiteral("date"));
    parser.addOption(dateOpt);
    parser.process(app);
    Q_UNUSED(selfTestOpt);

    if (parser.isSet(previewOpt)) return runMailPreview();
    if (parser.isSet(probeOpt)) return runSmtpProbe();
    if (parser.isSet(mailTestOpt)) return runMailTest();
    if (parser.isSet(pushLatestOpt)) return runPushLatest();
    if (parser.isSet(backtestOpt)) {
        return runBacktest(parser.value(backtestOpt).toInt());
    }
    if (parser.isSet(reviewOpt)) return runAssistantReview();
    if (parser.isSet(briefOpt)) return runAssistantBrief(parser.isSet(sendOpt));

    const QStringList dates = parser.values(dateOpt);
    if (!dates.isEmpty()) {
        g_requestDate = QDate::fromString(dates.first(), QStringLiteral("yyyy-MM-dd"));
    }
    return runSelfTest(app);
}
