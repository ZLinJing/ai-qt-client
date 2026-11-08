#include "strategy/FilterPipeline.h"     // 本模块声明
#include "strategy/Scorer.h"             // 评分器
#include "core/Indicators.h"             // 指标计算
#include "core/RiskFlags.h"              // 风险标记
#include "sentiment/SentimentEngine.h"   // 情绪域收窄（passUniverse）

namespace {                              // 分层筛选阈值（固定口径）

// ---- L1 通用硬排除 ----
constexpr bool   kExcludeST          = true;   // 硬排除：ST / 退市风险股
constexpr bool   kExcludeBSE         = true;   // 硬排除：北交所 30cm 生态不同
constexpr bool   kExcludeOneWordBoard = true;  // 硬排除：一字板无换手，筹码断层
constexpr bool   kExcludeNewStock    = true;   // 硬排除：上市不足 minListDays 个交易日
constexpr double kMaxPrice           = 50.0;   // 股价上限（元）
constexpr double kMinPrice           = 2.0;    // 股价下限（元）
constexpr double kMinFloatCapYi      = 15.0;   // 流通市值下限（亿元）
constexpr double kMaxFloatCapYi      = 150.0;  // 流通市值上限（亿元）
constexpr int    kMinListDays        = 60;     // 上市天数下限

// ---- L2 板块效应 ----
constexpr bool   kPreferConceptTheme = true;   // 优先用概念题材统计（行业分类仅作兜底）
constexpr int    kMinSectorLimitCount = 3;     // 同板块当日涨停家数下限
constexpr int    kMinSectorPeers     = 2;      // 板块内除自身外需涨停家数

// ---- L3 分层量价 ----
constexpr int    kLimitGeneDays      = 20;     // 涨停基因回看天数
constexpr int    kMinLimitCount20    = 1;      // 近 20 日至少涨停过 N 次
constexpr double kMinVolRatio5       = 1.5;    // 当日量 / 前5日均量 下限
constexpr double kMaxVolRatio5       = 3.0;    // 上限（超过=爆量分歧）
constexpr int    kMaxFirstSealMinute = 630;    // 首次封板最晚 10:30（分钟数）
constexpr int    kMaxBreakCountFirst = 2;      // 首板当日开板次数上限
constexpr double kMaxPrevChangePct   = 9.5;    // 前一日涨幅上限（避免高位接力）
constexpr double kMinSealOrderRatio  = 0.20;   // 二板：封单额 / 当日成交额 下限
constexpr double kMinSealToFloatCap  = 0.01;   // 二板：封单额 / 流通市值 下限
constexpr double kMinVolRatioVsPrevBoard = 1.2;  // 二板量 / 首板量 下限
constexpr double kMaxVolRatioVsPrevBoard = 3.0;  // 二板量 / 首板量 上限（爆量分歧）
constexpr bool   kAllowShrinkVolume  = true;   // 二板：允许缩量不缩势
constexpr int    kMinHeightForMainUp = 3;      // 三板+：主攻所需最低高度
constexpr double kMaxMainUpHeight    = 6;      // 三板+：超过该高度一律不碰

// ---- L4 评分与输出 ----
constexpr double kRiskPenaltyPerFlag = 3.0;    // 每条风险标记的评分扣分
constexpr int    kMaxRiskPenaltyFlags = 3;     // 参与扣分的风险条数上限
constexpr double kGradeA             = 75.0;   // A 级分数线
constexpr double kGradeB             = 60.0;   // B 级分数线
constexpr int    kMaxCandidates      = 30;     // 候选池最大条目
constexpr double kLeaderBonusScore   = 8.0;    // 同题材身位第一的加分

} // namespace

FilterPipeline::FilterPipeline(QObject* parent) : QObject(parent) {}  // 构造：无额外初始化

// 按行业统计当日涨停家数（题材缺失时的兜底口径）
QHash<QString, int> FilterPipeline::buildSectorCounts(const QVector<LimitEntry>& pool)
{
    QHash<QString, int> counts;                  // 行业 → 家数
    for (const LimitEntry& e : pool) {           // 遍历涨停池
        const QString key = e.sector.isEmpty() ? QStringLiteral("未分类") : e.sector;  // 空行业归入未分类
        counts[key] = counts.value(key, 0) + 1;  // 累加计数
    }
    return counts;                               // 返回统计结果
}

