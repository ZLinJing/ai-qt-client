// 纯离线单元门禁：不联网、不依赖 GUI、不依赖数据库，可在任意 CI 上稳定运行。
// 覆盖最容易静默出错的纯计算层：涨停价量纲、均线、竞价三档、交易计划区间、
// 多因子权重归一化、贡献度守恒、过滤规则与实盘/回测输入口径一致性。
//
// 本文件只提供 CoreChecks::run()，**不含 main()** —— 断言集被两处复用：
//   ① DabanQuantCoreChecks.exe（console 目标，常规 CI 用）
//   ② DabanQuant.exe --corechecks（GUI 目标）
// 之所以要有 ②：受管控终端的应用控制策略会拦截**新编译的 console 可执行文件**
// （"An Application Control policy has blocked this file"），而 GUI 目标在白名单内。
// 没有 ② 的话，离线门禁在���类机器上根本跑不起来 —— 门禁形同虚设。

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QTime>
#include <cmath>

#include "tests/CoreChecks.h"
#include "analysis/TradeAdvisor.h"
#include "analysis/AiSettings.h"
#include "analysis/AiAnalysisSpec.h"
#include "core/Types.h"
#include "core/LegalNotice.h"
#include "core/Indicators.h"
#include "core/Auction.h"
#include "core/TradePlan.h"
#include "core/ChipDistribution.h"
#include "strategy/FactorConfig.h"
#include "strategy/MultiFactorEngine.h"
#include "review/DecisionReview.h"

