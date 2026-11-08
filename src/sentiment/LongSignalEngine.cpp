#include "sentiment/LongSignalEngine.h"  // 本模块声明
#include "sentiment/SentimentEngine.h"   // 阶段 → 板数窗口
#include <QSet>                          // 已占用题材集合
#include <QStringList>                   // 条件/阻塞项列表
#include <algorithm>                     // std::sort / std::min
#include <cmath>                         // std::round

namespace {

// ---------------- 做多信号阈值（固定口径） ----------------
constexpr double kRiskPerTradePct      = 1.0;   // 单笔风险预算（占总资金 %）
constexpr int    kMaxHoldings          = 3;     // 同时持仓上限（集中持仓，不做组合分散）
constexpr double kMaxSinglePositionPct = 20.0;  // 单票仓位上限
constexpr bool   kUseRiskBudgetSizing  = true;  // 单票仓位按「风险预算」而非纯评分线性映射

constexpr double kMinScoreForSignal    = 55.0;  // 做多信号：个股评分门槛
constexpr double kMinRiskReward        = 0.90;  // 盈亏比门槛（保守口径）
constexpr double kMinFillProbability   = 0.25;  // 预估成交概率门槛
// 小资金场景：集中持仓，推荐名单只保留 2 只；其余候选留在候选池作备选观察
constexpr int    kMaxSignalsPerDay     = kConcentratedPositionCount;
constexpr int    kMinThemeRank         = 3;     // 只做同题材身位前 N（1 = 龙头）
constexpr bool   kLeaderOnlyInClimax   = true;  // 高潮期只做题材龙头
constexpr int    kMaxHoldDays          = 2;     // 计划持有上限（T+1 兑现，超期强制离场）

constexpr int    kMaxRiskPenaltyFlags  = 3;     // 风险标记条数上限（E6 风险门）
constexpr double kMaxRiskPctMain       = 5.0;   // 10cm 单笔最大止损空间（%）
constexpr double kMaxRiskPct20cm       = 7.0;   // 20cm 单笔最大止损空间（%）
constexpr double kGradeA               = 75.0;  // A 级分数线
constexpr double kGradeB               = 60.0;  // B 级分数线
constexpr double kRecoveryIndex        = 42.0;  // 回暖线（仅用于结论文案）

// 把阶段窗口渲染成可读文本
QString boardWindowText(const SentimentState& st)
{
    const int maxBoard = SentimentEngine::maxBoardForPhase(st.phase);  // 本阶段最大板数
    if (maxBoard <= 0) return QStringLiteral("当前阶段不新开仓");        // 冰点/退潮
    return QStringLiteral("%1 ≤%2 板").arg(marketPhaseName(st.phase)).arg(maxBoard);
}

} // namespace

// 单票仓位：风险预算制
double LongSignalEngine::positionFor(double totalCapPct, double riskPct, bool leader,
                                     SignalAction action)
{
    if (totalCapPct <= 0.0 || action == SignalAction::None || action == SignalAction::Watch)
        return 0.0;                                    // 无额度 / 无动作 / 仅观察 → 仓位 0

    const double posByCap = totalCapPct / double(qMax(1, kMaxHoldings));  // 按持仓数均摊总仓

    double posByRisk = kMaxSinglePositionPct;          // 默认用单票上限
    if (kUseRiskBudgetSizing && riskPct > 0.05)        // 启用风险预算且有有效止损空间
        posByRisk = kRiskPerTradePct / riskPct * 100.0;  // 预算 / 止损空间

    double pos = std::min({ posByCap, posByRisk, kMaxSinglePositionPct });  // 三大约束取最小

    if (action == SignalAction::Probe) pos *= 0.5;   // 试仓半仓
    if (leader) pos *= 1.2;                          // 同题材身位第一 → 加码

    pos = std::min(pos, kMaxSinglePositionPct);      // 再夹一次上限
    return pos > 0.0 ? pos : 0.0;                      // 负值归零
}