// 按题材统计当日涨停家数（主口径）：一只票可计入多个题材
QHash<QString, int> FilterPipeline::buildThemeCounts(
    const QVector<LimitEntry>& pool,
    const QHash<QString, QStringList>& themesOfStock)
{
    QHash<QString, int> counts;                  // 题材 → 家数
    for (const LimitEntry& e : pool) {           // 遍历涨停池
        const QStringList themes = themesOfStock.value(e.code);  // 该票所属题材
        for (const QString& t : themes) counts[t] = counts.value(t, 0) + 1;  // 每个题材各计一次
    }
    return counts;                               // 返回统计结果
}

// L1 通用硬排除：ST / 北交所 / 一字板 / 股价 / 市值 / 次新 / 情绪域
bool FilterPipeline::passHardFilter(const LimitEntry& e,
                                    const FilterContext& ctx, Candidate& c, QString& reason) const
{
    c.entry = e;                                 // 先带上原始条目
    auto snapIt = ctx.snapshots.constFind(e.code);  // 查该票快照
    if (snapIt != ctx.snapshots.constEnd()) {    // 有快照则合并开高低
        c.snapshot = snapIt.value();             // 记录快照
        c.entry.open = snapIt->open;             // 回填今开
        c.entry.high = snapIt->high;             // 回填最高
        c.entry.low  = snapIt->low;              // 回填最低
    }

    if (kExcludeST && (e.name.contains(QStringLiteral("ST")) || e.name.contains(QStringLiteral("退")))) {
        reason = QStringLiteral("ST / 退市风险股");  // 命中 ST/退市
        return false;                            // 淘汰
    }
    if (kExcludeBSE && e.board == Board::BSE) {  // 北交所生态不同
        reason = QStringLiteral("北交所 30cm 生态不同，剔除");
        return false;                            // 淘汰
    }
    const double limitPx = e.limitPrice > 0.0 ? e.limitPrice : e.price;  // 涨停价锚点
    if (limitPx > 0.0 && c.entry.open > 0.0 && c.entry.high > 0.0 && c.entry.low > 0.0) {  // 需开高低齐全
        const auto eq = [limitPx](double v) { return qAbs(v - limitPx) < 0.011; };  // 是否等于涨停价（容忍浮点误差）
        c.entry.isOneWordBoard = eq(c.entry.open) && eq(c.entry.high) && eq(c.entry.low);  // 开=高=低=涨停价才是真一字
        const int sealMin = e.firstSealTime.isValid()                            // 首封时间换算成分钟
                              ? e.firstSealTime.hour() * 60 + e.firstSealTime.minute() : 9999;
        c.entry.isSecondSeal = !c.entry.isOneWordBoard && sealMin <= 9 * 60 + 31  // 非一字且 9:31 前封死
                             && e.breakCount == 0;                               // 且全天未开板
    }
    if (kExcludeOneWordBoard && c.entry.isOneWordBoard) {   // 一字板筹码断层
        reason = QStringLiteral("一字板无换手，筹码断层");
        return false;                            // 淘汰
    }
    if (e.price > kMaxPrice) {                   // 股价超上限
        reason = QStringLiteral("股价 %1 元 > 上限 %2 元").arg(e.price).arg(kMaxPrice);
        return false;                            // 淘汰
    }
    if (e.price < kMinPrice) {                   // 股价低于下限（仙股风险）
        reason = QStringLiteral("股价 %1 元 < 下限 %2 元（低价仙股风险）").arg(e.price).arg(kMinPrice);
        return false;                            // 淘汰
    }
    const double capYi = e.floatCap / 1e8;       // 流通市值换算成亿元
    if (capYi < kMinFloatCapYi) {                // 市值过小易被操纵
        reason = QStringLiteral("流通市值 %1 亿 < %2 亿（太小易被操纵）").arg(capYi, 0, 'f', 1).arg(kMinFloatCapYi);
        return false;                            // 淘汰
    }
    if (capYi > kMaxFloatCapYi) {                // 市值过大资金难拉动
        reason = QStringLiteral("流通市值 %1 亿 > %2 亿（资金难拉动）").arg(capYi, 0, 'f', 1).arg(kMaxFloatCapYi);
        return false;                            // 淘汰
    }
    auto kIt = ctx.klines.constFind(e.code);     // 查该票 K 线
    if (kIt != ctx.klines.constEnd() && kIt.value().valid()) {  // 有有效 K 线才判断上市天数
        c.listDays = kIt.value().candles.size(); // 可见交易日数
        if (kExcludeNewStock && c.listDays < kMinListDays) {  // 不足则为次新股
            reason = QStringLiteral("次新股：上市仅约 %1 个交易日(<%2)，无涨停基因参考")
                         .arg(c.listDays).arg(kMinListDays);
            return false;                        // 淘汰
        }
    }

    // L1.5 情绪域收窄：阶段决定「这个高度今天该不该参与」；无情绪状态时完全退回原行为
    if (ctx.sentiment) {                         // 有情绪状态才收窄
        QString sentimentReason;                 // 淘汰原因
        if (!SentimentEngine::passUniverse(e, *ctx.sentiment, sentimentReason)) {
            reason = sentimentReason;            // 记录情绪域给出的原因
            return false;                        // 淘汰
        }
    }
    return true;                                 // 全部通过
}