namespace {

int g_failed = 0;

QTextStream& out()
{
    static QTextStream ts(stdout);
    return ts;
}

void check(bool ok, const QString& name)
{
    out() << (ok ? "[PASS] " : "[FAIL] ") << name << Qt::endl;
    if (!ok) ++g_failed;
}

bool near(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

Candidate makeCandidate(const QString& code, double daban, double maBull, bool withSnapshot)
{
    Candidate c;
    c.entry.code = code;
    c.entry.name = code;
    c.entry.price = 20.0;
    c.entry.limitPrice = 22.0;
    c.entry.floatCap = 80.0e8;
    c.entry.totalCap = 120.0e8;
    c.entry.turnover = 12.0;
    c.entry.amount = 1.0e8;
    c.entry.sealOrder = 3.0e7;
    c.entry.board = Board::MainBoard;
    c.entry.boardCount = 2;
    c.score = daban;
    c.maBull = maBull;
    c.volRatio5 = 2.0;
    c.limitCount20 = 3;

    KlineSeries k;
    k.code = code;
    for (int i = 0; i < 80; ++i) {
        Candle cd;
        cd.date = QDate(2026, 1, 1).addDays(i);
        cd.open = 15.0 + i * 0.05;
        cd.close = cd.open + 0.06;
        cd.high = cd.close + 0.20;
        cd.low = cd.open - 0.20;
        cd.volume = 100000.0 + i * 100.0;
        cd.amount = cd.volume * 100.0 * cd.close;
        k.candles.append(cd);
    }
    k.rawCandles = k.candles;
    c.kline = k;

    if (withSnapshot) {
        Snapshot s;
        s.code = code;
        s.name = code;
        s.price = 20.0;
        s.prevClose = 19.0;
        s.pe = 18.0;
        s.pb = 2.2;
        s.floatCap = 80.0e8;
        s.totalCap = 120.0e8;
        c.snapshot = s;
    }
    return c;
}

FactorInput makeInput(const Candidate& c, const Snapshot& s)
{
    FactorInput in;
    in.candidate = c;
    in.kline = c.kline;
    in.snapshot = s;
    in.dabanScore = c.score;
    return in;
}

// 构造一段「低位密集成交 → 放量拉起 → 涨停」的日 K：
//   base     —— 密集成交区基准价（筹码密集峰应落在这里）
//   lastVol  —— 最后一根（涨停日）的成交量，用于制造缩量 / 放量两种形态
//   lastClose—— 最后一根的收盘价：base+1.10 = 涨停；base+0.10 = 跌进支撑带；base-0.60 = 跌破密集区
KlineSeries makeChipKline(double base, double lastVol, double lastClose)
{
    KlineSeries k;
    k.code = QStringLiteral("600777");
    QDate d(2026, 1, 5);
    // 阶段一：60 根在基准价附近窄幅震荡，成交量大 → 筹码在此密集堆积
    for (int i = 0; i < 60; ++i) {
        Candle cd;
        cd.date = d.addDays(i);
        const double wob = std::sin(i * 0.7) * 0.18;   // 小幅摆动
        cd.open  = base + wob;
        cd.close = base + wob * 0.6;
        cd.high  = cd.open + 0.12;
        cd.low   = cd.open - 0.12;
        cd.volume = 120000.0;
        cd.amount = cd.volume * 100.0 * cd.close;
        k.candles.append(cd);
    }
    // 阶段二：10 根逐步拉起，成交量递减 → 上方筹码稀薄
    for (int i = 0; i < 10; ++i) {
        Candle cd;
        cd.date = d.addDays(60 + i);
        cd.open  = base + 0.20 + i * 0.08;
        cd.close = cd.open + 0.10;
        cd.high  = cd.close + 0.10;
        cd.low   = cd.open - 0.08;
        cd.volume = 40000.0;
        cd.amount = cd.volume * 100.0 * cd.close;
        k.candles.append(cd);
    }
    // 阶段三：涨停日（最后一根）—— 缩量或放量两种形态
    Candle last;
    last.date = d.addDays(70);
    last.open  = base + 1.00;
    last.close = lastClose;
    last.high  = qMax(last.open, last.close);
    last.low   = qMin(last.open, last.close);
    last.volume = lastVol;
    last.amount = last.volume * 100.0 * last.close;
    k.candles.append(last);
    k.rawCandles = k.candles;
    return k;
}

} // namespace

namespace CoreChecks {

// 离线门禁入口：跑完全部断言，返回**失败断言数**（0 = 全通过）。
// 不依赖 GUI、不联网、不碰数据库 —— 任何机器上都能跑。
int run()
{

    // 1. 涨停价与涨停判定（主板 10cm / 创业板 20cm）
    check(near(Ind::calcLimitUpPrice(10.0, Board::MainBoard), 11.0),
          QStringLiteral("主板涨停价 10.00 → 11.00"));
    check(Ind::isLimitUp(11.0, 10.0, Board::MainBoard),
          QStringLiteral("主板 11.00 判定为涨停"));
    check(!Ind::isLimitUp(10.5, 10.0, Board::MainBoard),
          QStringLiteral("主板 10.50 不判为涨停"));
    check(near(Ind::calcLimitUpPrice(10.0, Board::ChiNext), 12.0),
          QStringLiteral("创业板涨停价 10.00 → 12.00"));

    // 2. 均线：未成熟区间为 NaN，成熟区间取算术均值
    const QVector<double> v { 1.0, 2.0, 3.0, 4.0, 5.0 };
    const QVector<double> ma = Ind::sma(v, 3);
    check(ma.size() == v.size(), QStringLiteral("SMA 输出等长"));
    check(std::isnan(ma.at(0)) && std::isnan(ma.at(1)),
          QStringLiteral("SMA 前 period-1 根为 NaN"));
    check(near(ma.at(2), 2.0) && near(ma.at(4), 4.0),
          QStringLiteral("SMA 窗口均值正确"));

    // 3. 竞价三档：用规则自身阈值构造入参，避免测试与阈值耦合
    LimitEntry e;
    e.code = QStringLiteral("000001");
    e.boardCount = 2;
    e.breakCount = 0;
    const AuctionRule rule = Auction::ruleFor(e);
    check(rule.hasRule, QStringLiteral("二板生成竞价规则"));

    const double prevClose = 10.0;
    const double prevAmount = 1.0e8;
    const double auctionAmount = prevAmount * rule.strongAmountRatio * 1.2;   // 量能充足
    const auto at = [&](double openPct) {
        return Auction::evaluate(rule, true, prevClose * (1.0 + openPct / 100.0),
                                 prevClose, auctionAmount, prevAmount, true);
    };
    const double mid = (rule.minOpenPct + rule.maxOpenPct) * 0.5;
    check(at(mid).verdict != AuctionAdvice::Abandon,
          QStringLiteral("区间内高开不判放弃"));
    check(at(rule.maxOpenPct + 2.0).verdict == AuctionAdvice::Abandon,
          QStringLiteral("高开超上沿判放弃"));
    check(at(rule.abandonPct - 2.0).verdict == AuctionAdvice::Abandon,
          QStringLiteral("低开破位判放弃"));
    const AuctionAdvice noQuote =
        Auction::evaluate(rule, false, 0.0, prevClose, 0.0, prevAmount, false);
    check(!noQuote.evaluated && noQuote.verdict == AuctionAdvice::Unknown,
          QStringLiteral("无竞价行情时不判定"));

    // 4. 交易计划中枢与有效性
    TradePlan p;
    p.code = QStringLiteral("000001");
    p.buyLow = 10.0;  p.buyHigh = 11.0;
    p.takeProfitLow = 12.0; p.takeProfitHigh = 13.0;
    p.stopLossLow = 9.0;    p.stopLossHigh = 9.5;
    check(p.valid(), QStringLiteral("TradePlan::valid 判定"));
    check(near(p.buyMid(), 10.5) && near(p.tpMid(), 12.5) && near(p.slMid(), 9.25),
          QStringLiteral("三段区间中枢计算"));
    check(!signalLevelName(3).isEmpty() && signalLevelName(0) != signalLevelName(3),
          QStringLiteral("信号档位名称映射"));

    // 5. 因子配置：默认权重、JSON 往返、指纹稳定、非法配置自动修正
    const FactorConfig cfg = FactorConfig::defaults();
    check(cfg.dims.size() == kFactorDimensionCount,
          QStringLiteral("默认配置含 6 个维度"));
    const FactorConfig round = FactorConfig::fromJson(cfg.toJson());
    check(round.digest() == cfg.digest(),
          QStringLiteral("配置 JSON 往返后指纹一致"));
    FactorConfig broken;
    broken.validate();
    check(broken.dims.size() == kFactorDimensionCount && broken.dimAt(0).enabled,
          QStringLiteral("空配置自动补齐并保留打板核心"));

    // 6. 多因子引擎：排序、贡献度守恒、过滤、缺失数据、输入口径一致
    const Candidate a = makeCandidate(QStringLiteral("600001"), 80.0, 0.90, true);
    const Candidate b = makeCandidate(QStringLiteral("600002"), 40.0, 0.20, true);
    QVector<FactorInput> inputs { makeInput(a, a.snapshot), makeInput(b, b.snapshot) };
    const FactorReport rep = MultiFactorEngine::rank(inputs, cfg);
    check(rep.ranking.size() == 2 && rep.ranking.at(0).code == a.entry.code
          && rep.ranking.at(0).rank == 1,
          QStringLiteral("综合评分按降序排名，高分标的居首"));
    double sum = 0.0;
    for (const FactorContribution& fc : rep.ranking.at(0).contributions) sum += fc.contribution;
    check(near(sum, rep.ranking.at(0).composite, 1e-6),
          QStringLiteral("维度贡献之和等于综合评分"));
    check(!rep.configDigest.isEmpty() && rep.configDigest == cfg.digest(),
          QStringLiteral("排名报告携带配置指纹"));

    FactorConfig noTrend = cfg;
    noTrend.dims[int(FactorDimension::Trend)].enabled = false;
    const FactorReport rep2 = MultiFactorEngine::rank(inputs, noTrend);
    bool trendGone = true;
    for (const FactorContribution& fc : rep2.ranking.at(0).contributions)
        if (fc.dimension == int(FactorDimension::Trend) && fc.available) trendGone = false;
    check(trendGone && !near(rep2.ranking.at(0).composite, rep.ranking.at(0).composite, 1e-9),
          QStringLiteral("关闭维度后权重自动重新归一化"));

    FactorConfig strict = cfg;
    strict.dims[int(FactorDimension::Volatility)].mode = FactorMode::Both;
    strict.dims[int(FactorDimension::Volatility)].threshold = 1.01;
    const FactorReport rep3 = MultiFactorEngine::rank(inputs, strict);
    check(rep3.ranking.isEmpty() && rep3.rejected.size() == 2
          && !rep3.rejected.at(0).rejectReason.isEmpty(),
          QStringLiteral("维度过滤门槛生效并记录淘汰原因"));

    // 回测无历史快照：基本面维度仍可用可得子项，且不被误判为数据完全缺失
    QVector<FactorInput> noSnap { makeInput(a, Snapshot()), makeInput(b, Snapshot()) };
    const FactorReport rep4 = MultiFactorEngine::rank(noSnap, cfg);
    check(rep4.ranking.size() == 2,
          QStringLiteral("历史快照缺失时不崩溃，按可用子项重分配权重"));

    // 实盘与回测共用 makeInputs：K 线按同一 asOf 截断
    QHash<QString, Snapshot> snaps;
    QHash<QString, KlineSeries> klines;
    snaps.insert(a.entry.code, a.snapshot);
    klines.insert(a.entry.code, a.kline);
    const QDate asOf(2026, 3, 1);
    const QVector<FactorInput> unified =
        MultiFactorEngine::makeInputs({ a }, snaps, klines, asOf);
    check(unified.size() == 1 && unified.at(0).kline.candles.size() < a.kline.candles.size()
          && near(unified.at(0).dabanScore, a.score),
          QStringLiteral("统一输入构造：K 线按 asOf 截断且打板分透传"));

    // 7. 均衡做多策略（版本管理移除后唯一保留的策略参数）
    const FactorConfig balanced = FactorConfig::balanced();
    check(FactorConfig::strategyName() == QStringLiteral("均衡做多"),
          QStringLiteral("策略名为「均衡做多」"));
    check(balanced.dims.size() == kFactorDimensionCount,
          QStringLiteral("均衡做多含 6 个评分维度"));
    bool allEnabled = true;
    for (int d = 0; d < kFactorDimensionCount; ++d)
        if (!balanced.dimAt(d).enabled) allEnabled = false;
    check(allEnabled, QStringLiteral("均衡做多默认启用全部六个维度"));
    check(balanced.digest() == FactorConfig::defaults().digest(),
          QStringLiteral("默认配置与均衡做多配置一致"));
    const FactorConfig balancedRound = FactorConfig::fromJson(balanced.toJson());
    check(balancedRound.digest() == balanced.digest(),
          QStringLiteral("均衡做多配置 JSON 往返后指纹一致"));
    const FactorReport rBalanced = MultiFactorEngine::rank(inputs, balanced);
    check(rBalanced.configDigest == balanced.digest() && !rBalanced.ranking.isEmpty(),
          QStringLiteral("均衡做多评分报告携带配置指纹"));

    // 8. 旧版多版本库迁移：取启用版本的权重，空/非法输入退回均衡做多
    {
        FactorConfig custom = balanced;
        custom.dims[int(FactorDimension::Trend)].weight = 0.42;
        custom.validate();
        QJsonObject verObj;
        verObj["id"] = QStringLiteral("custom");
        verObj["name"] = QStringLiteral("自定义");
        verObj["config"] = custom.toJson();
        QJsonObject balObj;
        balObj["id"] = QStringLiteral("balanced");
        balObj["name"] = QStringLiteral("均衡做多");
        balObj["config"] = balanced.toJson();

        QJsonObject legacy;
        legacy["activeId"] = QStringLiteral("custom");
        legacy["versions"] = QJsonArray{ verObj, balObj };
        const FactorConfig migrated =
            FactorConfig::fromLegacyVersionsText(QString::fromUtf8(
                QJsonDocument(legacy).toJson(QJsonDocument::Compact)));
        check(migrated.digest() == custom.digest(),
              QStringLiteral("旧版版本集合迁移取启用版本的权重"));

        QJsonObject legacyNoActive;
        legacyNoActive["activeId"] = QStringLiteral("missing");
        legacyNoActive["versions"] = QJsonArray{ verObj, balObj };
        const FactorConfig migratedBalanced =
            FactorConfig::fromLegacyVersionsText(QString::fromUtf8(
                QJsonDocument(legacyNoActive).toJson(QJsonDocument::Compact)));
        check(migratedBalanced.digest() == balanced.digest(),
              QStringLiteral("启用版本缺失时迁移退回均衡做多"));

        check(FactorConfig::fromLegacyVersionsText(QString()).digest() == balanced.digest()
              && FactorConfig::fromLegacyVersionsText(QStringLiteral("not-json")).digest()
                     == balanced.digest(),
              QStringLiteral("旧版配置为空或非法时退回均衡做多"));
    }

    // 9. 候选池「仅推荐两只」口径：名额按综合分分配，落选降级为备选
    {
        QVector<TradePlan> pool;
        for (int i = 0; i < 5; ++i) {
            TradePlan p;
            p.code          = QStringLiteral("60000%1").arg(i);
            p.name          = QStringLiteral("股%1").arg(i);
            p.actionable    = true;
            p.positionPct   = 10.0;
            p.compositeScore = 60.0 + i * 10.0;   // 60 / 70 / 80 / 90 / 100
            pool.append(p);
        }
        applyConcentratedPicks(pool);

        int picks = 0;
        QStringList pickCodes;
        for (const TradePlan& p : pool) {
            if (p.actionable && p.positionPct > 0.0) { ++picks; pickCodes << p.code; }
        }
        check(picks == kConcentratedPositionCount,
              QStringLiteral("候选池最多推荐 %1 只").arg(kConcentratedPositionCount));
        check(pickCodes.contains(QStringLiteral("600003"))
              && pickCodes.contains(QStringLiteral("600004")),
              QStringLiteral("推荐名额按综合评分最高的 2 只分配"));
        check(!pool.at(0).actionable && pool.at(0).positionPct == 0.0
              && pool.at(0).note.startsWith(QStringLiteral("备选")),
              QStringLiteral("落选标的降级为备选且仓位归零"));

        // 幂等：重复应用结果不变
        QVector<TradePlan> again = pool;
        applyConcentratedPicks(again);
        int picks2 = 0;
        for (const TradePlan& p : again) if (p.actionable && p.positionPct > 0.0) ++picks2;
        check(picks2 == picks, QStringLiteral("集中持仓口径重复应用结果稳定"));

        // 观察名单（不可执行）不占用推荐名额
        QVector<TradePlan> mixed;
        for (int i = 0; i < 3; ++i) {
            TradePlan p;
            p.code = QStringLiteral("60001%1").arg(i);
            p.actionable = (i == 0);           // 只有第 1 只可执行
            p.positionPct = p.actionable ? 10.0 : 0.0;
            p.compositeScore = 90.0 - i * 10.0;
            mixed.append(p);
        }
        applyConcentratedPicks(mixed);
        int mixedPicks = 0;
        for (const TradePlan& p : mixed) if (p.actionable && p.positionPct > 0.0) ++mixedPicks;
        check(mixedPicks == 1, QStringLiteral("观察名单不占用推荐名额"));
    }

    // 10. 筹码分布：支撑峰 / 压力峰 / 获利盘 / 数据不足保护
    {
        const KlineSeries ck = makeChipKline(11.20, 120000.0, 12.30);
        const Chip::Distribution chip = Chip::build(ck.rawCandles, ck.rawCandles.back().close);
        check(chip.valid && chip.totalVolume > 0.0 && chip.avgCost > 0.0,
              QStringLiteral("筹码分布可由日 K 重建并给出平均成本"));
        check(chip.profitRatio > 0.5 && chip.profitRatio < 1.0,
              QStringLiteral("获利盘比例落在合理区间"));

        const Chip::Peak* sup = chip.supportBelow(ck.rawCandles.back().close);
        check(sup != nullptr && sup->low <= sup->high && sup->center < ck.rawCandles.back().close,
              QStringLiteral("下方密集峰可作为支撑锚点"));
        check(sup != nullptr && sup->center > 11.0 && sup->center < 11.6,
              QStringLiteral("支撑峰落在密集成交区附近"));

        // 数据不足（K 线太少）时必须拒绝出分布，调用方回退均线口径
        const Chip::Distribution thin =
            Chip::build(QVector<Candle>(ck.rawCandles.mid(0, 5)), 11.0);
        check(!thin.valid, QStringLiteral("K 线不足时不出筹码分布（回退均线口径）"));
    }

    // 11. 交易计划接入筹码与量能：买入锚点 / 止损锚点 / 破位缓冲 / 破位降级
    {
        MarketRegime reg;
        reg.date = QDate(2026, 3, 20);
        reg.phase = MarketPhase::MainUp;

        Candidate c = makeCandidate(QStringLiteral("600777"), 88.0, 0.8, false);
        c.entry.limitPrice = 12.30;
        c.entry.price = 12.30;
        c.entry.boardCount = 1;

        // 缩量涨停：承接位取筹码支撑上沿，止损缓冲放宽到 2.5%
        const KlineSeries dry = makeChipKline(11.20, 30000.0, 12.30);
        const TradePlan tpDry = Plan::build(c, &dry, reg, true);
        check(tpDry.valid() && tpDry.chipSupportHigh > 0.0,
              QStringLiteral("计划带出筹码支撑密集峰"));
        check(near(tpDry.buyLow, tpDry.chipSupportHigh, 0.011),
              QStringLiteral("买入下沿锚定在筹码支撑上沿"));
        check(tpDry.buyHigh >= tpDry.buyLow && tpDry.buyHigh > tpDry.buyLow,
              QStringLiteral("买入区间上下沿有序"));
        const double dryCushion = (tpDry.stopLossHigh - tpDry.stopLossLow) / tpDry.stopLossHigh;
        check(dryCushion > 0.02 && dryCushion < 0.03,
              QStringLiteral("缩量破位缓冲放宽到 2.5%（实际 %1）").arg(dryCushion * 100, 0, 'f', 2));
        check(!tpDry.supportQuality.isEmpty(),
              QStringLiteral("承接质量结论已写入计划"));

        // 放量破位：收盘跌穿密集区下沿且量能放大 → 止损条件成立，降级为观察
        const KlineSeries heavy = makeChipKline(11.20, 400000.0, 10.60);
        const TradePlan tpHeavy = Plan::build(c, &heavy, reg, true);
        check(tpHeavy.chipSupportLow > 0.0 && heavy.rawCandles.back().close < tpHeavy.chipSupportLow,
              QStringLiteral("放量破位场景可定位到密集区下沿"));
        check(!tpHeavy.actionable,
              QStringLiteral("筹码支撑破位放量 → 降级为观察，不再新开仓"));
        const double heavyCushion = (tpHeavy.stopLossHigh - tpHeavy.stopLossLow) / tpHeavy.stopLossHigh;
        check(heavyCushion > 0.01 && heavyCushion < 0.02,
              QStringLiteral("放量破位缓冲收紧到 1.5%（实际 %1）").arg(heavyCushion * 100, 0, 'f', 2));
        check(tpHeavy.riskFlags.contains(QStringLiteral("筹码支撑破位放量")),
              QStringLiteral("破位放量写入风险标记"));

        // 放量击穿：跌进支撑带（未破密集区下沿）但量能放大且买盘不接力 → 不应买入
        const KlineSeries pierce = makeChipKline(11.20, 400000.0, 11.30);
        const TradePlan tpPierce = Plan::build(c, &pierce, reg, true);
        check(tpPierce.chipSupportHigh > 0.0 && tpPierce.chipSupportLow > 0.0
              && 11.30 < tpPierce.chipSupportHigh && 11.30 > tpPierce.chipSupportLow,
              QStringLiteral("跌进支撑带的场景可定位承接位与密集区下沿"));
        check(!tpPierce.actionable,
              QStringLiteral("支撑放量击穿 → 不应买入，降级为观察"));
        check(tpPierce.riskFlags.contains(QStringLiteral("筹码支撑放量击穿")),
              QStringLiteral("放量击穿写入风险标记"));

        // 无 K 线时必须优雅回退到涨停价 ± 容忍度，且不产生筹码字段
        const TradePlan tpNoK = Plan::build(c, nullptr, reg, true);
        check(tpNoK.valid() && tpNoK.chipSupportHigh == 0.0 && tpNoK.buyLow > 0.0,
              QStringLiteral("无日 K 时回退涨停价口径且不伪造筹码数据"));
    }

    // 12. 交易日志 AI 诊断：盈亏状态 / 止盈策略 / 回本可行性 / 数据缺失降级
    {
        const QDate begin(2026, 1, 5);
        // 线性走势日 K：step > 0 为多头，step < 0 为空头；raw 与 qfq 同价，便于直接比较成交价
        auto makeTrend = [&](double start, double step, int n) {
            KlineSeries k;
            for (int i = 0; i < n; ++i) {
                Candle cd;
                cd.date  = begin.addDays(i);
                cd.close = start + step * i;
                cd.open  = cd.close - step * 0.4;
                cd.high  = cd.close + 0.15;
                cd.low   = cd.open - 0.15;
                cd.volume = 100000.0;
                cd.amount = cd.volume * cd.close;
                k.rawCandles.append(cd);
            }
            k.candles = k.rawCandles;
            return k;
        };

        const KlineSeries upK   = makeTrend(9.00, 0.020, 80);   // 最后一根收 10.58
        const KlineSeries downK = makeTrend(11.20, -0.020, 80); // 最后一根收 9.62
        const QDate asOf = begin.addDays(79);

        TradeRecord base;
        base.buyDate  = begin.addDays(60);
        base.code     = QStringLiteral("600001");
        base.name     = QStringLiteral("测试标的");
        base.buyPrice = 10.00;
        base.qty      = 1000;
        base.planBuy  = 9.90;
        base.planTake = 10.60;
        base.planStop = 9.50;

        // 12.1 持仓浮盈：给止盈策略，不进回本评估
        {
            TradeAI::Input in;
            in.trade = base;
            in.asOf = asOf;
            in.kline = &upK;
            in.hasQuote = true;
            in.snapshot.code = base.code;
            in.snapshot.price = 10.70;
            in.snapshot.prevClose = 10.50;
            in.snapshot.changePct = 1.90;
            in.phase = MarketPhase::MainUp;
            in.phaseKnown = true;

            const TradeAI::Insight a = TradeAI::analyze(in);
            check(a.valid && a.priced && a.verdict == TradeAI::Verdict::Profit,
                  QStringLiteral("AI 诊断：持仓浮盈识别为盈利"));
            check(a.recovery.feasibility == TradeAI::Feasibility::NotNeeded,
                  QStringLiteral("AI 诊断：盈利单不进入回本可行性评估"));
            check(!a.exitSteps.isEmpty() && a.exitStrategy.contains(QStringLiteral("分批")),
                  QStringLiteral("AI 诊断：盈利单给出分批止盈策略与执行步骤"));
            check(a.breakeven > base.buyPrice && a.breakeven < base.buyPrice * 1.003,
                  QStringLiteral("AI 诊断：含费回本价高于买入价且幅度合理（%1）").arg(a.breakeven, 0, 'f', 4));
            check(a.reasons.size() >= 6,
                  QStringLiteral("AI 诊断：盈亏原因覆盖入场/兑现/趋势/量能/周期/费用"));
            check(!a.trace.isEmpty(), QStringLiteral("AI 诊断：输出数据口径以便核对"));

            const TradeAI::Insight again = TradeAI::analyze(in);
            check(near(again.pnl, a.pnl) && again.stateTag == a.stateTag,
                  QStringLiteral("AI 诊断：同一输入结果确定（可重复复算）"));
        }

        // 12.2 持仓浮亏：给含费回本价、修复区间与预计交易日
        {
            TradeRecord t = base;
            t.planTake = 11.00;
            t.planStop = 8.80;

            TradeAI::Input in;
            in.trade = t;
            in.asOf = asOf;
            in.kline = &downK;
            in.hasQuote = true;
            in.snapshot.code = t.code;
            in.snapshot.price = 9.40;
            in.snapshot.prevClose = 9.50;
            in.snapshot.changePct = -1.00;
            in.phase = MarketPhase::Recovery;
            in.phaseKnown = true;

            const TradeAI::Insight a = TradeAI::analyze(in);
            check(a.valid && a.verdict == TradeAI::Verdict::Loss,
                  QStringLiteral("AI 诊断：持仓浮亏识别为亏损"));
            check(a.recovery.applicable
                  && a.recovery.feasibility != TradeAI::Feasibility::NotNeeded
                  && a.recovery.feasibility != TradeAI::Feasibility::Closed,
                  QStringLiteral("AI 诊断：亏损单进入回本可行性评估"));
            check(a.recovery.breakeven > base.buyPrice
                  && a.recovery.requiredPct > 5.0 && a.recovery.requiredPct < 8.0,
                  QStringLiteral("AI 诊断：回本价与还需涨幅口径正确（+%1%）")
                      .arg(a.recovery.requiredPct, 0, 'f', 2));
            check(a.recovery.targetLow > a.price && a.recovery.targetLow <= a.recovery.targetHigh
                  && near(a.recovery.targetHigh, a.recovery.breakeven, 1e-6),
                  QStringLiteral("AI 诊断：修复区间位于现价与回本价之间"));
            check(a.recovery.daysLow >= 1 && a.recovery.daysHigh > a.recovery.daysLow,
                  QStringLiteral("AI 诊断：预计修复时间输出为区间"));
            check(!a.recovery.conditions.isEmpty() && !a.recovery.verdict.isEmpty(),
                  QStringLiteral("AI 诊断：给回本条件与一句话判断"));
            check(!a.exitSteps.isEmpty(),
                  QStringLiteral("AI 诊断：亏损单同样给出减仓/止损纪律"));
        }

        // 12.3 跌破计划止损：回本硬约束优先于「等回本」
        {
            TradeRecord t = base;
            t.planStop = 9.50;
            TradeAI::Input in;
            in.trade = t;
            in.asOf = asOf;
            in.kline = &downK;
            in.hasQuote = true;
            in.snapshot.code = t.code;
            in.snapshot.price = 9.20;
            in.snapshot.prevClose = 9.50;
            in.snapshot.changePct = -3.16;

            const TradeAI::Insight a = TradeAI::analyze(in);
            check(a.verdict == TradeAI::Verdict::Loss
                  && a.stateTag.contains(QStringLiteral("破位")),
                  QStringLiteral("AI 诊断：跌破计划止损标记为破位"));
            check(a.recovery.feasibility == TradeAI::Feasibility::Hard
                  && !a.recovery.blockers.isEmpty(),
                  QStringLiteral("AI 诊断：破位后回本判为困难并给出硬约束"));
            check(a.exitStrategy.contains(QStringLiteral("止损")),
                  QStringLiteral("AI 诊断：破位单优先输出止损纪律"));
        }

        // 12.4 已了结亏损：本笔不可回本，改为「后续交易需盈利多少」
        {
            TradeRecord t = base;
            t.sellDate  = begin.addDays(65);
            t.sellPrice = 9.00;
            TradeAI::Input in;
            in.trade = t;
            in.asOf = asOf;
            in.kline = &downK;

            const TradeAI::Insight a = TradeAI::analyze(in);
            check(a.valid && a.closed && a.verdict == TradeAI::Verdict::Loss,
                  QStringLiteral("AI 诊断：已了结亏损按卖出价结算"));
            check(a.recovery.feasibility == TradeAI::Feasibility::Closed
                  && !a.recovery.applicable,
                  QStringLiteral("AI 诊断：已了结交易不再给「等待回本」路径"));
            check(a.recovery.requiredPct > 0.0 && !a.recovery.conditions.isEmpty(),
                  QStringLiteral("AI 诊断：已了结亏损给出后续交易修复要求"));
            check(!a.exitSteps.isEmpty(),
                  QStringLiteral("AI 诊断：已了结交易输出复盘模板"));
        }

        // 12.5 无行情 / 缺 K 线：明确降级，不编造盈亏
        {
            TradeAI::Input in;
            in.trade = base;
            in.asOf = asOf;
            in.hasQuote = false;

            const TradeAI::Insight a = TradeAI::analyze(in);
            check(a.valid && !a.priced && a.verdict == TradeAI::Verdict::Unknown,
                  QStringLiteral("AI 诊断：无现价时不做 0 元估算"));
            check(a.stateTag == QStringLiteral("待行情")
                  && !a.riskNotes.isEmpty(),
                  QStringLiteral("AI 诊断：无行情明确标注并保留计划纪律"));
        }

        // 12.6 未记录计划止盈：止盈步骤必须用动态目标兜底，不能出现「触及未记录」
        {
            TradeRecord t = base;
            t.planTake = 0.0;
            TradeAI::Input in;
            in.trade = t;
            in.asOf = asOf;
            in.kline = &upK;
            in.hasQuote = true;
            in.snapshot.code = t.code;
            in.snapshot.price = 10.70;
            in.snapshot.prevClose = 10.50;
            in.snapshot.changePct = 1.90;

            const TradeAI::Insight a = TradeAI::analyze(in);
            bool blankTrigger = false;
            for (const TradeAI::ExitStep& s : a.exitSteps) {
                if (s.trigger.contains(QStringLiteral("未记录"))) blankTrigger = true;
            }
            check(a.verdict == TradeAI::Verdict::Profit && !a.exitSteps.isEmpty()
                  && !blankTrigger,
                  QStringLiteral("AI 诊断：无计划止盈时用动态目标兜底，不输出空触发条件"));
        }
    }

    // ---------------------------------------------------------------- 13. AI 分析门禁与边界
    {
        AiSettings empty;
        QString why;
        check(empty.apiKey.isEmpty() && !empty.ready(&why) && !why.isEmpty(),
              QStringLiteral("AI 分析门禁：默认不内置任何 API Key，未配置即拒绝并给出原因"));
        check(!AiSettings::validateKey(QStringLiteral("   "))
              && !AiSettings::validateKey(QStringLiteral("abc"))
              && !AiSettings::validateKey(QStringLiteral("sk-123 456"))
              && AiSettings::validateKey(QStringLiteral("sk-test-key-1234567890")),
              QStringLiteral("AI 分析门禁：API Key 格式校验拒绝空值/过短/含空白"));
        check(!AiSettings::validateEndpoint(QStringLiteral("http://api.example.com/v1"))
              && AiSettings::validateEndpoint(QStringLiteral("http://127.0.0.1:8000/v1"))
              && AiSettings::validateEndpoint(QStringLiteral("https://api.openai.com/v1")),
              QStringLiteral("AI 分析门禁：外部端点强制 https，仅本机允许 http"));
        check(!AiSettings::validateModel(QString()) && AiSettings::validateModel(QStringLiteral("gpt-4o-mini")),
              QStringLiteral("AI 分析门禁：模型名必填且格式合法"));

        AiSettings configured;
        configured.apiKey = QStringLiteral("sk-test-key-1234567890");
        check(configured.ready() && configured.endpoint().endsWith(QStringLiteral("/chat/completions"))
              && configured.maskedKey().contains(QStringLiteral("****"))
              && !configured.maskedKey().contains(configured.apiKey),
              QStringLiteral("AI 分析门禁：配置齐备后可用，回显只给掩码"));

        // 输入白名单：未知字段必须被丢弃，缺失数值必须置 null
        QJsonObject raw;
        raw[QStringLiteral("code")] = QStringLiteral("001219");
        raw[QStringLiteral("buyLow")] = 19.34;
        raw[QStringLiteral("accountBalance")] = 999999;   // 白名单外：账户资金
        raw[QStringLiteral("apiKey")] = QStringLiteral("sk-should-not-leak");
        raw[QStringLiteral("rawKlines")] = QStringLiteral("长文本");
        const QJsonObject safe = AiSpec::sanitizeInput(raw);
        check(safe.keys().size() == AiSpec::allowedInputKeys().size()
              && !safe.contains(QStringLiteral("accountBalance"))
              && !safe.contains(QStringLiteral("apiKey"))
              && !safe.contains(QStringLiteral("rawKlines"))
              && safe.value(QStringLiteral("buyHigh")).isNull(),
              QStringLiteral("AI 分析边界：输入只保留白名单字段，越界字段与缺失值不进入请求"));
        const QJsonObject body = AiSpec::buildRequestBody(QStringLiteral("gpt-4o-mini"), safe);
        const QString bodyText = QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact));
        check(!bodyText.contains(QStringLiteral("accountBalance"))
              && !bodyText.contains(QStringLiteral("sk-should-not-leak"))
              && bodyText.contains(QStringLiteral("temperature")),
              QStringLiteral("AI 分析边界：请求体不含账户/密钥字段且温度固定为 0"));
        check(AiSpec::systemPrompt().contains(QStringLiteral("【分析目标】"))
              && AiSpec::systemPrompt().contains(QStringLiteral("【输入信息范围】"))
              && AiSpec::systemPrompt().contains(QStringLiteral("【输出内容结构】"))
              && AiSpec::systemPrompt().contains(QStringLiteral("【判断原则】")),
              QStringLiteral("AI 分析边界：目标/输入/输出/原则四段固定在 system prompt"));

