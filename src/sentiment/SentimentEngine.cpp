#include "sentiment/SentimentEngine.h"   // 本模块声明
#include <QSet>                          // 今日涨停代码集合
#include <algorithm>                     // std::nth_element
#include <cmath>                         // 数学函数

// 实现要点：① 用固定上下界线性归一而非滚动 z-score；② 硬否决优先于加权指数；
// ③ 数据覆盖不足时分项降权 + 全局降级减仓，宁可少赚也不用残样本算假繁荣
namespace {

inline double clampD(double v, double lo, double hi)   // 数值夹紧到 [lo, hi]
{
    return v < lo ? lo : (v > hi ? hi : v);            // 三目实现，避免引入额外依赖
}

// 把 x 从 [lo, hi] 线性映射到 [0, 100]（超出区间截断）
inline double scaleTo(double x, double lo, double hi)
{
    if (!(hi > lo)) return 50.0;                       // 区间非法时给中性分
    return clampD((x - lo) / (hi - lo), 0.0, 1.0) * 100.0;  // 归一化后放大到 100
}

inline double safeDiv(double a, double b)              // 安全除法：分母为 0 返回 0
{
    return b > 0.0 ? a / b : 0.0;                      // 避免除零
}

// 取中位数（允许打乱入参副本）
double medianOf(QVector<double> v)
{
    if (v.isEmpty()) return 0.0;                       // 空序列返回 0
    const int mid = v.size() / 2;                      // 中间下标
    std::nth_element(v.begin(), v.begin() + mid, v.end());  // 部分排序定位中位
    double m = v.at(mid);                              // 奇数个直接取该值
    if (v.size() % 2 == 0) {                           // 偶数个需取两数均值
        std::nth_element(v.begin(), v.begin() + mid - 1, v.end());  // 定位前一个
        m = (m + v.at(mid - 1)) * 0.5;                 // 两者取平均
    }
    return m;                                          // 返回中位数
}

// 判定快照是否与目标交易日对齐：当日涨停股在快照中应普遍大涨
bool quotesAlignedWith(const QDate& date, const QVector<LimitEntry>& limitUp,
                       const QHash<QString, Snapshot>& quotes)
{
    if (quotes.isEmpty()) return false;                // 无快照无法对齐
    if (limitUp.isEmpty()) return date == QDate::currentDate();  // 无涨停池时只能按日期判断

    int checked = 0, strong = 0;                       // 已检查数 / 明显上涨数
    for (const LimitEntry& e : limitUp) {              // 抽样检查涨停股
        auto it = quotes.constFind(e.code);            // 查该股快照
        if (it == quotes.constEnd() || !it->isValid()) continue;  // 无有效快照则跳过
        ++checked;                                     // 计入检查数
        if (it->changePct > 5.0) ++strong;             // 涨幅 >5% 视为吻合
        if (checked >= 60) break;                      // 最多检查 60 只，够用即可
    }
    if (checked < qMin(5, limitUp.size())) return false;  // 样本太少无法判断
    return strong * 2 >= checked;                      // 半数以上吻合才算对齐
}

// ---------------- 情绪量化阈值（固定口径） ----------------
// 六维权重
constexpr double kWProfitEffect = 0.30;   // S1 赚钱效应（先行）
constexpr double kWSealQuality  = 0.20;   // S2 封板质量
constexpr double kWHeight       = 0.15;   // S3 空间高度
constexpr double kWBreadth      = 0.15;   // S4 市场宽度
constexpr double kWDivergence   = 0.10;   // S5 分歧收敛（反向计分）
constexpr double kWThemeFocus   = 0.10;   // S6 主线集中度

// 各分项的固定映射区间
constexpr double kPremiumFloor      = -5.0;  // 昨日涨停今日溢价 %（下界 → 0 分）
constexpr double kPremiumCeil       =  5.0;  // 上界 → 100 分
constexpr double kAdvanceFloor      = 0.10;  // 连板晋级率下界
constexpr double kAdvanceCeil       = 0.55;  // 连板晋级率上界
constexpr double kFirstAdvanceFloor = 0.08;  // 首板晋级率下界
constexpr double kFirstAdvanceCeil  = 0.40;  // 首板晋级率上界
constexpr double kBrokenRateBest    = 0.15;  // 炸板率 15% 及以下 = 100 分
constexpr double kBrokenRateWorst   = 0.65;  // 65% 及以上 = 0 分
constexpr double kSealRatioFloor    = 0.05;  // 平均封单/成交额下界
constexpr double kSealRatioCeil     = 0.35;  // 平均封单/成交额上界
constexpr double kEarlySealFloor    = 0.20;  // 10:30 前封板占比下界
constexpr double kEarlySealCeil     = 0.90;  // 10:30 前封板占比上界
constexpr double kHeightFloor       = 2.0;   // 最高板下界
constexpr double kHeightCeil        = 6.0;   // 最高板上界
constexpr double kUpRatioFloor      = 0.25;  // 全市场上涨家数占比下界
constexpr double kUpRatioCeil       = 0.75;  // 全市场上涨家数占比上界
constexpr double kMedianFloor       = -2.0;  // 全市场涨跌幅中位数下界 %
constexpr double kMedianCeil        =  2.0;  // 全市场中位数上界 %
constexpr double kThemeFocusFloor   = 0.10;  // 最强题材涨停家数 / 总涨停家数 下界
constexpr double kThemeFocusCeil    = 0.50;  // 同上上界

// 指数分档
constexpr double kIcePointIndex  = 25.0;  // < 25  冰点（空仓）
constexpr double kRecoveryIndex  = 42.0;  // 25~42 回暖（试仓 20%）
constexpr double kMainUpIndex    = 58.0;  // 42~58 主升（建仓 / 主攻）
constexpr double kClimaxIndex    = 78.0;  // ≥78   高潮（只减不加）

constexpr double kRetreatDrop        = 12.0;  // 指数低于回暖线且较 5 日均值回落 ≥ 该值 → 退潮
constexpr double kClimaxAdvanceWeak  = 0.30;  // 高潮期连板晋级率低于该值 → 高位分歧
constexpr int    kClimaxHeight       = 5;     // 触发「高位分歧」判定的最低空间板

constexpr double kBreadthDivergenceRatio = 0.30;  // 指数与宽度背离：上涨占比低于该值视为背离
constexpr double kBreadthDivergenceScale = 0.60;  // 背离时仓位上限折减系数

constexpr double kPremiumSurvivorPct     = 9.0;   // 溢价幸存者偏差：溢价 ≥ 该值
constexpr double kPremiumSurvivorUpRatio = 0.35;  // 且上涨占比 < 该值 → 按下调权重处理

// 档位仓位上限
constexpr double kCapProbe          = 20.0;  // 低强度档位
constexpr double kCapBuild          = 42.0;  // 中强度档位
constexpr double kCapAttack         = 60.0;  // 高强度档位
constexpr double kCapHarvest        = 30.0;  // 高潮期（只做龙头，只减不加）
constexpr double kDegradedCapScale  = 0.80;  // 数据缺失降级时的仓位折减

// 阶段允许参与的最大板数
constexpr int kMaxBoardRecovery = 2;  // 回暖期
constexpr int kMaxBoardMainUp   = 4;  // 主升期
constexpr int kMaxBoardClimax   = 3;  // 高潮期（只做低位补涨 / 龙头）

// 硬否决与市场宽度共用阈值
constexpr int    kWeakLimitUpCount    = 30;    // 弱市涨停家数门槛
constexpr int    kStrongLimitUpCount  = 60;    // 强市涨停家数门槛
constexpr int    kMaxLimitDownCount   = 8;     // 跌停家数上限
constexpr double kMaxBrokenRate       = 0.40;  // 炸板率停手线
constexpr double kWatchBrokenRate     = 0.30;  // 炸板率降仓提示线

} // namespace