// 信号生成：逐候选跑八道门，再按档位定动作
QVector<LongSignal> LongSignalEngine::generate(const QVector<Candidate>& candidates,
                                               const QHash<QString, KlineSeries>& klines,
                                               const SentimentState& state)
{
    QVector<LongSignal> out;                           // 输出信号
    out.reserve(candidates.size());                    // 预留容量

    const int maxBoard = SentimentEngine::maxBoardForPhase(state.phase);  // 本阶段板数窗口

    for (const Candidate& c : candidates) {            // 逐候选生成
        LongSignal s;                                  // 单条信号
        s.code = c.entry.code;                         // 代码
        s.name = c.entry.name;                         // 名称
        s.score = c.score;                             // 评分
        s.riskBudgetPct = kRiskPerTradePct;            // 单笔风险预算
        s.maxHoldDays = kMaxHoldDays;                  // 持有上限

        const KlineSeries* k = nullptr;                // 该票日 K（可空）
        auto kit = klines.constFind(c.entry.code);     // 查 K 线
        if (kit != klines.constEnd() && kit.value().valid()) k = &kit.value();  // 有效才用

        const TradePlan plan = Plan::build(c, k, state.regime, true);  // 复用交易计划，保证口径一致
        s.riskPct = plan.riskPct;                      // 止损空间
        s.riskReward = plan.riskReward;                // 保守盈亏比
        s.triggerLow = plan.buyLow;                    // 触发价下沿
        s.triggerHigh = plan.buyHigh;                  // 触发价上沿
        s.invalidation = plan.stopLossHigh;            // 失效价

        auto mark = [&s](bool ok, const QString& name, const QString& detail) {
            if (ok) s.conditions << QStringLiteral("通过·%1：%2").arg(name, detail);  // 记通过项
            else    s.blockers   << QStringLiteral("未过·%1：%2").arg(name, detail);  // 记阻塞项
        };

        const bool e1 = state.tradeAllowed && state.tier != SentimentTier::NoTrade;  // E1 情绪门
        mark(e1, QStringLiteral("情绪门"),
             QStringLiteral("%1（回暖线 %2）")
                 .arg(sentimentStatusLabel(state.composite, state.positionCapPct,
                                           state.tradeAllowed))
                 .arg(kRecoveryIndex, 0, 'f', 0));

        const bool e2 = maxBoard > 0 && c.entry.boardCount <= maxBoard;  // E2 高度门
        mark(e2, QStringLiteral("高度门"),
             QStringLiteral("%1 板 vs 允许 %2").arg(c.entry.boardCount).arg(boardWindowText(state)));

        const bool e3 = c.entry.themeLeader                                     // E3 身位门：龙头直接过
                     || (c.entry.themeRank > 0 && c.entry.themeRank <= kMinThemeRank);  // 或名次在前 N
        mark(e3, QStringLiteral("身位门"),
             QStringLiteral("题材「%1」身位 %2（要求 ≤%3，龙头优先）")
                 .arg(c.entry.theme.isEmpty() ? QStringLiteral("未分类") : c.entry.theme)
                 .arg(c.entry.themeRank > 0 ? QString::number(c.entry.themeRank)
                                            : QStringLiteral("未知"))
                 .arg(kMinThemeRank));

        const bool e4 = c.score >= kMinScoreForSignal;   // E4 评分门
        mark(e4, QStringLiteral("评分门"),
             QStringLiteral("评分 %1 ≥ %2").arg(c.score, 0, 'f', 1)
                 .arg(kMinScoreForSignal, 0, 'f', 0));

        const bool e5 = plan.riskReward >= kMinRiskReward;  // E5 盈亏门
        mark(e5, QStringLiteral("盈亏门"),
             QStringLiteral("保守盈亏比 %1 ≥ %2").arg(plan.riskReward, 0, 'f', 2)
                 .arg(kMinRiskReward, 0, 'f', 2));

        const bool e6 = c.riskFlags.size() <= kMaxRiskPenaltyFlags;  // E6 风险门
        mark(e6, QStringLiteral("风险门"),
             QStringLiteral("风险标记 %1 条（上限 %2）")
                 .arg(c.riskFlags.size()).arg(kMaxRiskPenaltyFlags));

        const bool is20cm = (c.entry.board == Board::ChiNext || c.entry.board == Board::StarMarket);  // 是否双创
        const double maxRisk = is20cm ? kMaxRiskPct20cm : kMaxRiskPctMain;                          // 该板块风险上限
        const bool e7 = plan.riskPct > 0.0 && plan.riskPct <= maxRisk + 0.05 && !plan.riskTooHigh;   // E7 结构门
        mark(e7, QStringLiteral("结构门"),
             QStringLiteral("止损空间 %1% ≤ %2%").arg(plan.riskPct, 0, 'f', 2)
                 .arg(maxRisk, 0, 'f', 1));

        // E8 成交门：买不到的计划不该算进当日可执行清单
        const bool e8 = plan.fillProbability >= kMinFillProbability;
        mark(e8, QStringLiteral("成交门"),
             QStringLiteral("预估成交 %1%（门槛 %2%）：%3")
                 .arg(plan.fillProbability * 100, 0, 'f', 0)
                 .arg(kMinFillProbability * 100, 0, 'f', 0)
                 .arg(plan.fillNote));

        bool core = e1 && e2 && e3 && e4 && e5 && e6 && e7 && e8;  // 八门全过才升级
        bool harvestOk = true;                                     // 高潮期龙头门默认通过
        if (state.tier == SentimentTier::Harvest && kLeaderOnlyInClimax && !c.entry.themeLeader) {
            harvestOk = false;                                     // 高潮期非龙头被拦
            s.blockers << QStringLiteral("未过·龙头门：高潮期只做题材龙头，该标的非身位第一");
        }

        if (!core || !harvestOk) {                    // 任一未过 → 观察
            s.action = SignalAction::Watch;
            s.level = 1;
        } else {
            switch (state.tier) {                     // 按档位决定动作强度
            case SentimentTier::Probe:
                s.action = SignalAction::Probe;       // 回暖只试仓
                break;
            case SentimentTier::Build:
                s.action = (c.score >= kGradeB) ? SignalAction::Build : SignalAction::Probe;  // 评分够则建仓
                break;
            case SentimentTier::Attack:
                s.action = (c.entry.themeLeader && c.score >= kGradeA && plan.riskReward >= 1.2)
                               ? SignalAction::Attack : SignalAction::Build;  // 龙头+高分+高盈亏比才主攻
                break;
            case SentimentTier::Harvest:
                s.action = SignalAction::Probe;   // 高潮期：龙头 + 试仓规模，只减不加
                break;
            default:
                s.action = SignalAction::Watch;       // 无档位只观察
                break;
            }
            s.level = (s.action == SignalAction::Attack) ? 4         // 主攻
                    : (s.action == SignalAction::Build)  ? 3         // 建仓
                    : (s.action == SignalAction::Probe)  ? 2 : 1;    // 试仓 / 观察
        }

        if (s.action == SignalAction::Watch) {        // 观察：给出原因，仓位归零
            s.rationale = s.blockers.isEmpty()
                ? QStringLiteral("观察：情绪或结构条件未齐备")
                : QStringLiteral("观察：%1").arg(s.blockers.first());
            s.positionPct = 0.0;
        } else {
            s.fillProbability = plan.fillProbability; // 带上成交概率
            s.rationale = QStringLiteral("%1：%2 板 · %3 · 评分 %4 · 盈亏比 %5 · 成交 %6% · 触发 %7~%8 · 失效 %9")
                              .arg(signalActionName(s.action))
                              .arg(c.entry.boardCount)
                              .arg(c.entry.themeLeader ? QStringLiteral("题材龙头")
                                                       : QStringLiteral("身位第 %1").arg(c.entry.themeRank))
                              .arg(c.score, 0, 'f', 1)
                              .arg(plan.riskReward, 0, 'f', 2)
                              .arg(plan.fillProbability * 100, 0, 'f', 0)
                              .arg(plan.buyLow, 0, 'f', 2)
                              .arg(plan.buyHigh, 0, 'f', 2)
                              .arg(plan.stopLossHigh, 0, 'f', 2);

            const double raw = positionFor(state.positionCapPct, plan.riskPct,   // 理论仓位
                                           c.entry.themeLeader, s.action);
            s.positionPct = raw * plan.fillProbability;   // 期望仓位 = 可成交仓位 × 成交概率
            s.riskBudgetPct = kRiskPerTradePct;           // 记录风险预算
        }
        out.append(s);                                // 收集信号
    }
    return out;                                       // 返回全部信号
}