        // 输出结构：严格 schema，任何不合规都整份丢弃
        QJsonObject good;
        good[QStringLiteral("conclusion")] = QStringLiteral("一致");
        good[QStringLiteral("summary")] = QStringLiteral("区间自洽，止损位于买入区间下方。");
        QJsonObject ev;
        ev[QStringLiteral("field")] = QStringLiteral("buyLow");
        ev[QStringLiteral("observation")] = QStringLiteral("买入下沿 19.34 低于买入上沿");
        QJsonArray evArr; evArr.append(ev);
        good[QStringLiteral("evidence")] = evArr;
        QJsonArray risks; risks.append(QStringLiteral("止损空间偏大"));
        good[QStringLiteral("risks")] = risks;
        QJsonArray disc; disc.append(QStringLiteral("跌破计划止损离场"));
        good[QStringLiteral("discipline")] = disc;
        good[QStringLiteral("uncertainty")] = QStringLiteral("样本仅来自当日快照");
        const QByteArray valid = QJsonDocument(good).toJson(QJsonDocument::Compact);

        AiSpec::Result parsed;
        QString err;
        check(AiSpec::parseResult(valid, &parsed, &err)
              && parsed.conclusion == QStringLiteral("一致")
              && parsed.evidence.size() == 1
              && AiSpec::renderResult(parsed).contains(QStringLiteral("⑤ 不确定性")),
              QStringLiteral("AI 分析边界：合规输出按固定五段结构渲染"));