// L2 板块效应：取「涨停家数最多、涨幅最高」的题材/行业作为主题材，并校验家数门槛
bool FilterPipeline::passSector(const LimitEntry& e,
                                const FilterContext& ctx, Candidate& c, QString& reason) const
{
    int bestCount = 0;                           // 最佳板块的涨停家数
    double bestChg = 0.0;                        // 最佳板块的涨幅
    QString bestTheme;                           // 最佳板块名称

    if (kPreferConceptTheme) {                   // 优先用概念题材口径
        const QStringList themes = ctx.themesOfStock.value(e.code);  // 该票所属题材
        for (const QString& t : themes) {        // 逐个题材比较
            const int cnt = ctx.themeCount.value(t, 0);          // 该题材涨停家数
            const double chg = ctx.themeChangePct.value(t, 0.0);  // 该题材涨幅
            if (cnt > bestCount || (cnt == bestCount && !bestTheme.isEmpty() && chg > bestChg)) {
                bestCount = cnt;                 // 家数更多或同家数涨幅更高则替换
                bestTheme = t;                   // 记录题材名
                bestChg = chg;                   // 记录涨幅
            }
        }
    }

    const QString industry = e.sector.isEmpty() ? QStringLiteral("未分类") : e.sector;  // 行业口径名称
    const int industryCount = ctx.sectorCount.value(industry, 0);                      // 行业口径家数
    if (bestTheme.isEmpty() || industryCount > bestCount) {   // 题材缺失或行业更强则回退行业
        bestTheme = industry;                    // 用行业名，避免口径混搭
        bestCount = industryCount;               // 用行业家数
        bestChg = ctx.themeChangePct.value(industry, 0.0);  // 用行业涨幅（通常为 0）
    }

    c.entry.theme = bestTheme;                   // 写入主题材
    c.entry.themeLimitCount = bestCount;         // 写入题材涨停家数
    c.entry.themeChangePct = bestChg;            // 写入题材涨幅
    c.sectorLimitCount = bestCount;              // 兼容字段：板块涨停家数

    const int minNeed = qMax(1, qMax(kMinSectorLimitCount, kMinSectorPeers + 1));  // 门槛（同伴数语义需 +1）
    if (bestCount < minNeed) {                   // 未达门槛即为孤板
        reason = QStringLiteral("题材「%1」当日涨停仅 %2 家(<%3)，孤板无联动")
                     .arg(bestTheme).arg(bestCount).arg(minNeed);
        return false;                            // 淘汰
    }
    return true;                                 // 通过
}

