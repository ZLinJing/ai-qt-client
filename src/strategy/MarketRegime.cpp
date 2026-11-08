#include "strategy/MarketRegime.h"       // 本模块声明
#include <QSet>                          // 今日涨停代码集合（判晋级）

namespace {                              // 情绪闸门阈值（固定口径）

constexpr int    kMinLimitUpCount        = 30;    // 涨停家数下限（低于则视为冰点/退潮）
constexpr int    kMaxLimitDownCount      = 8;     // 跌停家数上限（超过则高位接力风险极大）
constexpr double kMaxBrokenRate          = 0.40;  // 炸板率停手线（>40% 直接禁止出手）
constexpr double kWatchBrokenRate        = 0.30;  // 炸板率降仓提示线
constexpr int    kMinMaxBoardHeight      = 2;     // 最低空间板高度
constexpr double kMinAdvanceRate         = 0.15;  // 最低连板晋级率
constexpr double kMinPrevLimitUpPremium  = 0.0;   // 昨日涨停股今日平均涨幅下限（%）

constexpr int    kStrongLimitUpCount     = 60;    // 强市：涨停家数门槛
constexpr int    kHealthyLimitUpCount    = 40;    // 健康：涨停家数门槛
constexpr int    kWeakLimitUpCount       = 30;    // 弱市：涨停家数门槛
constexpr int    kClimaxHeight           = 5;     // 高潮期要求的最低高度
constexpr double kHealthyBrokenRate      = 0.25;  // 健康区间炸板率
constexpr double kRecoveryBrokenRate     = 0.35;  // 回暖区间炸板率

} // namespace

// 情绪阶段 → 中文名
QString marketPhaseName(MarketPhase p)
{
    switch (p) {                         // 逐一映射
    case MarketPhase::IcePoint: return QStringLiteral("冰点期");   // 空仓
    case MarketPhase::Recovery:  return QStringLiteral("回暖期");   // 试仓
    case MarketPhase::MainUp:    return QStringLiteral("主升期");   // 主攻
    case MarketPhase::Climax:    return QStringLiteral("高潮期");   // 减仓
    case MarketPhase::Retreat:   return QStringLiteral("退潮期");   // 空仓
    default:                     return QStringLiteral("未知");     // 数据不足
    }
}

// 情绪阶段 → 操作建议文案
QString marketPhaseAdvice(MarketPhase p)
{
    switch (p) {                         // 逐一映射
    case MarketPhase::IcePoint:
        return QStringLiteral("空仓。仅记录首板进观察池，不参与接力。");        // 冰点只记录
    case MarketPhase::Recovery:
        return QStringLiteral("小仓位（1-2 成）试错一进二，严格小盘小仓位。");  // 回暖小仓
    case MarketPhase::MainUp:
        return QStringLiteral("主攻二进三 / 三进四，仓位可提至 5-7 成。");      // 主升主攻
    case MarketPhase::Climax:
        return QStringLiteral("减仓。只做龙头，不碰中位股，防范分歧加剧。");    // 高潮减仓
    case MarketPhase::Retreat:
        return QStringLiteral("停止高位接力，坚决空仓。");                      // 退潮空仓
    default:
        return QStringLiteral("数据不足，建议观望。");                          // 未知观望
    }
}

RegimeEngine::RegimeEngine(QObject* parent) : QObject(parent) {}  // 构造：无需额外初始化