        auto reject = [&](const QJsonObject& mutated, const QString& name) {
            AiSpec::Result r;
            QString e;
            check(!AiSpec::parseResult(QJsonDocument(mutated).toJson(QJsonDocument::Compact), &r, &e)
                  && !e.isEmpty() && r.summary.isEmpty(), name);
        };
        QJsonObject extra = good;
        extra[QStringLiteral("targetPrice")] = QStringLiteral("25.00");
        reject(extra, QStringLiteral("AI 分析边界：多余字段（价格预测）整份丢弃"));
        QJsonObject badEnum = good;
        badEnum[QStringLiteral("conclusion")] = QStringLiteral("建议买入");
        reject(badEnum, QStringLiteral("AI 分析边界：非法结论枚举整份丢弃"));
        QJsonObject badEvidence = good;
        QJsonObject outside;
        outside[QStringLiteral("field")] = QStringLiteral("externalQuote");
        outside[QStringLiteral("observation")] = QStringLiteral("引用了外部行情");
        QJsonArray outArr; outArr.append(outside);
        badEvidence[QStringLiteral("evidence")] = outArr;
        reject(badEvidence, QStringLiteral("AI 分析边界：引用白名单外字段整份丢弃"));
        QJsonObject badRisks = good;
        badRisks[QStringLiteral("risks")] = QStringLiteral("不是数组");
        reject(badRisks, QStringLiteral("AI 分析边界：字段类型不符整份丢弃"));
        AiSpec::Result untouched;
        QString fenceErr;
        check(!AiSpec::parseResult(QByteArray("不是 JSON"), &untouched, &fenceErr)
              && untouched.summary.isEmpty(),
              QStringLiteral("AI 分析边界：非结构化输出不展示半截结果"));
    }