// L3 分层量价：首板看换手、二板看强度与封单、三板+看龙头地位
bool FilterPipeline::passQuant(const LimitEntry& e,
                               const FilterContext& ctx, Candidate& c, QString& reason) const
{
    const Ind::TurnoverBand band = Ind::turnoverBandFor(e.floatCap / 1e8, e.board);  // 选换手档（双创已放大）
    const bool hasBand = band.dangerTurnover > 0.0;   // 档位是否有效

    auto kIt = ctx.klines.constFind(e.code);          // 查 K 线
    const KlineSeries* k = (kIt != ctx.klines.constEnd()) ? &kIt.value() : nullptr;  // 指针形式便于判空
    if (k) {                                          // 有 K 线才做派生计算
        const bool isST = e.name.contains(QStringLiteral("ST"));  // ST 涨跌幅规则不同
        c.listDays = k->candles.size();               // 可见交易日数
        c.volRatio5 = Ind::volRatio(*k, 5);           // 5 日量比
        c.limitCount20 = Ind::limitUpCount(*k, kLimitGeneDays, e.board, isST);  // 涨停基因
        c.maBull = Ind::maBullStrength(*k, 5, 20);    // 均线多头强度
        c.prevDayChangePct = Ind::prevDayChangePct(*k);  // T-1 真实涨幅（非涨停当日）
    }

    if (e.boardCount <= 1) {                          // 首板：核心看换手
        if (hasBand) {                                // 有档位才校验换手
            if (e.turnover < band.minTurnover) {      // 换手不足
                reason = QStringLiteral("首板换手 %1% < 合格线 %2%，筹码未换手，次日易砸")
                             .arg(e.turnover, 0, 'f', 1).arg(band.minTurnover, 0, 'f', 1);
                return false;                         // 淘汰
            }
            if (e.turnover >= band.dangerTurnover) {  // 换手过热
                reason = QStringLiteral("首板换手 %1% ≥ 危险线 %2%，动能透支出货")
                             .arg(e.turnover, 0, 'f', 1).arg(band.dangerTurnover, 0, 'f', 1);
                return false;                         // 淘汰
            }
        }
        if (c.volRatio5 > 0.0) {                      // 有量比数据才校验
            if (c.volRatio5 < kMinVolRatio5) {        // 未放量
                reason = QStringLiteral("首板量比 %1 < %2（未放量）")
                             .arg(c.volRatio5, 0, 'f', 2).arg(kMinVolRatio5, 0, 'f', 2);
                return false;                         // 淘汰
            }
            if (c.volRatio5 > kMaxVolRatio5) {        // 爆量分歧
                reason = QStringLiteral("首板量比 %1 > %2（爆量分歧，易炸板）")
                             .arg(c.volRatio5, 0, 'f', 2).arg(kMaxVolRatio5, 0, 'f', 2);
                return false;                         // 淘汰
            }
        }
        if (e.firstSealTime.isValid()) {              // 有首封时间才校验封板早迟
            const int sealMinute = e.firstSealTime.hour() * 60 + e.firstSealTime.minute();  // 换算分钟
            if (sealMinute > kMaxFirstSealMinute) {   // 封板过晚（尾盘偷袭）
                reason = QStringLiteral("首封 %1 晚于 %2:%3，尾盘偷袭板")
                             .arg(e.firstSealTime.toString("HH:mm"))
                             .arg(kMaxFirstSealMinute / 60).arg(kMaxFirstSealMinute % 60, 2, 10, QChar('0'));
                return false;                         // 淘汰
            }
        }
        if (e.breakCount > kMaxBreakCountFirst) {     // 开板次数超限
            reason = QStringLiteral("当日开板 %1 次(>%2)，封板决心不足")
                         .arg(e.breakCount).arg(kMaxBreakCountFirst);
            return false;                             // 淘汰
        }
        if (c.limitCount20 < kMinLimitCount20) {      // 涨停基因不足
            reason = QStringLiteral("近 %1 日仅涨停 %2 次，涨停基因弱")
                         .arg(kLimitGeneDays).arg(c.limitCount20);
            return false;                             // 淘汰
        }
        if (c.prevDayChangePct > kMaxPrevChangePct) {  // 前一日已大涨，高位接力风险
            reason = QStringLiteral("前一日已涨 %1%，高位接力风险")
                         .arg(c.prevDayChangePct, 0, 'f', 1);
            return false;                             // 淘汰
        }
        return true;                                  // 首板通过
    }

    if (e.boardCount == 2) {                          // 二板：核心看强度，换手降为辅助
        const double sealRatio = e.sealToAmountRatio();  // 封单额占成交额比
        if (sealRatio < kMinSealOrderRatio) {         // 封单不足
            reason = QStringLiteral("二板封单/成交额 %1% < %2%，封板勉强")
                         .arg(sealRatio * 100, 0, 'f', 1).arg(kMinSealOrderRatio * 100, 0, 'f', 1);
            return false;                             // 淘汰
        }
        if (e.sealToFloatCapRatio() < kMinSealToFloatCap) {  // 封单占流通市值不足
            reason = QStringLiteral("二板封单/流通市值 %1% < %2%，锁仓不足")
                         .arg(e.sealToFloatCapRatio() * 100, 0, 'f', 2)
                         .arg(kMinSealToFloatCap * 100, 0, 'f', 2);
            return false;                             // 淘汰
        }
        if (k && k->candles.size() >= 2) {            // 有至少两根 K 线才比较量能
            const double prevVol = k->candles[k->candles.size() - 2].volume;  // 首板成交量
            const double todayVol = k->candles.back().volume;                 // 二板成交量
            if (prevVol > 0 && todayVol > 0) {        // 量能有效才计算倍数
                const double vr = todayVol / prevVol; // 二板/首板 量能倍数
                if (vr < kMinVolRatioVsPrevBoard && !kAllowShrinkVolume) {  // 缩量且不允许缩量
                    reason = QStringLiteral("二板量能萎缩至首板 %1 倍，接力资金不足").arg(vr, 0, 'f', 2);
                    return false;                     // 淘汰
                }
                if (vr > kMaxVolRatioVsPrevBoard) {   // 放量过大＝分歧大
                    reason = QStringLiteral("二板量能放大至首板 %1 倍(>%2)，分歧过大")
                                 .arg(vr, 0, 'f', 2).arg(kMaxVolRatioVsPrevBoard, 0, 'f', 2);
                    return false;                     // 淘汰
                }
            }
        }
        return true;                                  // 二板通过
    }

    if (e.boardCount >= kMinHeightForMainUp) {        // 三板及以上：看龙头地位
        if (e.boardCount > kMaxMainUpHeight) {        // 高度超限（高位加速区）
            reason = QStringLiteral("%1 板属高位加速区，坚决不碰").arg(e.boardCount);
            return false;                             // 淘汰
        }
        if (c.sectorLimitCount < qMax(kMinSectorLimitCount, 3)) {  // 板块家数不足，缺带队效应
            reason = QStringLiteral("%1 板但板块仅 %2 家涨停，缺乏带队效应")
                         .arg(e.boardCount).arg(c.sectorLimitCount);
            return false;                             // 淘汰
        }
    }
    return true;                                      // 通过
}