// 基础指标：由当日三池直接统计家数、高度与炸板率
void RegimeEngine::computeBase(MarketRegime& reg,
                                const QVector<LimitEntry>& limitUp,
                                const QVector<LimitEntry>& broken,
                                const QVector<LimitEntry>& limitDown,
                                const QDate& tradeDate)
{
    reg.limitUpCount   = limitUp.size();         // 涨停家数
    reg.brokenCount    = broken.size();          // 炸板家数
    reg.limitDownCount = limitDown.size();       // 跌停家数

    int first = 0, multi = 0, maxH = 0;          // 首板数 / 连板数 / 最高连板
    for (const LimitEntry& e : limitUp) {        // 遍历涨停池
        if (e.boardCount <= 1) ++first; else ++multi;   // 按连板数分类计数
        maxH = qMax(maxH, e.boardCount);         // 更新最高连板
    }
    reg.firstBoardCount = first;                 // 写入首板数
    reg.multiBoardCount = multi;                 // 写入连板数
    reg.maxBoardHeight = maxH;                   // 写入最高高度

    const int touched = reg.limitUpCount + reg.brokenCount;  // 触板总数
    reg.brokenRate = touched > 0 ? double(reg.brokenCount) / double(touched) : 0.0;  // 炸板率
    reg.date = tradeDate.isValid() ? tradeDate : QDate::currentDate();  // 交易日以传入值为准
}

// 叠加昨日数据：晋级率与溢价率（先行指标）
void RegimeEngine::applyYesterday(MarketRegime& reg,
                                  const QVector<LimitEntry>& prevLimitUp,
                                  const QVector<LimitEntry>& todayLimitUp,
                                  const QHash<QString, Snapshot>& quotes)
{
    if (prevLimitUp.isEmpty()) return;           // 无昨日数据则不做任何计算

    QSet<QString> todaySet;                      // 今日涨停代码集合
    for (const LimitEntry& e : todayLimitUp) todaySet.insert(e.code);  // 填充集合

    int prevMulti = 0, prevMultiAdvanced = 0;   // 昨日连板数 / 其中今日继续涨停数
    int prevFirst = 0, prevFirstAdvanced = 0;   // 昨日首板数 / 其中今日继续涨停数
    double pctSum = 0.0;                         // 今日涨幅累加
    int pctN = 0;                                // 有效采样数

    for (const LimitEntry& e : prevLimitUp) {    // 遍历昨日涨停池
        const bool stillLimit = todaySet.contains(e.code);  // 今日是否仍涨停
        if (e.boardCount >= 2) {                 // 昨日为连板
            ++prevMulti;                         // 连板基数 +1
            if (stillLimit) ++prevMultiAdvanced; // 晋级成功 +1
        } else {                                 // 昨日为首板
            ++prevFirst;                         // 首板基数 +1
            if (stillLimit) ++prevFirstAdvanced; // 晋级成功 +1
        }

        // 溢价采样：优先用昨日池自带的今日涨跌幅（getYesterdayZTPool），回退今日快照
        if (e.hasTodayPct) {                     // 池条目自带今日涨跌幅
            pctSum += e.changePct;               // 直接累加
            ++pctN;                              // 采样数 +1
            continue;                            // 无需查快照
        }
        auto it = quotes.constFind(e.code);      // 回退：查该股今日快照
        if (it != quotes.constEnd() && it->isValid()) {  // 快照有效才计入溢价
            pctSum += it->changePct;             // 累加今日涨幅
            ++pctN;                              // 采样数 +1
        }
    }

    reg.prevLimitUpCount = prevLimitUp.size();   // 昨日涨停家数
    reg.advanceRate = prevMulti > 0 ? double(prevMultiAdvanced) / double(prevMulti) : 0.0;      // 连板晋级率
    reg.firstBoardAdvanceRate = prevFirst > 0 ? double(prevFirstAdvanced) / double(prevFirst) : 0.0;  // 首板晋级率
    reg.prevLimitUpAvgPct = pctN > 0 ? pctSum / double(pctN) : 0.0;  // 昨日涨停股今日平均涨幅
    reg.premiumSampleCount = pctN;               // 实际采样数
    reg.premiumKnown = !prevLimitUp.isEmpty() && pctN * 2 >= prevLimitUp.size();  // 覆盖率过半才可信
    reg.yesterdayKnown = prevMulti > 0 || prevFirst > 0;  // 昨日数据是否可用
}