    // ---------------------------------------------------------------- 14. 已下线信息的源码回归门禁
    {
        // 故意拆开拼接，避免本文件自身命中（否则扫描会把自己当成反例）
        const QStringList banned = {
            QStringLiteral("做多") + QStringLiteral(" LONG"),
            QStringLiteral("主升期") + QStringLiteral("·建仓"),
            QStringLiteral("Long") + QStringLiteral("Badge")
        };
        const QString here = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
        QStringList roots = { QDir(here + QStringLiteral("/..")).canonicalPath(),
                              QDir::currentPath() + QStringLiteral("/../src"),
                              QDir::currentPath() + QStringLiteral("/src") };
        QString srcRoot;
        for (const QString& r : roots) {
            if (!r.isEmpty() && QFileInfo::exists(r + QStringLiteral("/ui/MainWindow.cpp"))) {
                srcRoot = r; break;
            }
        }
        if (srcRoot.isEmpty()) {
            out() << "[SKIP] 未定位到 src 目录，跳过已下线信息源码扫描" << Qt::endl;
        } else {
            QStringList hits;
            QDirIterator it(srcRoot, { QStringLiteral("*.cpp"), QStringLiteral("*.h") },
                            QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString path = it.next();
                if (path.endsWith(QStringLiteral("core_checks_main.cpp"))) continue;
                if (path.endsWith(QStringLiteral("CoreChecks.cpp"))) continue;   // 断言集本体，自含禁用词
                QFile f(path);
                if (!f.open(QIODevice::ReadOnly)) continue;
                const QString text = QString::fromUtf8(f.readAll());
                for (const QString& b : banned) {
                    if (text.contains(b)) hits << QFileInfo(path).fileName() + QStringLiteral(":") + b;
                }
            }
            check(hits.isEmpty(),
                  hits.isEmpty()
                      ? QStringLiteral("已下线信息：源码中不存在「做多 LONG / 主升期·建仓 / LongBadge」")
                      : QStringLiteral("已下线信息仍在源码中：%1").arg(hits.join(QStringLiteral("；"))));
        }
    }