// 生成因子明细（含未通过项），供 UI 展开核对
void FilterPipeline::attachFactors(Candidate& c, const FilterContext& ctx) const
{
    const double capYi = c.entry.floatCap / 1e8;      // 流通市值（亿元）
    const Ind::TurnoverBand band = Ind::turnoverBandFor(capYi, c.entry.board);  // 对应换手档
    const bool hasBand = band.dangerTurnover > 0.0;   // 档位是否有效

    auto add = [&c](const QString& name, const QString& disp, bool passed, const QString& hint) {
        FactorHit f;                                  // 单条因子
        f.name = name; f.display = disp; f.passed = passed; f.hint = hint;  // 填字段
        c.factors.append(f);                          // 追加到候选
    };

    const int peers = c.sectorLimitCount;             // 同题材涨停家数
    const QString themeName = c.entry.theme.isEmpty() ? QStringLiteral("未分类") : c.entry.theme;  // 题材名
    add(QStringLiteral("题材效应"),
        QStringLiteral("%1（%2 家涨停，板块 %3%）")
            .arg(themeName).arg(peers).arg(c.entry.themeChangePct, 0, 'f', 2),
        peers >= kMinSectorLimitCount,                 // 是否达标
        QStringLiteral("要求同题材 ≥%1 家涨停").arg(kMinSectorLimitCount));

    if (c.entry.themeRank > 0) {                       // 有名次才展示身位
        add(QStringLiteral("题材身位"),
            c.entry.themeLeader ? QStringLiteral("第 1（龙头）")
                                : QStringLiteral("第 %1").arg(c.entry.themeRank),
            c.entry.themeLeader,                       // 只有龙头算通过
            QStringLiteral("同题材按 板数→首封时间→封单 排序"));
    }

    if (hasBand) {                                     // 有档位才展示换手
        add(QStringLiteral("换手率"),
            QStringLiteral("%1%").arg(c.entry.turnover, 0, 'f', 2),
            c.entry.turnover >= band.minTurnover && c.entry.turnover < band.dangerTurnover,  // 落在合格区间
            QStringLiteral("合格 ≥%1% / 黄金 %2-%3% / 危险 ≥%4%")
                .arg(band.minTurnover, 0, 'f', 0)
                .arg(band.goodTurnover, 0, 'f', 0)
                .arg(band.bestTurnover, 0, 'f', 0)
                .arg(band.dangerTurnover, 0, 'f', 0));
    }

    if (c.volRatio5 > 0) {                             // 有量比才展示
        add(QStringLiteral("量比(5日)"), QStringLiteral("%1x").arg(c.volRatio5, 0, 'f', 2),
            c.volRatio5 >= kMinVolRatio5 && c.volRatio5 <= kMaxVolRatio5,   // 落在区间
            QStringLiteral("要求 %1~%2x").arg(kMinVolRatio5, 0, 'f', 1).arg(kMaxVolRatio5, 0, 'f', 1));
    }

    if (c.entry.firstSealTime.isValid()) {             // 有首封时间才展示
        const int m = c.entry.firstSealTime.hour() * 60 + c.entry.firstSealTime.minute();  // 换算分钟
        add(QStringLiteral("首次封板"), c.entry.firstSealTime.toString("HH:mm:ss"),
            m <= kMaxFirstSealMinute,                  // 是否早于截止时间
            QStringLiteral("要求 ≤%1:%2").arg(kMaxFirstSealMinute / 60)
                .arg(kMaxFirstSealMinute % 60, 2, 10, QChar('0')));
    }

    add(QStringLiteral("炸板次数"), QString::number(c.entry.breakCount),   // 炸板次数恒展示
        c.entry.breakCount <= kMaxBreakCountFirst,
        QStringLiteral("要求 ≤%1").arg(kMaxBreakCountFirst));

    add(QStringLiteral("封单/成交额"),                                     // 封单强度
        QStringLiteral("%1%").arg(c.entry.sealToAmountRatio() * 100, 0, 'f', 1),
        c.entry.sealToAmountRatio() >= kMinSealOrderRatio,
        QStringLiteral("要求 ≥%1%").arg(kMinSealOrderRatio * 100, 0, 'f', 0));

    add(QStringLiteral("连板高度"), QStringLiteral("%1 板").arg(c.entry.boardCount),  // 高度
        c.entry.boardCount <= kMaxMainUpHeight,
        QStringLiteral("%1 板以上坚决不碰").arg(kMaxMainUpHeight));

    add(QStringLiteral("流通市值"), QStringLiteral("%1 亿").arg(capYi, 0, 'f', 1),    // 市值区间
        capYi >= kMinFloatCapYi && capYi <= kMaxFloatCapYi,
        QStringLiteral("要求 %1~%2 亿").arg(kMinFloatCapYi).arg(kMaxFloatCapYi));

    if (c.limitCount20 > 0 || c.entry.boardCount > 1) {  // 有基因数据或连板才展示
        add(QStringLiteral("涨停基因(20日)"), QStringLiteral("%1 次").arg(c.limitCount20),
            c.limitCount20 >= kMinLimitCount20,
            QStringLiteral("要求 ≥%1 次").arg(kMinLimitCount20));
    }

    if (c.maBull > 0) {                                // 有均线强度才展示
        add(QStringLiteral("均线多头"), QStringLiteral("%1%").arg(c.maBull * 100, 0, 'f', 0),
            c.maBull >= 0.5, QStringLiteral("MA5>MA10>MA20 且价站上MA5"));
    }

    Q_UNUSED(ctx);                                     // ctx 当前未参与因子生成，显式忽略
}