// 把信号写回计划，并做组合层额度分配
void LongSignalEngine::apply(RecommendBundle& bundle,
                             const QVector<LongSignal>& signalList,
                             const SentimentState& state)
{
    bundle.positionCapPct = state.positionCapPct;   // UI 顶部「建议总仓位」以情绪上限为准

    QHash<QString, LongSignal> idx;                   // 代码 → 信号索引
    idx.reserve(signalList.size());                   // 预留容量
    for (const LongSignal& s : signalList) idx.insert(s.code, s);  // 建索引

    QVector<int> order;                               // 分配顺序（下标）
    order.reserve(bundle.plans.size());               // 预留容量
    for (int i = 0; i < bundle.plans.size(); ++i) order.append(i);  // 初始顺序
    std::sort(order.begin(), order.end(), [&bundle, &idx](int a, int b) {
        const LongSignal sa = idx.value(bundle.plans.at(a).code);  // a 的信号
        const LongSignal sb = idx.value(bundle.plans.at(b).code);  // b 的信号
        if (sa.level != sb.level) return sa.level > sb.level;      // 动作等级降序优先
        return sa.score > sb.score;                                // 同级按评分降序
    });

    double usedCap = 0.0;                             // 已用仓位
    int taken = 0;                                    // 已占用信号名额
    QSet<QString> usedThemes;                         // 已占用题材

    for (int pi : order) {                            // 按顺序分配
        TradePlan& p = bundle.plans[pi];              // 当前计划
        p.riskBudgetPct = kRiskPerTradePct;           // 写入风险预算
        p.sentimentNote = sentimentStatusLabel(state.composite, state.positionCapPct,
                                               state.tradeAllowed);

        auto it = idx.constFind(p.code);              // 查该计划是否有信号
        if (it == idx.constEnd()) {                   // 未参与信号生成（如近失名单）
            if (!state.tradeAllowed) {                // 禁止开仓时统一归零
                p.actionable = false;
                p.positionPct = 0.0;
            }
            continue;                                 // 否则保持原判
        }
        const LongSignal& sig = it.value();           // 该计划对应信号
        p.signalLevel = sig.level;                    // 回写信号等级

        if (!state.tradeAllowed || sig.action == SignalAction::Watch) {  // 禁止开仓或信号不足
            const bool wasActionable = p.actionable;  // 记录原可执行状态
            p.actionable = false;                     // 降级为观察
            p.positionPct = 0.0;                      // 仓位归零
            if (wasActionable || p.note.isEmpty() || p.note == QStringLiteral("可执行")) {
                p.note = sig.rationale.isEmpty()
                             ? QStringLiteral("观察：情绪信号未触发")
                             : sig.rationale;         // 更新说明文案
            }
            continue;                                 // 下一只
        }

        if (p.note.startsWith(QStringLiteral("竞价否决"), Qt::CaseInsensitive)   // 竞价否决
            || p.note.startsWith(QStringLiteral("结构止损过远"))) {              // 结构止损否决
            p.actionable = false;                     // 不得因情绪好转而"复活"
            p.positionPct = 0.0;
            continue;                                 // 下一只
        }

        QString block;                                // 组合层拦截原因
        if (taken >= kMaxSignalsPerDay) {             // 名额已满
            block = QStringLiteral("当日信号名额已满（%1 只）").arg(kMaxSignalsPerDay);
        } else if (taken >= kMaxHoldings) {           // 持仓数已满
            block = QStringLiteral("持仓数已达上限（%1 只）").arg(kMaxHoldings);
        } else if (!p.theme.isEmpty() && usedThemes.contains(p.theme)) {  // 同题材重复
            block = QStringLiteral("题材「%1」已有一只信号，避免同题材双杀").arg(p.theme);
        }
        if (!block.isEmpty()) {                       // 被拦截 → 观察
            p.actionable = false;
            p.positionPct = 0.0;
            p.note = QStringLiteral("观察：%1").arg(block);
            continue;                                 // 下一只
        }

        double pos = positionFor(state.positionCapPct, sig.riskPct,     // 与 generate() 同口径
                                 p.themeLeader, sig.action)
                   * sig.fillProbability;              // 期望仓位
        const double remain = state.positionCapPct - usedCap;  // 剩余额度
        if (pos > remain) pos = remain;                // 不超过剩余额度
        if (pos < 1.0) {                               // 额度过小则无可执行意义
            p.actionable = false;
            p.positionPct = 0.0;
            p.note = QStringLiteral("观察：剩余仓位额度不足（已用 %1% / 上限 %2%）")
                         .arg(usedCap, 0, 'f', 0).arg(state.positionCapPct, 0, 'f', 0);
            continue;                                  // 下一只
        }

        p.actionable = true;                           // 可执行
        p.positionPct = std::round(pos * 10.0) / 10.0; // 仓位保留一位小数
        p.note = QStringLiteral("[%1] %2 ｜ 风险预算 %3%（止损空间 %4%）｜ 成交 %5%")
                     .arg(signalActionName(sig.action), sig.rationale)   // 动作 + 理由
                     .arg(kRiskPerTradePct, 0, 'f', 1)                   // 风险预算
                     .arg(sig.riskPct, 0, 'f', 2)                        // 止损空间
                     .arg(sig.fillProbability * 100, 0, 'f', 0);         // 成交概率

        usedCap += p.positionPct;                      // 累计已用仓位
        ++taken;                                       // 占用一个名额
        if (!p.theme.isEmpty()) usedThemes.insert(p.theme);  // 记录已用题材
    }

    const QString prefix = sentimentStatusLabel(state.composite, state.positionCapPct,
                                                state.tradeAllowed);

    bundle.notice = prefix + QStringLiteral(" ｜ ") + bundle.notice;  // 前置情绪结论

    if (!state.tradeAllowed) {                         // 明确「情绪允许 ≠ 可以买」
        bundle.notice += QStringLiteral("｜信号层：禁止开仓，清单仅供跟踪");
    } else if (taken == 0) {
        bundle.notice += QStringLiteral("｜信号层：0 只可执行（个股评分/高度/盈亏比未同时达标）");
    } else {
        bundle.notice += QStringLiteral("｜信号层：%1 只可执行，合计仓位 %2%")
                             .arg(taken)
                             .arg(usedCap, 0, 'f', 1);
    }
}