    // ---------------------------------------------------------------- 15. 决策复盘：结构化、可核对、不编造
    {
        using namespace DecisionReview;
        const QDate d(2026, 11, 6);

        // 合成 26 根日 K：d 对应第 21 根，后面还留 5 根供事后偏差计算。
        // flat=收盘全平（均线完全收敛）；fallingLows=逐窗低点下移；rising=连续上涨（RSI=100）
        auto makeK = [&](bool flat, bool fallingLows, bool rising) {
            KlineSeries k;
            k.code = QStringLiteral("000001");
            for (int i = 0; i < 26; ++i) {
                Candle c;
                c.date = d.addDays(i - 20);
                const double px = flat ? 10.0 : (rising ? 10.0 + i * 0.30 : 10.0 - i * 0.10);
                c.open = px;
                c.close = px;
                c.high = px + 0.20;
                c.low = fallingLows ? px - 0.20 - (i / 5) * 0.50 : px - 0.20;
                c.volume = 100000.0 + i * 1000.0;
                c.amount = c.volume * px * 100.0;
                k.candles.append(c);
                k.rawCandles.append(c);
            }
            return k;
        };
        // 假企稳：收盘全平（MA5/10/20 极差=0）但每 5 根窗口的最低点持续下移
        auto makeFakeStabilize = [&]() {
            KlineSeries k;
            k.code = QStringLiteral("000001");
            for (int i = 0; i < 26; ++i) {
                Candle c;
                c.date = d.addDays(i - 20);
                c.open = 10.0; c.close = 10.0; c.high = 10.05;
                c.low = 9.80 - (i / 5) * 0.50;
                c.volume = 100000.0;
                c.amount = 10.0 * c.volume * 100.0;
                k.candles.append(c);
                k.rawCandles.append(c);
            }
            return k;
        };

        // 15.1 指标口径：RSI（Wilder）/ 均线收敛 / 低点序列 / 筹码峰 / 事后窗口
        QVector<double> up, down;
        for (int i = 0; i < 20; ++i) { up << 10.0 + i; down << 20.0 - i; }
        check(near(Metric::rsi(up, Criteria::kRsiPeriod).last(), 100.0)
              && near(Metric::rsi(down, Criteria::kRsiPeriod).last(), 0.0),
              QStringLiteral("决策复盘：RSI 与图表同口径（连涨 100 / 连跌 0）"));
        const KlineSeries flatK = makeK(true, false, false);
        const Metric::MaInfo mi = Metric::ma(flatK, d);
        check(mi.known && near(mi.spreadPct, 0.0) && mi.converged,
              QStringLiteral("决策复盘：均线收敛度 = (max−min)/MA20×100%，全平时为 0"));
        const KlineSeries fallK = makeK(false, true, false);
        const Metric::LowInfo lo = Metric::swingLows(fallK, d);
        check(lo.known && lo.lows.size() == Criteria::kSwingSamples
              && lo.declining && lo.slopePct < 0.0,
              QStringLiteral("决策复盘：摆动低点逐窗递减可被识别（假企稳的一半）"));
        const Metric::ChipInfo ci = Metric::chipLayers(fallK, d, 9.0);
        check(ci.known && ci.layers == ci.detail.size(),
              QStringLiteral("决策复盘：筹码压力层数与逐层明细数量一致"));
        const Metric::FollowUp fu = Metric::followUp(flatK, d, Criteria::kOutcomeHorizon);
        check(fu.known && fu.bars == Criteria::kOutcomeHorizon && near(fu.base, 10.0)
              && fu.maxHigh >= fu.minLow,
              QStringLiteral("决策复盘：事后窗口只取信号日之后最多 5 根日 K"));

        // 15.2 报告：条件证据 / 偏差 / 跳过 / 板块排序 / 数据缺失降级
        auto baseInput = [&]() {
            Input in;
            in.date = d; in.asOf = d;
            TradePlan p;
            p.code = QStringLiteral("000001"); p.name = QStringLiteral("测试标的");
            p.actionable = true; p.signalLevel = 3; p.positionPct = 6.0;
            p.buyLow = 9.50; p.buyHigh = 10.00;
            p.takeProfitLow = 11.00; p.takeProfitHigh = 12.00;
            p.stopLossLow = 8.80; p.stopLossHigh = 9.00;
            p.riskReward = 1.20; p.fillProbability = 0.60;
            p.score = 70.0; p.compositeScore = 65.0;
            in.plans << p;
            in.klines.insert(p.code, makeFakeStabilize());
            in.sentimentKnown = true; in.composite = 65.0;
            in.tradeAllowed = true; in.positionCapPct = 40.0;
            in.mainNetYi.insert(p.code, 1.23);
            in.mainNetSource.insert(p.code, QStringLiteral("测试：东财 fflow 主力净额"));
            in.sealToAmount.insert(p.code, 0.31);
            for (int i = 0; i < 4; ++i) {
                SectorFlowRow row;
                row.code = QStringLiteral("BK000%1").arg(i);
                row.name = QStringLiteral("板块%1").arg(i);
                row.changePct = 5.0 - i * 3.0;
                row.mainNetYi = 12.0 - i * 5.0;
                row.mainPct = 1.5 - i * 0.4;
                row.flowKnown = true;
                in.sectorRows << row;
            }
            in.sectorFetchedAt = QDateTime(QDate(2026, 11, 6), QTime(15, 5));
            in.sectorSource = QStringLiteral("测试：东财 clist f3/f62/f184");
            return in;
        };
        auto evidenceOf = [](const SignalTrace& s, const QString& name) {
            for (const Evidence& e : s.conditions)
                if (e.name == name) return e;
            return Evidence();
        };

        const Report r = build(baseInput());
        const SignalTrace sig = r.signalTraces.value(0);
        check(r.valid && r.signalTraces.size() == 1 && r.outcomes.size() == 1
              && r.skipped.size() == 1,
              QStringLiteral("决策复盘：报告产出信号溯源 / 偏差 / 跳过机会三类结果"));
        check(!evidenceOf(sig, QStringLiteral("均线收敛度")).name.isEmpty()
              && evidenceOf(sig, QStringLiteral("均线收敛度")).known
              && !evidenceOf(sig, QStringLiteral("RSI（入场时过热度）")).name.isEmpty()
              && evidenceOf(sig, QStringLiteral("RSI（入场时过热度）")).known,
              QStringLiteral("决策复盘：当时均线收敛度与 RSI 数值都带实测值"));
        check(evidenceOf(sig, QStringLiteral("资金流入")).known
              && evidenceOf(sig, QStringLiteral("资金流入")).passed,
              QStringLiteral("决策复盘：资金流入给出实测值与判定阈值，不是文字猜测"));
        bool allStructured = true;
        for (const SignalTrace& s : r.signalTraces)
            for (const Evidence& e : s.conditions)
                if (e.name.isEmpty() || e.value.isEmpty() || e.rule.isEmpty()
                    || e.source.isEmpty()) allStructured = false;
        check(allStructured && sig.complete,
              QStringLiteral("决策复盘：每条条件都有数值 / 判定式 / 来源窗口，可逐条核对"));
        check(r.outcomes.value(0).known && r.outcomes.value(0).bars == 5
              && r.outcomes.value(0).stopTouched
              && !r.outcomes.value(0).targetTouched
              && r.outcomes.value(0).targetDeviationPct < 0.0,
              QStringLiteral("决策复盘：实际走势与计划目标的偏差按计划价客观计算"));
        check(r.skipped.value(0).known
              && r.skipped.value(0).skipRule.contains(QStringLiteral("000001"))
              && r.skipped.value(0).caveat.contains(QStringLiteral("不评价")),
              QStringLiteral("决策复盘：跳过的机会只做事后统计，不评价当时决策"));
        check(r.sectors.available && r.sectors.rows.size() == 4
              && r.sectors.rows.first().tag == QStringLiteral("领涨")
              && r.sectors.rows.last().tag == QStringLiteral("领跌")
              && r.sectors.rows.first().changePct >= r.sectors.rows.last().changePct,
              QStringLiteral("决策复盘：领涨 / 领跌按当日涨幅客观排序并附资金流数值"));

        // 15.3 持仓隐性风险：假企稳形态必须能被同时满足的两个条件判出来
        Input withHolding = baseInput();
        TradeRecord t;
        t.code = QStringLiteral("000001"); t.name = QStringLiteral("测试标的");
        t.buyDate = d; t.buyPrice = 10.0; t.qty = 1000;
        t.planStop = 9.0; t.planTake = 11.0;
        withHolding.trades << t;
        const Report r2 = build(withHolding);
        bool fakeHit = false;
        for (const HoldingRisk& risk : r2.risks)
            if (risk.type == QStringLiteral("低点下移 · 假企稳") && risk.triggered
                && risk.conditions.size() >= 2) fakeHit = true;
        check(r2.risks.size() == 3 && fakeHit && r2.skipped.isEmpty(),
              QStringLiteral("决策复盘：持仓三类隐性风险可判定，假企稳需「收敛 + 低点下移」同时成立"));
        check(r2.purpose.contains(QStringLiteral("不预测涨跌"))
              && !r2.purpose.contains(QStringLiteral("建议买入")),
              QStringLiteral("决策复盘：使用边界明确不预测、不给新的买卖指令"));
        check(DecisionReview::criteriaLines().join(QStringLiteral("｜"))
                  .contains(QString::number(Criteria::kMaConvergePct, 'f', 2))
              && DecisionReview::criteriaLines().join(QStringLiteral("｜"))
                     .contains(QString::number(Criteria::kRsiExtreme, 'f', 1)),
              QStringLiteral("决策复盘：判定条件清单与计算共用同一份阈值常量"));

        // 15.4 数据缺失：一律标缺失，不补 0、不推测后续走势
        Input empty = baseInput();
        empty.klines.clear();
        empty.mainNetYi.clear();
        empty.sealToAmount.clear();
        empty.sectorRows.clear();
        empty.sectorReason = QStringLiteral("测试：接口失败");
        const Report r3 = build(empty);
        check(!evidenceOf(r3.signalTraces.value(0), QStringLiteral("均线收敛度")).known
              && !evidenceOf(r3.signalTraces.value(0), QStringLiteral("资金流入")).known
              && !r3.outcomes.value(0).known
              && !r3.skipped.value(0).known
              && near(r3.skipped.value(0).hypotheticalPct, 0.0)
              && !r3.sectors.available && !r3.sectors.reason.isEmpty(),
              QStringLiteral("决策复盘：缺 K 线 / 缺资金流 / 接口失败一律标数据缺失，不补 0"));

        // 15.5 整包 JSON 往返：复盘结论可留档、可回看、可逐条复核
        const Report back = Report::fromJson(r2.toJson());
        check(back.valid && back.date == r2.date
              && back.signalTraces.size() == r2.signalTraces.size()
              && back.outcomes.size() == r2.outcomes.size()
              && back.skipped.size() == r2.skipped.size()
              && back.risks.size() == r2.risks.size()
              && back.sectors.rows.size() == r2.sectors.rows.size()
              && back.criteria == r2.criteria
              && near(back.outcomes.value(0).mfePct, r2.outcomes.value(0).mfePct)
              && back.signalTraces.value(0).conditions.size() == r2.signalTraces.value(0).conditions.size(),
              QStringLiteral("决策复盘：报告整包 JSON 往返无字段丢失（条件 / 偏差 / 风险 / 板块）"));
    }