// 对外统一状态标签：不出现阶段名 / 档位名，只保留可核对的数字口径
QString sentimentStatusLabel(double composite, double capPct, bool tradeAllowed)
{
    return QStringLiteral("复合情绪 %1 ｜ 有效总仓 ≤%2% ｜ 闸门 %3")
        .arg(composite, 0, 'f', 1)
        .arg(capPct, 0, 'f', 0)
        .arg(tradeAllowed ? QStringLiteral("放行") : QStringLiteral("拦截"));
}

// 档位 → 中文名
QString sentimentTierName(SentimentTier t)
{
    switch (t) {                                       // 逐一映射
    case SentimentTier::Probe:   return QStringLiteral("试仓");
    case SentimentTier::Build:   return QStringLiteral("建仓");
    case SentimentTier::Attack:  return QStringLiteral("主攻");
    case SentimentTier::Harvest: return QStringLiteral("兑现");
    default:                     return QStringLiteral("禁止开仓");
    }
}

// 档位 → 操作建议
QString sentimentTierAdvice(SentimentTier t)
{
    switch (t) {                                       // 逐一映射
    case SentimentTier::Probe:
        return QStringLiteral("只做低位首板/一进二，单笔试仓，盈亏比不足不接。");
    case SentimentTier::Build:
        return QStringLiteral("主线题材内正常建仓，回避后排跟风。");
    case SentimentTier::Attack:
        return QStringLiteral("情绪与主线共振，可打龙头与强二板，仍按风险预算控单票。");
    case SentimentTier::Harvest:
        return QStringLiteral("只做题材龙头，只减不加，兑现为主。");
    default:
        return QStringLiteral("空仓等待，不参与接力。");
    }
}

// 动作 → 中文名
QString signalActionName(SignalAction a)
{
    switch (a) {                                       // 逐一映射
    case SignalAction::Watch:  return QStringLiteral("观察");
    case SignalAction::Probe:  return QStringLiteral("试仓");
    case SignalAction::Build:  return QStringLiteral("建仓");
    case SignalAction::Attack: return QStringLiteral("主攻");
    default:                   return QStringLiteral("无信号");
    }
}

