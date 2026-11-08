#include "strategy/RecommendEngine.h"    // 本模块声明
#include "strategy/Scorer.h"              // 打板 8 因子明细（得分由来）
#include "core/Auction.h"                 // 竞价规则与判定
#include "core/Indicators.h"              // 指标工具
#include <QDateTime>                      // 时间戳
#include <QSet>                           // 候选池去重
#include <QTime>                          // 交易时段判断
#include <algorithm>                      // 按综合评分排序

namespace {

// 把多因子评估结果整理成「查看综合分由来」所需的明细
ScoreBreakdown makeCompositeBreakdown(const FactorEvaluation& ev, const FactorConfig& cfg)
{
    ScoreBreakdown sb;
    sb.title = QStringLiteral("综合评分");
    sb.value = ev.composite;

    const bool bucket = (cfg.rule == CombineRule::MinPenalty);
    sb.formula = bucket
        ? QStringLiteral("综合分 = (0.75 × Σ(归一化权重 × 维度得分) + 0.25 × 最低维度得分) × 100")
        : QStringLiteral("综合分 = Σ(归一化权重 × 维度得分) × 100");
    sb.source = QStringLiteral(
        "多因子引擎（实盘与回测同口径）：打板核心来自 Scorer 8 因子；趋势 / 动量 / 波动率来自前复权日 K；"
        "资金流向为封单与成交额的代理口径；基本面来自快照 PE / PB / 流通市值");

    for (const FactorContribution& fc : ev.contributions) {
        ScoreItem it;
        it.name = fc.name;
        it.value = fc.normalized;
        it.weight = fc.weight;
        it.contribution = fc.contribution;
        it.available = fc.available;
        it.detail = fc.detail;
        if (!fc.available) {
            it.detail += QStringLiteral("（数据不足，未参与评分）");
        } else if (fc.coverage < 0.999) {
            it.detail += QStringLiteral("（数据覆盖率 %1%，缺失子项已按剩余子项归一）")
                             .arg(fc.coverage * 100.0, 0, 'f', 0);
        }
        sb.items.append(it);
    }

    sb.notes << QStringLiteral("权重口径：%1").arg(cfg.summary());
    if (bucket) sb.notes << QStringLiteral("木桶规则：加权分占 75%，最低维度分占 25%");
    if (!ev.rejectReason.isEmpty()) sb.notes << QStringLiteral("过滤结果：%1").arg(ev.rejectReason);
    if (!ev.summary.isEmpty()) sb.notes << ev.summary;
    return sb;
}

} // namespace

RecommendEngine::RecommendEngine(QObject* parent) : QObject(parent) {}  // 构造：无额外初始化

void RecommendEngine::setFactorData(const QHash<QString, Snapshot>* snapshots,
                                    const QHash<QString, KlineSeries>* klines,
                                    const QDate& asOf)
{
    m_snapshots = snapshots;
    m_klines = klines;
    m_asOf = asOf;                            // K 线统一按此日截断，杜绝未来函数
}

// 构建单只计划，并叠加竞价校验
void RecommendEngine::appendPlan(QVector<TradePlan>& out, const Candidate& c,
                                 const QHash<QString, KlineSeries>& klines,
                                 const MarketRegime& regime,
                                 bool actionable, const QString& noteOverride)
{
    const KlineSeries* k = nullptr;               // 该票日 K（可能缺失）
    auto it = klines.constFind(c.entry.code);     // 查 K 线表
    if (it != klines.constEnd() && it.value().valid()) k = &it.value();  // 有效才使用

    TradePlan tp = Plan::build(c, k, regime, actionable);  // 核心建计划
    if (!tp.valid()) return;                      // 计划无效则丢弃
    tp.dabanBreakdown = Scorer().describe(c, tp.score);   // 打板分由来（8 因子 + 修正项）

    const QDate today = QDate::currentDate();     // 今天
    const QTime now = QTime::currentTime();       // 当前时间
    const bool sameDay = (regime.date == today);  // 计划是否为当日
    const bool afterAuction = now >= QTime(9, 25);  // 是否已过集合竞价
    const bool amountKnown = sameDay && now >= QTime(9, 25) && now <= QTime(9, 31);  // 9:25~9:31 竞价额可单独取得
    const bool hasQuote = sameDay && afterAuction                 // 当日且已过竞价
                          && c.snapshot.open > 0.0 && c.snapshot.prevClose > 0.0;  // 且价格有效

    const AuctionAdvice adv = Auction::evaluate(  // 竞价判定
        Auction::ruleFor(c.entry), hasQuote,
        c.snapshot.open, c.snapshot.prevClose,
        c.snapshot.amount, c.entry.amount, amountKnown);

    tp.auctionEvaluated   = adv.evaluated;        // 是否已评估
    tp.auctionVerdict     = static_cast<int>(adv.verdict);  // 裁决枚举转整数
    tp.auctionOpenPct     = adv.openPct;          // 实际高开幅度
    tp.auctionAmountRatio = adv.amountRatio;      // 竞价量能占比
    tp.auctionNote        = adv.note;             // 结论文案
    if (!adv.rule.basis.isEmpty()) tp.auctionBasis = adv.rule.basis;  // 规则说明优先用判定结果里的

    if (tp.actionable && adv.verdict == AuctionAdvice::Abandon) {  // 竞价明确放弃
        tp.actionable = false;                    // 降级为不可执行
        tp.positionPct = 0.0;                     // 仓位归零
        tp.note = QStringLiteral("竞价否决：%1").arg(adv.note);  // 说明否决原因
    }

    if (!noteOverride.isEmpty()) tp.note = noteOverride;   // 调用方指定文案优先
    if (!actionable && !c.rejectReason.isEmpty()) {        // 观察名单需写明淘汰原因
        tp.riskNote = QStringLiteral("淘汰原因：%1 ｜ 观察触发条件：情绪修复且该因素达标")
                          .arg(c.rejectReason);
    }
    out.append(tp);                               // 追加到结果
}