namespace {
// 把所有 K 线截断到「本轮回放的交易日」再算指标，避免 MA5/量比/基因错位到最新一天
FilterContext sliceContext(const FilterContext& ctx, const QDate& asOf)
{
    if (!asOf.isValid()) return ctx;              // 日期无效则原样返回
    FilterContext out = ctx;                      // 复制上下文
    out.klines.clear();                           // 重建 K 线表
    for (auto it = ctx.klines.constBegin(); it != ctx.klines.constEnd(); ++it) {  // 逐只截断
        KlineSeries s = it.value().sliceTo(asOf);  // 截到目标交易日
        if (s.valid()) out.klines.insert(it.key(), s);   // 数据不含该日则丢弃，宁可不算
    }
    return out;                                   // 返回截断后的上下文
}
} // namespace

// 主流程：L0 闸门 → L1 硬排除 → L2 题材 → L3 分层量价 → L4 评分/身位排序
QVector<Candidate> FilterPipeline::run(const QVector<LimitEntry>& limitUpPool,
                                       const FilterContext& ctxRaw)
{
    m_funnel = Funnel();                          // 重置漏斗
    m_funnel.input = limitUpPool.size();          // 输入总数
    m_passedPreGate.clear();                      // 清空闸门前名单
    m_nearMiss.clear();                           // 清空近失名单
    m_rejects.clear();                            // 清空淘汰记录

    FilterContext ctx = sliceContext(ctxRaw, ctxRaw.regime.date);  // 先按决策日截断 K 线
    ctx.themeCount = buildThemeCounts(limitUpPool, ctx.themesOfStock);  // 计算题材涨停家数
    QVector<Candidate> passed;                    // 通过全部筛选的候选
    Scorer scorer;                                // 评分器

    const bool gated = !ctx.regime.tradeAllowed;  // 情绪是否不允许出手
    if (gated) {                                  // 拦截时仍跑完整管线以填充漏斗
        Q_EMIT progress(QStringLiteral("[L0 情绪判定] %1 —— 本轮不产生候选")
                            .arg(ctx.regime.note));
    }

    for (const LimitEntry& e : limitUpPool) {     // 逐只过管线
        Candidate c;                              // 当前候选
        QString reason;                           // 淘汰原因

        if (!passHardFilter(e, ctx, c, reason)) {      // L1 硬排除
            c.rejectReason = reason;                   // 记录原因
            m_rejects.append({ e.code, e.name, QStringLiteral("L1 硬排除"), reason });  // 入淘汰表
            continue;                                  // 下一只
        }
        ++m_funnel.afterHardFilter;               // L1 通过计数

        if (!passSector(e, ctx, c, reason)) {          // L2 题材效应
            c.rejectReason = reason;                   // 记录原因
            m_rejects.append({ e.code, e.name, QStringLiteral("L2 题材效应"), reason });
            continue;                                  // 下一只
        }
        ++m_funnel.afterSector;                   // L2 通过计数

        if (!passQuant(e, ctx, c, reason)) {           // L3 分层量价
            c.rejectReason = reason;                   // 记录原因
            attachFactors(c, ctx);                     // 保留因子明细
            c.score = scorer.score(c);                 // 保留评分
            c.grade = c.score >= kGradeA ? QStringLiteral("A")     // 评级
                     : c.score >= kGradeB ? QStringLiteral("B") : QStringLiteral("C");
            m_nearMiss.append(c);                      // 进第二梯队
            m_rejects.append({ e.code, e.name, QStringLiteral("L3 分层量价"), reason });
            continue;                                  // 下一只
        }
        ++m_funnel.afterQuant;                    // L3 通过计数

        auto kIt = ctx.klines.constFind(e.code);  // 查 K 线
        c.riskFlags = Risk::evaluate(c.entry, c.snapshot,                      // 评估风险标记
                                     kIt != ctx.klines.constEnd() ? &kIt.value() : nullptr);

        passed.append(c);                         // 全部通过，暂存（因子明细稍后统一生成）
    }

    for (Candidate& c : passed) {                 // L4 评分
        c.score = scorer.score(c);                // 综合评分
        if (!c.riskFlags.isEmpty() && kRiskPenaltyPerFlag > 0.0) {   // 软性风险扣分
            const int n = qMin(c.riskFlags.size(), kMaxRiskPenaltyFlags);  // 参与扣分的条数
            c.score = qMax(0.0, c.score - n * kRiskPenaltyPerFlag);        // 逐条扣分
        }
        c.grade = c.score >= kGradeA ? QStringLiteral("A")             // 评级 A
                : c.score >= kGradeB ? QStringLiteral("B")             // 评级 B
                                                 : QStringLiteral("C"); // 评级 C
    }

    {                                             // L4.5 同题材身位排序
        QHash<QString, QVector<int>> groups;      // 题材 → 候选下标
        for (int i = 0; i < passed.size(); ++i)
            if (!passed[i].entry.theme.isEmpty()) groups[passed[i].entry.theme].append(i);  // 按题材分组

        for (auto it = groups.begin(); it != groups.end(); ++it) {   // 逐题材排序
            QVector<int> idxs = it.value();                          // 该题材内的候选下标
            std::sort(idxs.begin(), idxs.end(), [&passed](int a, int b) {
                const LimitEntry& ea = passed[a].entry;
                const LimitEntry& eb = passed[b].entry;
                if (ea.boardCount != eb.boardCount) return ea.boardCount > eb.boardCount;  // 板数优先
                if (ea.firstSealTime.isValid() != eb.firstSealTime.isValid())
                    return ea.firstSealTime.isValid();                                     // 有封板时间者优先
                if (ea.firstSealTime != eb.firstSealTime) return ea.firstSealTime < eb.firstSealTime;  // 封板更早优先
                return ea.sealOrder > eb.sealOrder;                                        // 封单更大优先
            });
            for (int r = 0; r < idxs.size(); ++r) {                  // 赋身位
                Candidate& c = passed[idxs.at(r)];                   // 该名次候选
                c.entry.themeRank = r + 1;                          // 身位 1 = 龙头
                c.entry.themeLeader = (r == 0);                     // 第一名标记为龙头
                if (r == 0 && c.entry.themeLimitCount >= 3)        // 题材确实成势才加龙头分
                    c.score = qMin(100.0, c.score + kLeaderBonusScore);
            }
        }
    }

    std::sort(passed.begin(), passed.end(), [](const Candidate& a, const Candidate& b) {
        return a.score > b.score;                  // 按综合评分降序
    });
    if (passed.size() > kMaxCandidates) passed.resize(kMaxCandidates);  // 截断到上限

    for (Candidate& c : passed) attachFactors(c, ctx);   // 统一生成因子明细（含题材身位）

    m_passedPreGate = passed;                      // 保留闸门前名单（退潮期也要能回答「盯哪只」）

    std::sort(m_nearMiss.begin(), m_nearMiss.end(), [](const Candidate& a, const Candidate& b) {
        return a.score > b.score;                  // 近失名单按评分降序
    });
    if (m_nearMiss.size() > m_maxNearMiss) m_nearMiss.resize(m_maxNearMiss);  // 截断到容量上限

    m_funnel.output = gated ? 0 : passed.size();   // 输出计数（拦截时为 0）
    if (gated) passed.clear();          // 闸门拦截：不输出候选
    Q_EMIT progress(QStringLiteral("[筛选完成] 输入 %1 → 硬过滤 %2 → 板块 %3 → 量价 %4 → 输出 %5")
        .arg(m_funnel.input).arg(m_funnel.afterHardFilter)
        .arg(m_funnel.afterSector).arg(m_funnel.afterQuant).arg(m_funnel.output));
    return passed;                                 // 返回最终候选
}

// 单只标的评估：只跑三层判定，不参与排序（供 UI 点开一只票时展示）
Candidate FilterPipeline::evaluate(const LimitEntry& entry, const FilterContext& ctxRaw) const
{
    Candidate c;                                  // 结果
    QString reason;                               // 淘汰原因
    const FilterContext ctx = sliceContext(ctxRaw, ctxRaw.regime.date);  // 按决策日截断
    if (!passHardFilter(entry, ctx, c, reason)) c.rejectReason = reason;        // L1
    else if (!passSector(entry, ctx, c, reason)) c.rejectReason = reason;       // L2
    else if (!passQuant(entry, ctx, c, reason)) c.rejectReason = reason;        // L3
    attachFactors(c, ctx);                        // 生成因子明细
    return c;                                     // 返回
}