// 采集层：把既有数据源统计成原始事实
SentimentFact SentimentEngine::collect(const QDate& date,
                                       const QVector<LimitEntry>& limitUp,
                                       const QVector<LimitEntry>& broken,
                                       const QVector<LimitEntry>& limitDown,
                                       const QVector<LimitEntry>& prevLimitUp,
                                       const QHash<QString, Snapshot>& quotes,
                                       const QHash<QString, QStringList>& themesOfStock)
{
    SentimentFact f;                                   // 待填充事实
    f.date = date;                                     // 归属交易日
    f.limitUpCount = limitUp.size();                   // 涨停家数
    f.brokenCount = broken.size();                     // 炸板家数
    f.limitDownCount = limitDown.size();               // 跌停家数

    double sealAmtSum = 0.0, sealCapSum = 0.0, turnoverSum = 0.0, breakSum = 0.0;  // 各指标累加器
    int sealFirstN = 0, sealLateN = 0;                 // 早封 / 尾盘封 计数

    for (const LimitEntry& e : limitUp) {              // 遍历涨停池统计封板质量
        if (e.boardCount <= 1) ++f.firstBoardCount; else ++f.multiBoardCount;  // 首板/连板分类
        if (e.boardCount >= 3) ++f.highBoardCount;     // 高位板计数
        f.maxBoardHeight = qMax(f.maxBoardHeight, e.boardCount);  // 更新最高板

        if (e.isOneWordBoard) ++f.oneWordCount;        // 一字板计数
        if (e.isSecondSeal)   ++f.secondSealCount;     // 秒板计数

        sealAmtSum += e.sealToAmountRatio();           // 封单/成交额 累加
        sealCapSum += e.sealToFloatCapRatio();         // 封单/流通市值 累加
        turnoverSum += e.turnover;                     // 换手率累加
        breakSum += e.breakCount;                      // 开板次数累加

        if (e.firstSealTime.isValid()) {               // 有首封时间才判断早迟
            const int m = e.firstSealTime.hour() * 60 + e.firstSealTime.minute();  // 换算分钟
            if (m <= 10 * 60 + 30) ++sealFirstN;       // 10:30 前封板
            if (m >= 14 * 60) ++sealLateN;             // 14:00 后封板
        }
    }
    f.sealBefore1030 = sealFirstN;                     // 早封家数
    f.sealAfter1400  = sealLateN;                      // 尾盘封板家数
    f.avgSealToAmount = safeDiv(sealAmtSum, f.limitUpCount);  // 平均封单/成交额
    f.avgSealToCap    = safeDiv(sealCapSum, f.limitUpCount);  // 平均封单/市值
    f.avgTurnover     = safeDiv(turnoverSum, f.limitUpCount); // 平均换手
    f.avgBreakCount   = safeDiv(breakSum, f.limitUpCount);    // 平均开板次数

    const int touched = f.limitUpCount + f.brokenCount;  // 触板总数
    f.brokenRate = safeDiv(f.brokenCount, touched);      // 炸板率

    QSet<QString> todaySet;                            // 今日涨停代码集合
    todaySet.reserve(f.limitUpCount);                  // 预留容量
    for (const LimitEntry& e : limitUp) todaySet.insert(e.code);  // 填充集合

    if (!prevLimitUp.isEmpty()) {                      // 有昨日池才能算晋级率
        f.prevLimitUpCount = prevLimitUp.size();       // 昨日涨停家数
        int prevMulti = 0, prevMultiOk = 0;            // 昨日连板数 / 晋级数
        int prevFirst = 0, prevFirstOk = 0;            // 昨日首板数 / 晋级数
        for (const LimitEntry& e : prevLimitUp) {      // 遍历昨日池
            const bool still = todaySet.contains(e.code);  // 今日是否仍涨停
            if (e.boardCount >= 2) { ++prevMulti; if (still) ++prevMultiOk; }  // 连板口径
            else                   { ++prevFirst; if (still) ++prevFirstOk; }  // 首板口径
        }
        f.advanceRate           = safeDiv(prevMultiOk, prevMulti);   // 连板晋级率
        f.firstBoardAdvanceRate = safeDiv(prevFirstOk, prevFirst);   // 首板晋级率
    }

    f.quotesAligned = quotesAlignedWith(date, limitUp, quotes);  // 快照是否与本交易日对齐

    if (!prevLimitUp.isEmpty()) {                      // 有昨日池才考虑溢价
        // 溢价样本来源优先级：① 昨日池自带今日涨跌幅（东财 getYesterdayZTPool，不依赖快照）
        //                    ② 新浪快照今日涨跌幅（历史模式回退时使用）
        double sum = 0.0;                              // 涨幅累加
        int n = 0;                                     // 采样数
        for (const LimitEntry& e : prevLimitUp) {      // 遍历昨日涨停股
            if (e.hasTodayPct) {                       // 池条目自带今日涨跌幅
                sum += e.changePct;                    // 直接累加（zdp 已是百分数）
                ++n;                                   // 采样数 +1
                continue;                              // 无需查快照
            }
            auto it = quotes.constFind(e.code);        // 回退：查今日快照
            if (it == quotes.constEnd() || !it->isValid()) continue;  // 无有效快照跳过
            sum += it->changePct;                      // 累加今日涨幅
            ++n;                                       // 采样数 +1
        }
        f.premiumSamples = n;                          // 实际采样数
        f.premiumPct = safeDiv(sum, n);                // 平均溢价
        f.premiumKnown = (n * 2 >= prevLimitUp.size());  // 覆盖过半才可信
    }

    if (f.quotesAligned && quotes.size() >= 1000) {    // 快照规模足够才算宽度
        QVector<double> changes;                       // 全市场涨跌幅
        changes.reserve(quotes.size());                // 预留容量
        double amountSum = 0.0;                        // 成交额累加
        int up = 0, down = 0, flat = 0;                // 涨/跌/平 家数
        int limitLike = 0;                             // 涨幅≥9.7% 家数
        for (auto it = quotes.constBegin(); it != quotes.constEnd(); ++it) {  // 遍历快照
            const Snapshot& s = it.value();            // 单只快照
            if (!s.isValid()) continue;                // 无效则跳过
            changes.append(s.changePct);               // 收集涨跌幅
            amountSum += s.amount;                     // 累加成交额
            if (s.changePct > 0.05) ++up;              // 上涨
            else if (s.changePct < -0.05) ++down;      // 下跌
            else ++flat;                               // 平盘
            if (s.changePct >= 9.7) ++limitLike;       // 近似涨停家数（反证池子完整性）
        }
        f.snapshotLimitLike = limitLike;               // 快照侧涨停家数
        f.sampleCount = changes.size();                // 有效样本数
        f.upCount = up;                                // 上涨家数
        f.downCount = down;                            // 下跌家数
        f.flatCount = flat;                            // 平盘家数
        f.upRatio = safeDiv(up, up + down + flat);     // 上涨占比
        f.medianChangePct = medianOf(changes);         // 涨跌幅中位数
        f.totalAmount = amountSum;                     // 全市场成交额
        f.breadthKnown = changes.size() * 2 >= 5000;   // 覆盖过半（A 股约 5500 只）才可信
    }

    if (!themesOfStock.isEmpty() && f.limitUpCount > 0) {  // 有题材且涨停非空才统计集中度
        QHash<QString, int> themeCnt;                  // 题材 → 涨停家数
        int covered = 0;                               // 有题材归属的涨停股数
        for (const LimitEntry& e : limitUp) {          // 遍历涨停池
            const QStringList ts = themesOfStock.value(e.code);  // 该股题材
            if (ts.isEmpty()) continue;                // 无题材归属跳过
            ++covered;                                 // 计入覆盖数
            for (const QString& t : ts) themeCnt[t] = themeCnt.value(t, 0) + 1;  // 各题材计数
        }
        int top = 0;                                   // 最强题材家数
        for (auto it = themeCnt.constBegin(); it != themeCnt.constEnd(); ++it)
            top = qMax(top, it.value());               // 取最大值

        f.themeCoveredStocks = covered;                // 有题材的股数
        f.themeCount = themeCnt.size();                // 题材总数
        f.topThemeCount = top;                         // 最强题材家数
        f.themeFocus = safeDiv(top, f.limitUpCount);   // 主线集中度
        f.themeKnown = covered * 10 >= f.limitUpCount * 3;  // 覆盖≥30% 才具代表性
    }

    return f;                                          // 返回原始事实
}