// 装配整份荐股清单
RecommendBundle RecommendEngine::build(const QVector<Candidate>& passed,
                                       const QHash<QString, KlineSeries>& klines,
                                       const MarketRegime& regime,
                                       FilterPipeline* pipeline)
{
    RecommendBundle bundle;                       // 输出结果
    bundle.regime = regime;                       // 携带情绪快照
    bundle.tradeDate = regime.date;               // 结论所属交易日
    bundle.updatedAt = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));  // 数据时间戳

    const bool gated = !regime.tradeAllowed;      // 闸门是否拦截

    // 闸门拦截时 filter 返回值为空，真正「因子全达标」的名单保留在 lastPassedPreGate 中
    const QVector<Candidate> eligible = pipeline ? pipeline->lastPassedPreGate() : passed;

    if (!gated) {                                 // 闸门放行：正常荐股
        for (const Candidate& c : eligible) {     // 逐只建计划
            appendPlan(bundle.plans, c, klines, regime, true, QString());
        }
        // 候选池数量优先：分层量价差一点的第二梯队也进池，作为「备选」观察项。
        // 它们不进入计划明细 / 推送 / 落库推荐名单，只扩充决策台的候选池。
        if (pipeline) {
            QSet<QString> known;
            for (const TradePlan& p : bundle.plans) known.insert(p.code);
            for (const Candidate& c : pipeline->lastNearMiss()) {
                if (known.contains(c.entry.code)) continue;
                appendPlan(bundle.backups, c, klines, regime, false,
                           QStringLiteral("备选：%1").arg(
                               c.rejectReason.isEmpty() ? QStringLiteral("分层量价差一点达标")
                                                        : c.rejectReason));
            }
        }
    } else {                                      // 闸门拦截：降级为观察名单
        if (pipeline) {
            for (const Candidate& c : pipeline->lastPassedPreGate()) {  // 第一梯队：因子全达标仅环境不允许
                appendPlan(bundle.plans, c, klines, regime, false,
                           QStringLiteral("仅观察：因子全达标，但%1").arg(regime.note));
            }
            for (const Candidate& c : pipeline->lastNearMiss()) {       // 第二梯队：差一点达标
                appendPlan(bundle.plans, c, klines, regime, false,
                           QStringLiteral("仅观察：%1（明日重点跟踪）").arg(c.rejectReason));
            }
        }
        bundle.notice = QStringLiteral("市场情绪未达出手条件（%1）—— 今日不出手。下方为已算好的观察名单，"
                                       "区间已按同一套模型计算，供明日修复时参考")
                            .arg(regime.note);
    }

    if (pipeline) {                               // 漏斗与淘汰明细：回答「47 只只给 2 只」
        const FilterPipeline::Funnel f = pipeline->lastFunnel();  // 各层通过数
        if (f.input > 0) {                        // 有输入才有意义
            bundle.funnel.append({ QStringLiteral("涨停池"),     f.input,           false });  // 第 1 层
            bundle.funnel.append({ QStringLiteral("硬排除后"),   f.afterHardFilter, false });  // 第 2 层
            bundle.funnel.append({ QStringLiteral("题材后"),     f.afterSector,     false });  // 第 3 层
            bundle.funnel.append({ QStringLiteral("分层量价后"), f.afterQuant,      false });  // 第 4 层
            bundle.funnel.append({ QStringLiteral("输出"),       f.output,          true  });  // 末端
        }
        for (const FilterPipeline::RejectRecord& r : pipeline->lastRejects()) {   // 逐条淘汰明细
            bundle.rejects.append({ r.code, r.name, r.stage, r.reason });         // 转成对外结构
        }
    }

    // ---- 多维度评分 / 排名 / 贡献度（实盘与回测共用 MultiFactorEngine）----
    QVector<Candidate> all = eligible;
    if (pipeline) {
        for (const Candidate& c : pipeline->lastNearMiss()) {   // 观察名单也给出综合评分
            bool dup = false;
            for (const Candidate& e : all) {
                if (e.entry.code == c.entry.code) { dup = true; break; }
            }
            if (!dup) all.append(c);
        }
    }
    applyFactorRanking(bundle, all);

    // 结论文案放在评分之后：候选池 / 推荐 / 备选的数量与界面实际展示一致
    if (!gated) {
        if (bundle.plans.isEmpty()) {             // 无合格标的
            bundle.notice = QStringLiteral("市场情绪满足出手条件，但分层量价筛选未留下标的 —— 今日无合格打板标的，空仓等待");
        } else {
            const int pickCount = qMin(kConcentratedPositionCount, int(bundle.plans.size()));
            bundle.notice = QStringLiteral(
                "市场情绪满足出手条件（%1），候选池 %2 只（推荐 %3 只 + 备选 %4 只），含买入/止盈/止损区间")
                    .arg(marketPhaseName(regime.phase))
                    .arg(int(bundle.plans.size() + bundle.backups.size()))
                    .arg(pickCount)
                    .arg(int(bundle.backups.size()));
        }
    }

    m_bundle = bundle;                            // 缓存最近一次结果
    return bundle;                                // 返回
}