// 核心判定：硬否决 → 阶段判定 → 先行指标修正 → 生成结论文案
void RegimeEngine::evaluateOn(MarketRegime& r)
{
    QStringList reasons;                         // 判定理由集合

    bool veto = false;                           // 是否触发硬否决
    if (r.limitDownCount >= kMaxLimitDownCount) {   // 跌停家数超限
        veto = true;                             // 禁止出手
        reasons << QStringLiteral("跌停 %1 家(≥%2)，高位接力风险极大")
                       .arg(r.limitDownCount).arg(kMaxLimitDownCount);
    }
    if (r.brokenRate > kMaxBrokenRate) {         // 炸板率超停手线
        veto = true;                             // 禁止出手
        reasons << QStringLiteral("炸板率 %1%(>%2%)，接力意愿极差")
                       .arg(r.brokenRate * 100, 0, 'f', 1)
                       .arg(kMaxBrokenRate * 100, 0, 'f', 0);
    } else if (r.brokenRate > kWatchBrokenRate) {   // 超过提示线只降仓
        reasons << QStringLiteral("炸板率 %1%(>%2%)，需降仓")
                       .arg(r.brokenRate * 100, 0, 'f', 1)
                       .arg(kWatchBrokenRate * 100, 0, 'f', 0);
    }
    if (r.limitUpCount < kMinLimitUpCount) {     // 涨停家数不足
        reasons << QStringLiteral("涨停仅 %1 家(<%2)，赚钱效应弱")
                    .arg(r.limitUpCount).arg(kMinLimitUpCount);
    }
    if (r.maxBoardHeight < kMinMaxBoardHeight && r.maxBoardHeight < 3) {  // 空间高度不足
        reasons << QStringLiteral("最高板仅 %1 板，空间不足").arg(r.maxBoardHeight);
    }

    MarketPhase phase = MarketPhase::Unknown;    // 待判定阶段
    const bool strong  = r.limitUpCount >= kStrongLimitUpCount;    // 强市阈值
    const bool healthy = r.limitUpCount >= kHealthyLimitUpCount;   // 健康阈值
    const bool weak    = r.limitUpCount < kWeakLimitUpCount;       // 弱势阈值

    if (r.limitDownCount >= kMaxLimitDownCount || r.brokenRate > kMaxBrokenRate) {
        phase = MarketPhase::Retreat;            // 跌停多或炸板率高 → 退潮
    } else if (weak && r.brokenRate > kWatchBrokenRate) {
        phase = MarketPhase::IcePoint;           // 弱势 + 高炸板 → 冰点
    } else if (strong && r.brokenRate < kWatchBrokenRate) {
        phase = (r.maxBoardHeight >= kClimaxHeight) ? MarketPhase::Climax : MarketPhase::MainUp;  // 涨停潮：高度够则高潮，否则主升
    } else if (healthy && r.brokenRate < kHealthyBrokenRate) {
        phase = MarketPhase::MainUp;             // 健康区间 → 主升
    } else if (r.limitUpCount >= kWeakLimitUpCount && r.brokenRate < kRecoveryBrokenRate) {
        phase = MarketPhase::Recovery;           // 温和区间 → 回暖
    } else if (weak) {
        phase = MarketPhase::IcePoint;           // 弱势 → 冰点
    } else {
        phase = MarketPhase::Recovery;           // 其余兜底为回暖
    }

    if (r.yesterdayKnown && r.advanceRate > 0.0
        && r.advanceRate < kMinAdvanceRate && phase == MarketPhase::MainUp) {  // 晋级率过低
        phase = MarketPhase::Recovery;           // 主升降级为回暖
        reasons << QStringLiteral("连板晋级率 %1%(<%2%)，接力生态差")
                       .arg(r.advanceRate * 100, 0, 'f', 1)
                       .arg(kMinAdvanceRate * 100, 0, 'f', 0);
    }
    if (r.premiumKnown && r.prevLimitUpAvgPct < kMinPrevLimitUpPremium
        && phase == MarketPhase::MainUp) {       // 溢价为负说明打板即亏
        phase = MarketPhase::Recovery;           // 主升降级为回暖
        reasons << QStringLiteral("昨日涨停今日平均 %1%，溢价为负（打板即亏），降级")
                       .arg(r.prevLimitUpAvgPct, 0, 'f', 2);
    }
    if (r.premiumKnown && r.prevLimitUpAvgPct < -3.0
        && r.brokenRate > kWatchBrokenRate
        && phase != MarketPhase::Retreat) {      // 溢价极差 + 炸板偏高
        phase = MarketPhase::Retreat;            // 直接判退潮
        reasons << QStringLiteral("昨日涨停今日平均 %1%，亏钱效应扩散，禁止出手")
                       .arg(r.prevLimitUpAvgPct, 0, 'f', 2);
    }

    r.phase = phase;                             // 写入最终阶段
    r.tradeAllowed = !veto                       // 未被否决
                  && phase != MarketPhase::IcePoint && phase != MarketPhase::Retreat;  // 且非空仓阶段

    QString lead;                                // 先行指标说明
    if (r.yesterdayKnown) {                      // 昨日数据可用
        const QString premiumText = r.premiumKnown              // 溢价可信则给具体数值
            ? QStringLiteral("%1%（采样 %2/%3）")
                  .arg(r.prevLimitUpAvgPct, 0, 'f', 2)
                  .arg(r.premiumSampleCount).arg(r.prevLimitUpCount)
            : QStringLiteral("样本不足（%1/%2，快照未就绪）")     // 否则说明样本不足
                  .arg(r.premiumSampleCount).arg(r.prevLimitUpCount);
        lead = QStringLiteral("先行指标：昨日涨停 %1 家｜连板晋级率 %2%｜首板晋级率 %3%｜今日溢价 %4")
                   .arg(r.prevLimitUpCount)
                   .arg(r.advanceRate * 100, 0, 'f', 1)
                   .arg(r.firstBoardAdvanceRate * 100, 0, 'f', 1)
                   .arg(premiumText);
    } else {
        lead = QStringLiteral("先行指标：昨日涨停池未就绪（仅按当日统计判定）");  // 未就绪提示
    }

    if (veto) {                                  // 有硬否决 → 结论为禁止出手
        r.note = QStringLiteral("【禁止出手】") + reasons.join(QStringLiteral("；")) + QStringLiteral(" ｜ ") + lead;
    } else {                                     // 正常放行 → 输出阶段与理由
        r.note = QStringLiteral("【%1】%2 | %3 ｜ %4")
                    .arg(marketPhaseName(phase), marketPhaseAdvice(phase),
                         reasons.isEmpty() ? QStringLiteral("情绪指标达标") : reasons.join(QStringLiteral("；")),
                         lead);
    }
}