// 量化层：六维打分 → 复合指数 → 阶段/档位/仓位
SentimentState SentimentEngine::evaluate(const SentimentFact& fact,
                                         const QVector<QPair<QDate, double>>& history)
{
    SentimentState s;                                  // 待填充状态
    s.date = fact.date;                                // 归属交易日
    s.fact = fact;                                     // 带上原始事实
    s.history = history;                               // 带上历史序列

    const double luN = qMax(1, fact.limitUpCount);     // 涨停家数（防除零）
    bool degraded = false;                             // 是否有分项降级

    auto push = [&s](const QString& key, const QString& name, double score,
                     double weight, bool available, const QString& detail) {
        SentimentSubScore sub;                         // 单个分项
        sub.key = key;                                 // 编号 S1..S6
        sub.name = name;                               // 中文名
        sub.score = clampD(score, 0.0, 100.0);         // 得分夹紧到 0~100
        sub.weight = weight;                           // 权重
        sub.available = available;                     // 是否可用
        sub.detail = detail;                           // 明细说明
        s.subScores.append(sub);                       // 追加
    };

    // ---- S1 赚钱效应（先行指标）----
    const double premiumScore = scaleTo(fact.premiumPct, kPremiumFloor, kPremiumCeil);  // 溢价得分
    const double advanceScore = scaleTo(fact.advanceRate, kAdvanceFloor, kAdvanceCeil); // 连板晋级得分
    const double firstAdvScore = scaleTo(fact.firstBoardAdvanceRate, kFirstAdvanceFloor, kFirstAdvanceCeil);  // 首板晋级得分

    double s1 = 0.0;                                   // S1 得分
    QString s1Detail;                                  // S1 说明
    const bool premiumSurvivor = fact.premiumKnown && fact.breadthKnown        // 溢价可信且宽度可信
                              && fact.premiumPct >= kPremiumSurvivorPct        // 溢价极高
                              && fact.upRatio < kPremiumSurvivorUpRatio;       // 但全市场普跌
    if (fact.premiumKnown && premiumSurvivor) {        // 幸存者偏差：下调溢价权重
        s1 = 0.20 * premiumScore + 0.45 * advanceScore + 0.35 * firstAdvScore;
        s1Detail = QStringLiteral("溢价 %1% 与宽度背离（上涨仅 %2%），按幸存者偏差下调溢价权重 · 晋级 %3%/%4%")
                       .arg(fact.premiumPct, 0, 'f', 2)
                       .arg(fact.upRatio * 100, 0, 'f', 1)
                       .arg(fact.advanceRate * 100, 0, 'f', 1)
                       .arg(fact.firstBoardAdvanceRate * 100, 0, 'f', 1);
    } else if (fact.premiumKnown) {                    // 正常口径：溢价权重最高
        s1 = 0.45 * premiumScore + 0.35 * advanceScore + 0.20 * firstAdvScore;
        s1Detail = QStringLiteral("溢价 %1% · 连板晋级 %2% · 首板晋级 %3%")
                       .arg(fact.premiumPct, 0, 'f', 2)
                       .arg(fact.advanceRate * 100, 0, 'f', 1)
                       .arg(fact.firstBoardAdvanceRate * 100, 0, 'f', 1);
    } else {                                           // 溢价不可信：权重回退到晋级率
        s1 = 0.60 * advanceScore + 0.40 * firstAdvScore;
        s1Detail = QStringLiteral("溢价样本不足（%1/%2），权重回退到晋级率：连板 %3% · 首板 %4%")
                       .arg(fact.premiumSamples).arg(fact.prevLimitUpCount)
                       .arg(fact.advanceRate * 100, 0, 'f', 1)
                       .arg(fact.firstBoardAdvanceRate * 100, 0, 'f', 1);
        degraded = true;                               // 标记降级
    }
    push(QStringLiteral("S1"), QStringLiteral("赚钱效应"), s1, kWProfitEffect, true, s1Detail);

    // ---- S2 封板质量 ----
    const double zbScore    = 100.0 - scaleTo(fact.brokenRate, kBrokenRateBest, kBrokenRateWorst);  // 炸板率反向
    const double sealScore  = scaleTo(fact.avgSealToAmount, kSealRatioFloor, kSealRatioCeil);       // 封单强度
    const double earlyShare = safeDiv(fact.sealBefore1030, luN);                                    // 早封占比
    const double earlyScore = scaleTo(earlyShare, kEarlySealFloor, kEarlySealCeil);                 // 早封得分
    const double s2 = 0.55 * zbScore + 0.25 * sealScore + 0.20 * earlyScore;                        // 加权
    push(QStringLiteral("S2"), QStringLiteral("封板质量"), s2, kWSealQuality, true,
         QStringLiteral("炸板率 %1% · 封单/成交 %2% · 10:30 前封板 %3%")
             .arg(fact.brokenRate * 100, 0, 'f', 1)
             .arg(fact.avgSealToAmount * 100, 0, 'f', 1)
             .arg(earlyShare * 100, 0, 'f', 0));

    // ---- S3 空间高度 ----
    const double heightScore = scaleTo(fact.maxBoardHeight, kHeightFloor, kHeightCeil);  // 最高板
    const double multiScore  = scaleTo(safeDiv(fact.multiBoardCount, luN), 0.05, 0.45);  // 连板占比
    const double highScore   = scaleTo(fact.highBoardCount, 0.0, 8.0);                   // ≥3 板家数
    const double s3 = 0.50 * heightScore + 0.25 * multiScore + 0.25 * highScore;         // 加权
    push(QStringLiteral("S3"), QStringLiteral("空间高度"), s3, kWHeight, true,
         QStringLiteral("最高 %1 板 · 连板 %2 家 · ≥3 板 %3 家")
             .arg(fact.maxBoardHeight).arg(fact.multiBoardCount).arg(fact.highBoardCount));

    // ---- S4 市场宽度 ----
    const double luScore = scaleTo(fact.limitUpCount, kWeakLimitUpCount, kStrongLimitUpCount);  // 涨停家数
    double s4 = 0.0;                                   // S4 得分
    if (fact.breadthKnown) {                           // 宽度数据可用
        const double upScore = scaleTo(fact.upRatio, kUpRatioFloor, kUpRatioCeil);           // 上涨占比
        const double medScore = scaleTo(fact.medianChangePct, kMedianFloor, kMedianCeil);    // 中位涨幅
        const double dtPenalty = qMin(40.0, fact.limitDownCount * 6.0);                      // 跌停惩罚
        s4 = clampD(0.40 * upScore + 0.30 * medScore + 0.30 * luScore - dtPenalty, 0.0, 100.0);
        push(QStringLiteral("S4"), QStringLiteral("市场宽度"), s4, kWBreadth, true,
             QStringLiteral("上涨占比 %1% · 中位数 %2% · 涨停 %3 家 · 跌停 %4 家")
                 .arg(fact.upRatio * 100, 0, 'f', 1)
                 .arg(fact.medianChangePct, 0, 'f', 2)
                 .arg(fact.limitUpCount).arg(fact.limitDownCount));
    } else {                                           // 宽度不可用：退化为涨停家数
        s4 = luScore;
        push(QStringLiteral("S4"), QStringLiteral("市场宽度"), s4, kWBreadth, true,
             QStringLiteral("快照未对齐/覆盖不足（样本 %1），退化为涨停家数 %2 家")
                 .arg(fact.sampleCount).arg(fact.limitUpCount));
        degraded = true;                               // 标记降级
    }

    // ---- S5 分歧收敛（反向：分高 = 分歧小）----
    const double breakScore = 100.0 - scaleTo(fact.avgBreakCount, 0.0, 3.0);      // 开板次数反向
    const double turnScore  = 100.0 - scaleTo(fact.avgTurnover, 25.0, 45.0);      // 换手过热反向
    const double lateShare  = safeDiv(fact.sealAfter1400, luN);                   // 尾盘封板占比
    const double lateScore  = 100.0 - scaleTo(lateShare, 0.05, 0.40);             // 尾盘封板反向
    const double s5 = 0.40 * breakScore + 0.35 * turnScore + 0.25 * lateScore;    // 加权
    push(QStringLiteral("S5"), QStringLiteral("分歧收敛"), s5, kWDivergence, true,
         QStringLiteral("平均开板 %1 次 · 平均换手 %2% · 尾盘封板 %3%")
             .arg(fact.avgBreakCount, 0, 'f', 2)
             .arg(fact.avgTurnover, 0, 'f', 1)
             .arg(lateShare * 100, 0, 'f', 0));

    // ---- S6 主线集中度 ----
    if (fact.themeKnown) {                             // 题材数据可用
        const double topScore   = scaleTo(fact.topThemeCount, 2.0, 10.0);                     // 最强题材家数
        const double focusScore = scaleTo(fact.themeFocus, kThemeFocusFloor, kThemeFocusCeil);  // 集中度
        const double breadthScore = scaleTo(fact.themeCount, 3.0, 12.0);                      // 题材扩散度
        const double s6 = 0.50 * topScore + 0.30 * focusScore + 0.20 * breadthScore;          // 加权
        push(QStringLiteral("S6"), QStringLiteral("主线集中度"), s6, kWThemeFocus, true,
             QStringLiteral("最强题材 %1 家 · 集中度 %2% · 题材 %3 个")
                 .arg(fact.topThemeCount)
                 .arg(fact.themeFocus * 100, 0, 'f', 0)
                 .arg(fact.themeCount));
    } else {                                           // 覆盖不足：该维度不参与复合
        push(QStringLiteral("S6"), QStringLiteral("主线集中度"), 0.0, kWThemeFocus, false,
             QStringLiteral("题材覆盖不足（%1/%2），本维度不参与复合")
                 .arg(fact.themeCoveredStocks).arg(fact.limitUpCount));
        degraded = true;                               // 标记降级
    }

    double sum = 0.0, wsum = 0.0;                      // 加权和 / 权重和
    for (const SentimentSubScore& sub : s.subScores) { // 只对可用维度归一化
        if (!sub.available || sub.weight <= 0.0) continue;  // 跳过不可用或零权重
        sum += sub.score * sub.weight;                 // 累加加权分
        wsum += sub.weight;                            // 累加权重
    }
    s.composite = wsum > 0.0 ? sum / wsum : 0.0;       // 复合指数

    if (!history.isEmpty()) {                          // 有历史才能算趋势
        QVector<double> prev;                          // 早于当日的样本
        for (const auto& p : history) {                // 遍历历史
            if (p.first.isValid() && p.first < fact.date) prev.append(p.second);  // 只取更早的
        }
        if (!prev.isEmpty()) {                         // 有可比样本
            const int take = qMin(5, prev.size());     // 取最近 5 个
            double avg = 0.0;                          // 均值累加
            for (int i = prev.size() - take; i < prev.size(); ++i) avg += prev.at(i);  // 求和
            avg /= take;                               // 求平均
            s.trend = s.composite - avg;               // 相对 5 日均值的偏离
            s.acceleration = s.composite - prev.last(); // 相对上一日的变化
            s.trendKnown = true;                       // 趋势可算
        }
    }

    // 数据交叉校验：用「快照里涨幅≥9.7% 的家数」反证涨停池是否缺页
    if (fact.breadthKnown && fact.limitUpCount > 0) {  // 宽度可用才有反证基准
        const int pool = fact.limitUpCount;            // 池子家数
        const int snap = fact.snapshotLimitLike;       // 快照侧家数
        const bool tooFew  = snap * 2 < pool;                       // 快照侧不足池子一半
        const bool tooMany = pool > 20 && snap > pool * 2 + 20;     // 池子明显缺页
        if (tooFew || tooMany) {                       // 两侧矛盾
            s.dataSuspect = true;                      // 标记数据可疑
            s.dataWarnings << QStringLiteral(
                "数据校验失败：涨停池 %1 家 vs 快照涨幅≥9.7% %2 家（疑似接口缺页/降级）")
                .arg(pool).arg(snap);
            s.vetoReasons << QStringLiteral("数据不可信（池 %1 / 快照 %2）").arg(pool).arg(snap);
        } else {
            s.dataWarnings << QStringLiteral("数据校验通过：池 %1 / 快照 %2").arg(pool).arg(snap);
        }
    }

    // 硬否决：一票否决，优先于加权指数
    if (fact.limitDownCount >= kMaxLimitDownCount) {   // 跌停潮
        s.vetoReasons << QStringLiteral("跌停 %1 家(≥%2)")
                             .arg(fact.limitDownCount).arg(kMaxLimitDownCount);
    }
    if (fact.brokenRate > kMaxBrokenRate) {            // 炸板率失控
        s.vetoReasons << QStringLiteral("炸板率 %1%(>%2%)")
                             .arg(fact.brokenRate * 100, 0, 'f', 1)
                             .arg(kMaxBrokenRate * 100, 0, 'f', 0);
    }
    if (fact.premiumKnown                              // 溢价崩塌
        && fact.premiumPct < -3.0 && fact.brokenRate > kWatchBrokenRate) {
        s.vetoReasons << QStringLiteral("昨日涨停今日 %1%（打板即亏）")
                             .arg(fact.premiumPct, 0, 'f', 2);
    }
    const bool riskVeto = !s.vetoReasons.isEmpty();    // 是否已触发否决

    bool retreat = riskVeto;                           // 是否判定退潮
    if (!retreat && s.composite < kRecoveryIndex && s.trendKnown
        && s.trend <= -kRetreatDrop) {                 // 指数低于回暖线且回落过快
        retreat = true;                                // 判退潮
        s.vetoReasons << QStringLiteral("情绪指数较 5 日均值回落 %1 点").arg(-s.trend, 0, 'f', 1);
    }
    if (!retreat && s.composite < kRecoveryIndex && fact.premiumKnown
        && fact.premiumPct < 0.0 && fact.brokenRate > kWatchBrokenRate) {  // 溢价转负 + 炸板偏高
        retreat = true;                                // 判退潮
        s.vetoReasons << QStringLiteral("溢价转负且炸板率偏高");
    }

    MarketPhase phase = MarketPhase::Unknown;          // 待判定阶段
    if (retreat) {
        phase = MarketPhase::Retreat;                  // 退潮
    } else if (s.composite < kIcePointIndex) {
        phase = MarketPhase::IcePoint;                 // 冰点
    } else if (s.composite < kRecoveryIndex) {
        phase = MarketPhase::Recovery;                 // 回暖
    } else if (s.composite >= kClimaxIndex) {
        phase = MarketPhase::Climax;                   // 高潮
    } else if (s.composite >= kMainUpIndex             // 主升区但高度高、晋级率弱
               && fact.maxBoardHeight >= kClimaxHeight
               && fact.prevLimitUpCount > 0
               && fact.advanceRate < kClimaxAdvanceWeak) {
        phase = MarketPhase::Climax;                   // 高位分歧按高潮处理
    } else {
        phase = MarketPhase::MainUp;                   // 主升
    }
    s.phase = phase;                                   // 写入阶段
    s.tradeAllowed = !riskVeto                         // 未被否决
                  && phase != MarketPhase::IcePoint    // 且非冰点
                  && phase != MarketPhase::Retreat;    // 且非退潮

    if (!s.tradeAllowed) {                             // 禁止开仓
        s.tier = SentimentTier::NoTrade;               // 无档位
    } else if (phase == MarketPhase::Climax) {         // 高潮期
        s.tier = SentimentTier::Harvest;               // 只兑现
    } else if (phase == MarketPhase::Recovery) {       // 回暖期
        s.tier = SentimentTier::Probe;                 // 只试仓
    } else {                                           // 主升期按指数强度再分档
        const double attackLine = (kMainUpIndex + kClimaxIndex) * 0.5;  // 主攻分界
        s.tier = (s.composite >= attackLine) ? SentimentTier::Attack : SentimentTier::Build;
    }

    double cap = 0.0;                                  // 仓位上限
    switch (s.tier) {                                  // 按档位取基准上限
    case SentimentTier::Probe:   cap = kCapProbe;     break;   // 试仓
    case SentimentTier::Build:   cap = kCapBuild;     break;   // 建仓
    case SentimentTier::Attack:  cap = kCapAttack;    break;   // 主攻
    case SentimentTier::Harvest: cap = kCapHarvest;   break;   // 兑现
    default:                     cap = 0.0;           break;   // 禁止开仓
    }
    if (s.tier != SentimentTier::NoTrade) {            // 仅在有档位时做折减
        if (degraded) cap *= kDegradedCapScale;        // 数据降级折减
        if (s.trendKnown && s.acceleration <= -5.0) cap *= 0.80;  // 情绪拐头当日降档
        if (fact.breadthKnown && fact.upRatio < kBreadthDivergenceRatio)  // 指数与宽度背离
            cap *= kBreadthDivergenceScale;            // 极端分化降仓
    }
    s.degraded = degraded;                             // 记录降级标记
    s.positionCapPct = clampD(cap, 0.0, 100.0);        // 夹紧到 0~100

    buildRegime(s);                                    // 生成兼容 MarketRegime
    buildNote(s);                                      // 生成一行结论
    return s;                                          // 返回完整状态
}