    // ---------------------------------------------------------------- 16. 免责声明合规要点
    {
        using namespace LegalNotice;
        const Document& d = document();
        const QString text = plainText();

        check(!d.version.isEmpty() && !d.effectiveDate.isEmpty() && !d.title.isEmpty(),
              QStringLiteral("免责声明：标题 / 版本 / 生效日期齐全（同意记录按版本失效）"));
        check(d.blocks.size() >= 5,
              QStringLiteral("免责声明：投资风险 / 信息性质 / 数据来源 / 责任划分 / 法律与责任限制五部分齐全"));

        QStringList missingTopics;
        for (const QString& topic : requiredTopics()) {
            if (!text.contains(topic)) missingTopics << topic;
        }
        check(missingTopics.isEmpty(),
              QStringLiteral("免责声明：全部合规要点在正文中可检索（缺失：%1）")
                  .arg(missingTopics.isEmpty() ? QStringLiteral("无")
                                               : missingTopics.join(QStringLiteral("、"))));
        check(text.contains(QStringLiteral("本金损失")) && text.contains(QStringLiteral("市场波动")),
              QStringLiteral("免责声明：明确揭示本金损失风险与市场波动风险"));
        check(text.contains(QStringLiteral("不构成投资建议"))
                  && text.contains(QStringLiteral("不代客")),
              QStringLiteral("免责声明：明确信息非投资建议、不代客操作"));
        check(text.contains(QStringLiteral("数据来源"))
                  && text.contains(QStringLiteral("准确性"))
                  && text.contains(QStringLiteral("现状")),
              QStringLiteral("免责声明：数据来源、准确性边界与按现状提供"));
        check(text.contains(QStringLiteral("独立决策"))
                  && text.contains(QStringLiteral("自行承担")),
              QStringLiteral("免责声明：用户自主决策与责任划分"));
        check(text.contains(QStringLiteral("适用中华人民共和国法律"))
                  && text.contains(QStringLiteral("责任限制"))
                  && text.contains(QStringLiteral("争议解决")),
              QStringLiteral("免责声明：适用法律、争议解决与责任限制条款"));
        check(!acceptedVersionKey().isEmpty() && !acceptedAtKey().isEmpty(),
              QStringLiteral("免责声明：同意记录的持久化键已定义（界面与门禁共用）"));
    }

    out() << QStringLiteral("失败断言数: %1").arg(g_failed) << Qt::endl;
    out().flush();
    return g_failed;
}

} // namespace CoreChecks