void RecommendEngine::applyFactorRanking(RecommendBundle& bundle,
                                         const QVector<Candidate>& eligible)
{
    static const QHash<QString, Snapshot> emptySnapshots;
    static const QHash<QString, KlineSeries> emptyKlines;
    const QHash<QString, Snapshot>& snaps = m_snapshots ? *m_snapshots : emptySnapshots;
    const QHash<QString, KlineSeries>& kls = m_klines ? *m_klines : emptyKlines;

    FactorConfig cfg = m_factorCfg;
    cfg.validate();
    const QVector<FactorInput> inputs =
        MultiFactorEngine::makeInputs(eligible, snaps, kls, m_asOf);
    m_factorReport = MultiFactorEngine::rank(inputs, cfg);
    bundle.factorDigest = m_factorReport.configDigest;
    bundle.factorRule = cfg.summary();

    QHash<QString, FactorEvaluation> byCode;
    for (const FactorEvaluation& ev : m_factorReport.ranking) byCode.insert(ev.code, ev);
    for (const FactorEvaluation& ev : m_factorReport.rejected) byCode.insert(ev.code, ev);

    // 推荐名单与候选池备选共用同一份评分结果，保证池内分数口径一致
    auto applyOne = [&byCode, &cfg](TradePlan& p) {
        const auto it = byCode.constFind(p.code);
        if (it == byCode.constEnd()) return;
        p.compositeScore = it->composite;
        p.factorRank = it->rank;
        p.contributions = it->contributions;
        p.compositeBreakdown = makeCompositeBreakdown(*it, cfg);   // 综合分由来
        p.factorSummary = it->summary;
        p.factorReject = it->passed ? QString() : it->rejectReason;
        if (!it->passed) {                     // 多因子过滤 → 降级为观察
            p.actionable = false;
            p.positionPct = 0.0;
            p.note = QStringLiteral("多因子过滤：%1").arg(it->rejectReason);
        }
    };
    for (TradePlan& p : bundle.plans) applyOne(p);
    for (TradePlan& p : bundle.backups) applyOne(p);

    // TopN 截断后拿不到评分的备选不进池：候选池里不应出现「0 分假标的」
    QVector<TradePlan> keptBackups;
    keptBackups.reserve(bundle.backups.size());
    for (const TradePlan& p : bundle.backups) {
        if (byCode.contains(p.code)) keptBackups.append(p);
    }
    bundle.backups = keptBackups;

    const auto byComposite = [](const TradePlan& a, const TradePlan& b) {
        if (a.compositeScore == b.compositeScore) return a.code < b.code;
        return a.compositeScore > b.compositeScore;   // 按综合评分排序
    };
    std::sort(bundle.plans.begin(), bundle.plans.end(), byComposite);
    std::sort(bundle.backups.begin(), bundle.backups.end(), byComposite);
}