// 一步到位入口：采集 + 量化
SentimentState SentimentEngine::run(const QDate& date,
                                    const QVector<LimitEntry>& limitUp,
                                    const QVector<LimitEntry>& broken,
                                    const QVector<LimitEntry>& limitDown,
                                    const QVector<LimitEntry>& prevLimitUp,
                                    const QHash<QString, Snapshot>& quotes,
                                    const QHash<QString, QStringList>& themesOfStock,
                                    const QVector<QPair<QDate, double>>& history)
{
    const SentimentFact fact = collect(date, limitUp, broken, limitDown,   // 先采集
                                       prevLimitUp, quotes, themesOfStock);
    return evaluate(fact, history);                    // 再量化
}

// 兼容层：把情绪状态映射成下游认识的 MarketRegime
void SentimentEngine::buildRegime(SentimentState& s)
{
    const SentimentFact& f = s.fact;                   // 原始事实
    MarketRegime& r = s.regime;                        // 目标结构

    r.date = f.date;                                   // 交易日
    r.limitUpCount = f.limitUpCount;                   // 涨停家数
    r.brokenCount = f.brokenCount;                     // 炸板家数
    r.limitDownCount = f.limitDownCount;               // 跌停家数
    r.firstBoardCount = f.firstBoardCount;             // 首板家数
    r.multiBoardCount = f.multiBoardCount;             // 连板家数
    r.maxBoardHeight = f.maxBoardHeight;               // 最高板
    r.brokenRate = f.brokenRate;                       // 炸板率
    r.advanceRate = f.advanceRate;                     // 连板晋级率
    r.firstBoardAdvanceRate = f.firstBoardAdvanceRate; // 首板晋级率
    r.prevLimitUpAvgPct = f.premiumPct;                // 昨日涨停今日平均涨幅
    r.prevLimitUpCount = f.prevLimitUpCount;           // 昨日涨停家数
    r.premiumSampleCount = f.premiumSamples;           // 溢价采样数
    r.premiumKnown = f.premiumKnown;                   // 溢价是否可信
    r.yesterdayKnown = f.prevLimitUpCount > 0;         // 昨日数据是否就绪
    r.phase = s.phase;                                 // 阶段
    r.tradeAllowed = s.tradeAllowed;                   // 是否放行
    r.note = s.note;                                   // 结论文案
}