// 阶段 → 建议总仓（0~1）
double RegimeEngine::suggestedPosition() const
{
    switch (m_regime.phase) {                    // 按阶段给仓位
    case MarketPhase::MainUp:   return 0.60;     // 主升 60%
    case MarketPhase::Recovery: return 0.20;     // 回暖 20%
    case MarketPhase::Climax:   return 0.30;     // 高潮 30%
    case MarketPhase::IcePoint: return 0.0;      // 冰点空仓
    case MarketPhase::Retreat:  return 0.0;      // 退潮空仓
    default:                    return 0.0;      // 未知空仓
    }
}

// 对内部成员执行判定
void RegimeEngine::evaluate()
{
    evaluateOn(m_regime);                        // 复用静态实现
}

// 阶段 → 仓位说明文案
QString RegimeEngine::positionAdvice(MarketPhase p)
{
    switch (p) {                                 // 按阶段给文案
    case MarketPhase::MainUp:   return QStringLiteral("总仓位上限 60%");
    case MarketPhase::Recovery: return QStringLiteral("总仓位上限 20%");
    case MarketPhase::Climax:   return QStringLiteral("总仓位上限 30%（只做龙头）");
    case MarketPhase::IcePoint:
    case MarketPhase::Retreat:  return QStringLiteral("总仓位 0%，空仓");   // 两者都空仓
    default:                    return QStringLiteral("建议观望");
    }
}