// 生成一行结论：复合指数 + 趋势 + 六维 + 溢价 + 否决 + 仓位上限
void SentimentEngine::buildNote(SentimentState& s)
{
    QStringList parts;                                 // 结论分段
    parts << QStringLiteral("复合情绪 %1").arg(s.composite, 0, 'f', 1);  // 复合指数

    if (s.trendKnown) {                                // 有趋势才展示
        parts << QStringLiteral("较5日均 %1%2（今日 %3%4）")
                     .arg(s.trend >= 0 ? QStringLiteral("+") : QString())      // 趋势正负号
                     .arg(s.trend, 0, 'f', 1)                                  // 趋势值
                     .arg(s.acceleration >= 0 ? QStringLiteral("+") : QString())  // 加速度正负号
                     .arg(s.acceleration, 0, 'f', 1);                          // 加速度值
    }

    QStringList dims;                                  // 六维明细
    for (const SentimentSubScore& sub : s.subScores) { // 遍历分项
        if (!sub.available) continue;                  // 跳过不可用维度
        dims << QStringLiteral("%1 %2").arg(sub.name).arg(sub.score, 0, 'f', 0);  // 名称 + 得分
    }
    if (!dims.isEmpty()) parts << dims.join(QStringLiteral(" · "));  // 拼接六维

    parts << QStringLiteral("溢价 %1% · 晋级 %2%")
                 .arg(s.fact.premiumKnown ? QString::number(s.fact.premiumPct, 'f', 2)  // 可信则给数值
                                          : QStringLiteral("样本不足"))                 // 否则标注样本不足
                 .arg(s.fact.advanceRate * 100, 0, 'f', 1);

    if (s.dataSuspect && !s.dataWarnings.isEmpty())     // 数据可疑则附首条校验信息
        parts << s.dataWarnings.first();

    if (s.fact.breadthKnown
        && s.fact.upRatio < kBreadthDivergenceRatio) {  // 宽度背离提示
        parts << QStringLiteral("宽度背离（上涨 %1%，仓位折减）")
                     .arg(s.fact.upRatio * 100, 0, 'f', 1);
    }

    // 头部统一为数字口径状态标签，不再输出「阶段·档位」复合信息
    const QString head = QStringLiteral("【%1】")
                             .arg(sentimentStatusLabel(s.composite, s.positionCapPct,
                                                       s.tradeAllowed));

    QString note = head + parts.join(QStringLiteral("｜"));   // 拼接完整结论
    if (!s.vetoReasons.isEmpty()) {                    // 有否决则追加
        note += QStringLiteral("｜否决：") + s.vetoReasons.join(QStringLiteral("；"));
    }
    if (s.degraded) note += QStringLiteral("｜数据降级（仓位已折减）");  // 降级提示

    s.note = note;                                     // 写入状态
    s.regime.note = note;                              // 同步到兼容层
}

// 六维明细多行文本（自检 / 报告用）
QString SentimentEngine::factorSummary(const SentimentState& s)
{
    QStringList dims;                                  // 逐维文本
    for (const SentimentSubScore& sub : s.subScores) { // 遍历分项
        if (!sub.available) continue;                  // 跳过不可用
        dims << QStringLiteral("%1 %2(%3)")
                    .arg(sub.name)                     // 名称
                    .arg(sub.score, 0, 'f', 1)         // 得分
                    .arg(sub.detail);                  // 明细
    }
    return dims.join(QStringLiteral("\n"));            // 每维一行
}

// 阶段建议一行文本
QString SentimentEngine::phaseAdvice(const SentimentState& s)
{
    return QStringLiteral("%1 %2（有效总仓上限 %3%）")
        .arg(marketPhaseAdvice(s.phase), sentimentTierAdvice(s.tier))  // 阶段建议 + 档位建议
        .arg(s.positionCapPct, 0, 'f', 0);                             // 仓位上限
}

// 阶段允许参与的最大连板高度
int SentimentEngine::maxBoardForPhase(MarketPhase phase)
{
    switch (phase) {                                   // 按阶段取窗口
    case MarketPhase::Recovery: return kMaxBoardRecovery;  // 回暖 2 板
    case MarketPhase::MainUp:   return kMaxBoardMainUp;    // 主升 4 板
    case MarketPhase::Climax:   return kMaxBoardClimax;    // 高潮 3 板
    case MarketPhase::IcePoint:
    case MarketPhase::Retreat:  return 0;                  // 不新开仓
    default:                    return kMaxBoardMainUp;    // 未知按主升
    }
}

// 情绪域收窄：只排除板数超出本阶段窗口的标的
bool SentimentEngine::passUniverse(const LimitEntry& entry, const SentimentState& state,
                                   QString& reason)
{
    if (!state.valid()) return true;                   // 无情绪状态则不收窄
    if (state.tier == SentimentTier::NoTrade) return true;         // 禁止开仓时仍保留观察名单

    const int maxBoard = maxBoardForPhase(state.phase);  // 本阶段高度窗口
    if (maxBoard > 0 && entry.boardCount > maxBoard) {                     // 超出窗口
        reason = QStringLiteral("情绪域：%1只做 ≤%2 板（该标的 %3 板，超出本阶段可参与高度）")
                     .arg(marketPhaseName(state.phase))
                     .arg(maxBoard)
                     .arg(entry.boardCount);
        return false;                                  // 排除
    }
    return true;                                       // 通过
}
