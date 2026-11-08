#include "core/TradePlan.h"              // 本模块声明
#include "core/Auction.h"                // 生成次日竞价价格带
#include "core/ChipDistribution.h"        // 筹码分布：支撑 / 压力密集峰
#include "core/Indicators.h"             // MA5 等指标
#include <QtMath>                        // qBound / qMax / qMin
#include <algorithm>                     // std::stable_sort
#include <cmath>                         // std::isfinite / std::round

// 信号级别 → 中文名
QString signalLevelName(int level)
{
    switch (level) {                     // 逐一映射级别
    case 4:  return QStringLiteral("主攻");   // 最高强度
    case 3:  return QStringLiteral("建仓");   // 正常建仓
    case 2:  return QStringLiteral("试仓");   // 小仓试探
    case 1:  return QStringLiteral("观察");   // 只看不做
    default: return QStringLiteral("-");      // 无信号
    }
}

// 一行推荐理由：题材身位 + 板数 + 评级 + 情绪档位（界面与邮件共用同一文案）
QString TradePlan::reasonLine() const
{
    QStringList parts;                                  // 理由片段
    const QString t = theme.isEmpty() ? sector : theme; // 题材优先，缺失退回行业
    if (!t.isEmpty()) {                                 // 有板块归属才写
        if (themeLeader)          parts << QStringLiteral("%1·龙头").arg(t);        // 同题材第一
        else if (themeRank > 0)   parts << QStringLiteral("%1·第%2").arg(t).arg(themeRank);  // 非龙头给身位
        else                      parts << t;           // 无名次则只写题材
    }
    if (!tag.isEmpty() && !(themeLeader && tag == QStringLiteral("龙头")))
        parts << tag;                                   // 板数标签（避免与「龙头」重复）
    if (!grade.isEmpty()) parts << QStringLiteral("%1 级").arg(grade);   // 评级
    if (signalLevel > 0 && signalLevelName(signalLevel) != QStringLiteral("-"))
        parts << signalLevelName(signalLevel);          // 情绪执行档位
    if (parts.isEmpty()) parts << QStringLiteral("情绪与结构达标");      // 兜底
    return parts.join(QStringLiteral(" · "));           // 拼接为一行
}

// 状态短标签：把冗长的 note 压成一眼能懂的词
QString TradePlan::statusTag() const
{
    // 状态标签是「扫一眼」用的短词，一律 ≤ 5 个字：
    // 表格里被截断的标签等于没有标签，完整说明在单元格 tooltip（note）里。
    if (actionable) return QStringLiteral("可执行");     // 可下单
    if (note.startsWith(QStringLiteral("竞价否决")))     return QStringLiteral("竞价否决");
    if (note.startsWith(QStringLiteral("结构止损过远"))) return QStringLiteral("止损过远");
    if (note.contains(QStringLiteral("名额已满")))       return QStringLiteral("名额已满");
    if (note.contains(QStringLiteral("持仓数已达上限"))) return QStringLiteral("持仓已满");
    if (note.contains(QStringLiteral("剩余仓位额度不足"))) return QStringLiteral("仓位不足");
    if (note.contains(QStringLiteral("避免同题材双杀"))) return QStringLiteral("题材冲突");
    if (note.contains(QStringLiteral("情绪信号未触发"))) return QStringLiteral("情绪未触发");
    // 「因子全达标但情绪环境不允许」：扫表时最需要知道的是**被谁拦**，不是因子是否达标
    if (note.contains(QStringLiteral("因子全达标")))     return QStringLiteral("情绪拦截");
    if (note.startsWith(QStringLiteral("仅观察：")))     return QStringLiteral("近失标");
    if (note.contains(QStringLiteral("未过·"))) {        // 「未过·评分门：…」抽出卡住的那道门
        const int s = note.indexOf(QStringLiteral("未过·")) + 3;
        const int e = note.indexOf(QChar(0xFF1A), s);    // 全角冒号
        const QString gate = note.mid(s, e > s ? e - s : -1);
        if (gate.isEmpty()) return QStringLiteral("未过门槛");
        const QString tag = QStringLiteral("未过") + gate;
        return tag.length() <= 5 ? tag : tag.left(4) + QStringLiteral("…");
    }
    if (note.isEmpty()) return QStringLiteral("—");
    return note.left(5);                                 // 兜底截断（≤5 字）
}

namespace {                              // 交易计划阈值（固定口径）

constexpr double kMaxRiskPctMain      = 5.0;   // 10cm 单笔最大止损空间（%）
constexpr double kMaxRiskPct20cm      = 7.0;   // 20cm 单笔最大止损空间（%）
constexpr double kMinProfitOverBuyHigh = 0.02; // 止盈下沿至少高于买入上沿 2%
constexpr double kMaxSinglePositionPct = 20.0; // 单票仓位上限
constexpr bool   kStrictRiskFilter     = true; // 止损空间超限时降级为观察

// ---- 筹码分布与量能口径（固定阈值）----
constexpr double kChipSupportMinRatio = 0.92;  // 支撑密集峰离现价不得低于 8%（太远说明筹码断层）
constexpr double kVolDryRatio         = 0.80;  // 缩量：当日量 < 前 5 日均量 0.8 倍
constexpr double kVolBreakRatio       = 1.50;  // 放量：当日量 ≥ 前 5 日均量 1.5 倍（支撑破位确认）
constexpr double kVolConfirmLo        = 1.50;  // 温和放量确认下限（量比 1.5~2 为佳）
constexpr double kVolConfirmHi        = 2.00;  // 温和放量确认上限
constexpr double kStopCushionNormal   = 0.015; // 常规破位缓冲 1.5%
constexpr double kStopCushionDry      = 0.025; // 缩量破位缓冲 2.5%（假跌破空间）

} // namespace

namespace Plan {                         // 交易计划构建命名空间

// 价格四舍五入到分
double round2(double v)
{
    if (!std::isfinite(v)) return 0.0;   // NaN/Inf 统一归零，避免脏数据往下游传
    return std::round(v * 100.0) / 100.0;  // 放大 100 倍取整再缩回
}

// 情绪阶段 → 总仓位上限（%）
double phasePositionCap(MarketPhase phase)
{
    switch (phase) {                     // 按阶段给上限
    case MarketPhase::MainUp:   return 60.0;   // 主升：总仓 60%
    case MarketPhase::Climax:   return 30.0;   // 高潮：减仓至 30%
    case MarketPhase::Recovery: return 20.0;   // 回暖：小仓试错 20%
    case MarketPhase::Retreat:  return 0.0;    // 退潮：空仓
    case MarketPhase::IcePoint: return 0.0;    // 冰点：空仓
    default:                    return 0.0;    // 未知阶段不开仓
    }
}

// 构建单只标的的交易计划：锚点 → 买入 → 止盈 → 止损 → 盈亏比 → 成交率 → 竞价 → 仓位 → 文案
TradePlan build(const Candidate& c, const KlineSeries* kline,
                const MarketRegime& regime, bool actionable)
{
    TradePlan tp;                        // 待填充的计划
    tp.code    = c.entry.code;           // 代码
    tp.name    = c.entry.name;           // 名称
    tp.sector  = c.entry.sector;         // 行业
    tp.theme   = c.entry.theme;          // 题材
    tp.themeRank = c.entry.themeRank;    // 题材内身位
    tp.themeLeader = c.entry.themeLeader;  // 是否题材龙头
    tp.score   = c.score;                // 综合评分
    tp.grade   = c.grade;                // 评级
    tp.actionable = actionable;          // 闸门是否放行
    tp.riskFlags = c.riskFlags;          // 风险标记从候选搬过来，供 UI 角标与文案使用

    const Board board = c.entry.board;                        // 板块决定涨跌幅比例
    const double ratio = boardLimitRatio(board);              // 该板块单日涨跌幅
    double limitPx = c.entry.limitPrice > 0.0 ? c.entry.limitPrice : c.entry.price;  // 锚点：优先涨停价
    if (limitPx <= 0.0) return tp;                            // 锚点无效则返回空计划

    // 卡片上的「当前价格」用行情侧的真实最新价，不用计划价反推（避免把买点伪装成现价）
    if (c.entry.price > 0.0)            tp.currentPrice = c.entry.price;
    else if (c.snapshot.price > 0.0)    tp.currentPrice = c.snapshot.price;
    else                                tp.currentPrice = limitPx;

    if (c.entry.boardCount >= 3)      tp.tag = QStringLiteral("龙头");   // 三板以上视为龙头
    else if (c.entry.boardCount == 2)  tp.tag = QStringLiteral("二板");  // 二板
    else                                tp.tag = QStringLiteral("首板"); // 首板

    double ma5 = 0.0;                    // 5 日均线（换算成真实报价）
    double prevClose = 0.0;              // 涨停日的前收
    double todayLow = 0.0;               // 涨停当日最低价
    bool hasKline = false;               // 是否拿到可用 K 线
    QVector<Candle> raw;                 // 真实价口径序列（筹码分布与主动性代理都要用）
    KlineSeries k;                       // 截断到决策日的 K 线（量比等派生计算用）
    if (kline && kline->valid()) {       // 有 K 线才做派生计算
        k = kline->sliceTo(regime.date);                            // 先截断到决策日，防止未来函数
        raw = k.hasRaw() ? k.rawCandles : k.candles;                // 真实价口径序列
        if (k.candles.size() >= 6 && raw.size() >= 2) {             // 至少够算 MA5 且有两根原始 K
            hasKline = true;             // 标记 K 线可用
            QVector<double> closes;      // 收盘价序列
            closes.reserve(k.candles.size());  // 预留容量
            for (const Candle& cd : k.candles) closes.append(cd.close);  // 逐根提取收盘
            const QVector<double> s5 = Ind::sma(closes, 5);         // 计算 5 日均线
            if (!s5.isEmpty() && std::isfinite(s5.back()))          // 末值有效才使用
                ma5 = s5.back() * k.adjustFactor();   // 前复权价换算回真实报价口径
            prevClose = raw.at(raw.size() - 2).close;               // 倒数第二根为前收
            todayLow  = raw.back().low;                             // 最后一根最低价为当日最低
        }
    }
    if (prevClose <= 0.0) prevClose = limitPx / (1.0 + ratio);  // K 线缺失时由涨停价反推昨收

    const double closePx = (hasKline && !raw.isEmpty()) ? raw.back().close : limitPx;  // 决策日收盘价

    // ================= 筹码分布 · 量能 · 主动性：定价的第二套锚点 =================
    // 筹码分布用不复权真实价重建（与涨停价 / 成交价同口径），K 线已按决策日截断，无未来函数。
    const Chip::Distribution chip = Chip::build(raw, limitPx);
    const Chip::Peak* chipSupport  = chip.supportBelow(limitPx);   // 下方最近密集峰（支撑）
    const Chip::Peak* chipPressure = chip.pressureAbove(limitPx);  // 上方最近密集峰（压力）
    if (chipSupport) {
        tp.chipSupportLow  = chipSupport->low;
        tp.chipSupportHigh = chipSupport->high;
    }
    if (chipPressure) {
        tp.chipPressureLow  = chipPressure->low;
        tp.chipPressureHigh = chipPressure->high;
    }
    tp.chipProfitRatio = chip.profitRatio * 100.0;                // 获利盘比例（%）

    // 量比：当日量 / 前 5 日均量 —— 缩量衰竭、放量击穿、量价背离的统一判据
    const double vr = hasKline ? Ind::volRatio(k, 5) : 0.0;
    tp.volRatio = vr;

    // 日线口径的「主动性」代理（分时外盘 / 内盘、逐笔主动买卖的日线等价物）：
    //   主动买强度 = (close-low)/(high-low)  收盘越靠近最高，买方越主动
    //   日均价 VWAP = amount/volume          收盘低于均价 = 全天弱势（反抽无力站上分时均价线）
    double activeBuy = 0.0;                                        // 0~1
    double vwap = 0.0;                                             // 当日均价
    bool closeAboveVwap = true;                                    // 收盘是否站上日均价
    if (hasKline && !raw.isEmpty()) {
        const Candle& last = raw.back();
        const double range = last.high - last.low;
        if (range > 0.0) activeBuy = (last.close - last.low) / range;
        if (last.amount > 0.0 && last.volume > 0.0) vwap = last.amount / (last.volume * 100.0);
        else vwap = (last.high + last.low + last.close + last.open) * 0.25;
        closeAboveVwap = last.close >= vwap;
    }

    double pullbackTol = 0.02;   // 回踩容忍度：首板 2%
    double chaseCap   = 0.03;   // 追高容忍度：首板 3%
    if (c.entry.boardCount == 2) { pullbackTol = 0.025; chaseCap = 0.02; }  // 二板收紧两端
    if (c.entry.boardCount >= 3) { pullbackTol = 0.03;  chaseCap = 0.0;  }  // 三板以上不允许追高

    // ---- 买入区间：承接位为锚，低于承接位即视为筹码支撑失效 ----
    // 承接位优先取「当前价下方最近的筹码密集峰上沿」（抛压衰竭、买盘介入的真实位置）；
    // 筹码断层（离现价超过 8%）时退回 MA5 承接；再退回涨停价 ± 容忍度。
    const double baseLow = limitPx * (1.0 - pullbackTol);            // 退化口径：涨停价下方回踩位
    double support = 0.0;                                            // 承接位
    QString supportDesc;                                             // 承接位来源
    if (tp.chipSupportHigh > 0.0 && tp.chipSupportHigh >= limitPx * kChipSupportMinRatio
        && tp.chipSupportHigh < limitPx) {
        support = tp.chipSupportHigh;
        supportDesc = QStringLiteral("筹码密集峰上沿 %1 元").arg(round2(support), 0, 'f', 2);
    } else if (ma5 > 0.0 && ma5 > baseLow && ma5 < limitPx) {
        support = ma5;
        supportDesc = QStringLiteral("MA5 %1 元").arg(round2(support), 0, 'f', 2);
    }

    const double buyHighRaw = limitPx * (1.0 + chaseCap);            // 上沿：追高上限（与原口径一致）
    double buyLow = (support > 0.0) ? support : baseLow;
    if (support > 0.0) {
        tp.buyBasis = QStringLiteral("涨停价 %1 元；下沿 %2 元（%3，承接位，跌破即视为筹码支撑失效）；"
                                     "上沿为追高上限 %4 元")
            .arg(round2(limitPx), 0, 'f', 2)
            .arg(round2(buyLow), 0, 'f', 2)
            .arg(supportDesc)
            .arg(round2(buyHighRaw), 0, 'f', 2);
    } else {
        tp.buyBasis = QStringLiteral("涨停价 %1 元；下沿为涨停价下方 %2% 回踩承接位，上沿为追高上限 %3 元")
            .arg(round2(limitPx), 0, 'f', 2)      // 涨停价
            .arg(pullbackTol * 100, 0, 'f', 1)    // 回踩容忍度
            .arg(round2(buyHighRaw), 0, 'f', 2);  // 追高上限
    }
    if (!hasKline) {                 // K 线缺失时显式说明是估计值
        tp.buyBasis += QStringLiteral("；日 K 未就绪，区间为保守估计（K 线到位后自动重算）");
    }
    tp.buyLow  = round2(buyLow);         // 记录买入下沿
    tp.buyHigh = round2(buyHighRaw);     // 记录买入上沿

    // ---- 量能过滤：区分「抛压衰竭」与「放量击穿」 ----
    // 缩量回踩 = 抛压有限（支撑可信）；温和放量回升（量比 1.5~2 且收盘站上承接位、日均价）= 买盘介入确认；
    // 放量下破承接位且买盘不接力 = 支撑正被主动卖盘击穿，此时不应买入。
    if (support > 0.0 && vr > 0.0) {
        const bool brokeSupport = closePx < support;               // 收盘跌破承接位
        if (vr >= kVolBreakRatio && brokeSupport && activeBuy < 0.5) {
            tp.supportQuality = QStringLiteral("放量击穿：量比 %1，收盘跌破承接位且买盘不接力，支撑承压")
                                 .arg(vr, 0, 'f', 2);
            tp.riskFlags << QStringLiteral("筹码支撑放量击穿");
            // 「跌到支撑位却放量」= 支撑正被主动卖盘击穿，此时不应买入 → 降级观察
            if (tp.actionable) {
                tp.actionable = false;
                tp.note = QStringLiteral("支撑放量击穿（量比 %1），抛压未衰竭，降级观察").arg(vr, 0, 'f', 2);
            }
        } else if (vr >= kVolConfirmLo && vr <= kVolConfirmHi && !brokeSupport && closeAboveVwap) {
            tp.supportQuality = QStringLiteral("温和放量确认：量比 %1（1.5~2 倍），收盘站上承接位且高于日均价 %2 元，买盘介入真实")
                                 .arg(vr, 0, 'f', 2).arg(round2(vwap), 0, 'f', 2);
        } else if (vr < kVolDryRatio) {
            tp.supportQuality = QStringLiteral("缩量衰竭：量比 %1，回踩缩量说明抛压有限").arg(vr, 0, 'f', 2);
        } else {
            tp.supportQuality = QStringLiteral("量能中性：量比 %1").arg(vr, 0, 'f', 2);
        }
        tp.buyBasis += QStringLiteral("；%1").arg(tp.supportQuality);
    } else if (!chip.valid) {
        tp.supportQuality = QStringLiteral("筹码分布数据不足，按涨停价 ± 容忍度保守估计");
    }

    const double buyMid = (tp.buyLow + tp.buyHigh) * 0.5;  // 买入中枢
    double tpLow  = qMax(buyMid * 1.03, tp.buyHigh * (1.0 + kMinProfitOverBuyHigh));  // 止盈下沿必须高于买入上沿
    double tpHigh = round2(limitPx * (1.0 + ratio));       // 止盈上沿＝次日涨停价（理论上限）
    // 筹码压力区：现实兑现位优先于涨停价。上方最近密集峰即前期套牢盘的解套抛压带，
    // 卖在压力区下沿之上、目标位取压力区上沿，比"等涨停价"更贴近可执行。
    if (tp.chipPressureHigh > tpLow && tp.chipPressureLow > 0.0 && tp.chipPressureLow < tpHigh) {
        tpHigh = round2(qMin(tp.chipPressureHigh, tpHigh));
        tpLow  = round2(qMax(tpLow, tp.chipPressureLow));
    }
    if (tpHigh <= tpLow) tpHigh = round2(tpLow * 1.02);   // 极端情况下保证区间有效
    tp.takeProfitLow  = round2(tpLow);            // 记录止盈下沿
    tp.takeProfitHigh = round2(tpHigh);           // 记录止盈上沿
    tp.tpBasis = QStringLiteral("下沿 %1 元（按买入上沿 %2 元 +%3% 计算，确保上沿成交也有正收益）；"
                                "上沿 %4 元为次日涨停价（理论上限，不作兑现假设）")
        .arg(tp.takeProfitLow, 0, 'f', 2)         // 止盈下沿
        .arg(tp.buyHigh, 0, 'f', 2)               // 买入上沿
        .arg(kMinProfitOverBuyHigh * 100, 0, 'f', 1)  // 最小溢价率
        .arg(tp.takeProfitHigh, 0, 'f', 2);       // 止盈上沿
    if (tp.chipPressureHigh > tpLow && tp.chipPressureLow > 0.0) {
        tp.tpBasis += QStringLiteral("；上沿已收敛到上方筹码压力密集区（%1~%2 元，前期套牢盘解套抛压带），"
                                     "进入该区若量能不能持续放大应减仓")
                        .arg(round2(tp.chipPressureLow), 0, 'f', 2)
                        .arg(round2(tp.chipPressureHigh), 0, 'f', 2);
    }

    // ---- 量价背离预警：压力区内量能跟不上 + 涨幅收窄 / 长上影 → 派发嫌疑 ----
    // 分时「假拉升」无法用日线直接观测，这里用日线等价物：涨幅收窄（当日涨幅低于前 5 日平均）
    // 与长上影（上影 > 实体 2 倍且 > 3%）。两者与缩量同时出现即视为顶背离。
    if (vr > 0.0 && vr < kVolDryRatio && hasKline && raw.size() >= 6) {
        const Candle& last = raw.back();
        const double body = std::fabs(last.close - last.open);
        const double upperShadow = last.high - qMax(last.open, last.close);
        const double chgPct = prevClose > 0.0 ? (last.close - prevClose) / prevClose * 100.0 : 0.0;
        double avgChg = 0.0;                                     // 前 5 日平均涨幅（绝对值）
        for (int i = raw.size() - 6; i < raw.size() - 1; ++i) {
            const double pc = raw.at(i - 1).close;
            if (pc > 0.0) avgChg += std::fabs((raw.at(i).close - pc) / pc * 100.0);
        }
        avgChg /= 5.0;
        const bool narrow = avgChg > 0.0 && std::fabs(chgPct) < avgChg * 0.6;   // 涨幅收窄
        const bool longUpper = upperShadow > body * 2.0 && upperShadow > last.close * 0.03;  // 长上影
        if (narrow || longUpper) {
            const QStringList why = { narrow ? QStringLiteral("涨幅收窄") : QString(),
                                      longUpper ? QStringLiteral("长上影") : QString() };
            QStringList kept;
            for (const QString& w : why) if (!w.isEmpty()) kept << w;
            tp.divergenceNote = QStringLiteral("量价背离：量比 %1（<0.8）且%2，筹码压力区内买盘承接不足，警惕派发")
                                 .arg(vr, 0, 'f', 2).arg(kept.join(QStringLiteral("、")));
            tp.riskFlags << QStringLiteral("量价背离");
        }
    }

    const double maxRiskPct = (board == Board::ChiNext || board == Board::StarMarket)
                                ? kMaxRiskPct20cm : kMaxRiskPctMain;  // 双创与主板风险上限不同
    // ---- 结构止损锚：止损的核心是「逻辑是否被破坏」，而不是固定百分比 ----
    // ① 筹码支撑密集峰下沿（买入逻辑依托于此，跌破即失效）
    // ② 涨停当日最低价（炸板即走）
    // ③ 涨停价下方 5% 保守位（无 K 线时的退化口径）
    double slHigh = 0.0;                 // 止损上沿：破位即走的位置
    QString anchorDesc;
    if (tp.chipSupportLow > 0.0 && tp.chipSupportLow < closePx) {
        slHigh = tp.chipSupportLow;
        anchorDesc = QStringLiteral("筹码支撑密集峰下沿（跌破即买入逻辑失效）");
    } else if (hasKline && todayLow > 0.0 && todayLow < limitPx) {
        slHigh = todayLow;               // 跌破当日最低即封板失败
        anchorDesc = QStringLiteral("涨停当日最低价（炸板即走）");
    } else {
        slHigh = limitPx * 0.95;         // 无 K 线时退化为涨停价下方 5%
        anchorDesc = QStringLiteral("涨停价下方 5% 保守位");
        if (!hasKline) anchorDesc += QStringLiteral("；日 K 未就绪，为保守估计");
    }
    if (slHigh > tp.buyLow * 0.995) slHigh = tp.buyLow * 0.995;  // 止损不得高于买入下沿（否则买即亏）

    const double naturalSlHigh = slHigh;             // 记录结构止损原始位置
    const double riskFloor = buyMid * (1.0 - maxRiskPct / 100.0);  // 按最大可承受风险算出的止损下限
    if (slHigh < riskFloor) slHigh = riskFloor;      // 振幅过大时上移止损，控制单笔风险

    tp.stopLossHigh = round2(slHigh);                // 记录止损上沿

    // 破位缓冲按量能区分：缩量破位可能是假跌破 → 放宽到 2.5%；放量破位 → 只留 1.5% 立即确认
    const double cushion = (vr > 0.0 && vr < kVolDryRatio) ? kStopCushionDry : kStopCushionNormal;
    tp.stopLossLow  = round2(slHigh * (1.0 - cushion));   // 下沿留缓冲带

    tp.slBasis = QStringLiteral("上沿 %1 元 = %2；下沿 %3 元为破位无条件止损（%4）")
        .arg(round2(tp.stopLossHigh), 0, 'f', 2)
        .arg(anchorDesc)
        .arg(round2(tp.stopLossLow), 0, 'f', 2)
        .arg((cushion > kStopCushionNormal)
                 ? QStringLiteral("缩量破位，缓冲放宽至 2.5%%，防假跌破")
                 : QStringLiteral("留 1.5% 缓冲防插针"));

    // ---- 支撑破位 + 放量 → 止损条件当场成立：新开仓逻辑已消失 ----
    if (tp.chipSupportLow > 0.0 && closePx < tp.chipSupportLow && vr >= kVolBreakRatio) {
        tp.riskFlags << QStringLiteral("筹码支撑破位放量");
        const QString msg = QStringLiteral("筹码支撑密集峰下沿 %1 元已放量跌破（量比 %2），买入逻辑失效")
                                .arg(round2(tp.chipSupportLow), 0, 'f', 2).arg(vr, 0, 'f', 2);
        tp.riskNote = tp.riskNote.isEmpty() ? msg : (tp.riskNote + QStringLiteral(" ｜ ") + msg);
        if (tp.actionable) {
            tp.actionable = false;
            tp.note = QStringLiteral("支撑破位放量，降级观察：%1").arg(msg);
        }
    }

    const double slMid = (tp.stopLossHigh + tp.stopLossLow) * 0.5;  // 止损中枢
    tp.riskPct = buyMid > 0.0 ? (buyMid - slMid) / buyMid * 100.0 : 0.0;  // 买入中枢到止损中枢的距离 %
    const double naturalSlMid = naturalSlHigh * (1.0 - cushion * 0.5);  // 原始结构止损中枢
    const double naturalRiskPct = buyMid > 0.0 ? (buyMid - naturalSlMid) / buyMid * 100.0 : 0.0;  // 原始风险空间 %
    tp.riskTooHigh = hasKline && naturalRiskPct > maxRiskPct + 0.05;  // 仅在有真实 K 线时才判「结构止损过远」
    if (tp.riskTooHigh) {                            // 超限时补一条可核对的说明
        tp.slBasis += QStringLiteral("；当日振幅过大（结构止损位 %1% > 上限 %2%），"
                                     "已按最大可承受风险上移至 %3 元")
                          .arg(naturalRiskPct, 0, 'f', 1)   // 原始风险空间
                          .arg(maxRiskPct, 0, 'f', 1)      // 允许上限
                          .arg(tp.stopLossHigh, 0, 'f', 2); // 上移后的止损位
    }

    const double tpMid = (tp.takeProfitLow + tp.takeProfitHigh) * 0.5;  // 止盈中枢
    const double denom = buyMid - slMid;             // 单位风险（分母）
    tp.riskReward    = denom > 0.0 ? (tp.takeProfitLow - buyMid) / denom : 0.0;  // 保守盈亏比
    tp.riskRewardMax = denom > 0.0 ? (tpMid - buyMid) / denom : 0.0;             // 乐观盈亏比

    {                                                // 成交概率：把「愿望仓位」折成「期望仓位」
        double p = 0.90;                             // 基准成交概率
        QStringList why;                             // 折减原因集合
        if (c.entry.isOneWordBoard) { p = 0.08; why << QStringLiteral("一字板"); }  // 一字板几乎买不到
        if (c.entry.isSecondSeal)   { p -= 0.15; why << QStringLiteral("秒板/T字"); }  // 秒板难排队
        if (c.entry.boardCount >= 4) {               // 高位板封单更易被抢
            p -= 0.10 * (c.entry.boardCount - 3);    // 每多一板再降 10%
            why << QStringLiteral("高位板");          // 记录原因
        }
        if (c.entry.firstSealTime.isValid()) {       // 有首封时间才判断封板早晚
            const int minute = c.entry.firstSealTime.hour() * 60 + c.entry.firstSealTime.minute();  // 换算分钟
            if (minute <= 575) { p -= 0.15; why << QStringLiteral("开盘即封"); }        // 09:35 前封死
            else if (minute >= 600) { p -= 0.10; why << QStringLiteral("封板偏晚"); }   // 10:00 后才封
        }
        const double seal = c.entry.sealToAmountRatio();  // 封单额占成交额比
        if (seal > 0.0 && seal < 0.15)      { p -= 0.15; why << QStringLiteral("封单薄"); }  // 封单过薄
        else if (seal > 0.0 && seal < 0.25) { p -= 0.08; }                                  // 封单一般
        if (c.entry.breakCount >= 2)      { p -= 0.12; why << QStringLiteral("多次开板"); }  // 反复开板
        else if (c.entry.breakCount == 1) { p -= 0.05; }                                    // 开板一次小幅折减
        if (c.entry.turnover > 30.0)      { p -= 0.08; why << QStringLiteral("换手过热"); }  // 换手过热易炸
        if (c.entry.limitPrice <= 0.0)    { p -= 0.05; }                                    // 涨停价缺失为估计值

        tp.fillProbability = qBound(0.05, p, 0.90);  // 概率限制在 5%~90%
        tp.fillNote = why.isEmpty()                  // 无折减原因说明结构正常
            ? QStringLiteral("封板结构正常，预计可按计划价成交")
            : QStringLiteral("%1，预计难以按计划价全额成交").arg(why.join(QStringLiteral(" / ")));  // 否则列出原因
    }

    {                                                // 次日竞价方案：把百分比换算成可直接核对的价格带
        const AuctionRule ar = Auction::ruleFor(c.entry);  // 生成竞价规则
        tp.auctionBasis = ar.basis;                                    // 规则说明
        tp.auctionBuyLow      = round2(prevClose * (1.0 + ar.minOpenPct / 100.0));  // 可接价格下沿
        tp.auctionBuyHigh     = round2(prevClose * (1.0 + ar.maxOpenPct / 100.0));  // 可接价格上沿
        tp.auctionAbandonPrice = round2(prevClose * (1.0 + ar.abandonPct / 100.0)); // 跌破即放弃价
    }

    if (!tp.actionable) {               // 闸门未放行 / 已被降级 → 不给仓位
        tp.positionPct = 0.0;             // 仓位归零
    } else {
        const double cap = phasePositionCap(regime.phase);          // 阶段总仓上限
        const double sectorFactor = c.sectorLimitCount >= 3 ? 1.0   // 板块涨停≥3 家：满系数
                                  : c.sectorLimitCount == 2 ? 0.75 : 0.5;  // 2 家 0.75，其余 0.5
        const double scoreFactor = qBound(0.0, (c.score - 50.0) / 50.0, 1.0);  // 评分归一化到 0~1
        const double perStock = cap / qMax(1, kConcentratedPositionCount);     // 按集中只数摊分
        double pos = perStock * (0.5 + 0.5 * scoreFactor) * sectorFactor;      // 摊分 × 评分 × 板块系数
        pos = qMin(pos, kMaxSinglePositionPct);                // 不超过单票上限
        if (tp.riskTooHigh) pos *= 0.5;              // 结构止损过远 → 轻仓
        if (c.entry.themeLeader) pos *= 1.2;         // 同题材身位第一 → 加码
        tp.positionPct = qBound(0.0, qMin(pos, kMaxSinglePositionPct), kMaxSinglePositionPct);  // 最终夹紧
    }

    if (!tp.actionable && !tp.note.isEmpty()) {
        // 前面已因「筹码支撑破位放量」降级，保留降级原因文案
    } else if (!actionable) {           // 闸门拦截
        tp.note = QStringLiteral("仅观察（今日不出手）");
    } else if (tp.riskTooHigh) {          // 结构止损过远
        if (kStrictRiskFilter) {          // 严格模式：直接降级为观察
            tp.actionable = false;        // 标记不可执行
            tp.positionPct = 0.0;         // 仓位归零
            tp.note = QStringLiteral("结构止损过远（%1%），降级为观察")
                          .arg(naturalRiskPct, 0, 'f', 1);  // 说明超限幅度
        } else {
            tp.note = QStringLiteral("结构止损过远，轻仓试错");    // 宽松模式：允许轻仓
        }
    } else if (c.entry.boardCount >= 3) {  // 高位接力
        tp.note = QStringLiteral("高位接力，仓位从轻");
    } else {
        tp.note = QStringLiteral("可执行");   // 常规可执行
    }

    // 合并候选自带的风险标记（此前已追加的筹码 / 量能标记必须保留，不能覆盖）
    for (const QString& f : c.riskFlags)
        if (!tp.riskFlags.contains(f)) tp.riskFlags << f;
    tp.riskNote = QStringLiteral("打板单日最大亏损可达 %1%（跌停且无法卖出），务必严格执行止损")
        .arg(ratio * 100, 0, 'f', 0);     // 用板块涨跌幅说明最大亏损
    if (!tp.riskFlags.isEmpty()) {        // 有风险标记则追加展示
        tp.riskNote += QStringLiteral("；风险：") + tp.riskFlags.join(QStringLiteral(" / "));
    }
    if (tp.riskPct > 0.0) {               // 有有效风险空间才补充盈亏比
        tp.riskNote += QStringLiteral("；本单买入中枢→止损中枢 %1%，盈亏比(保守) %2")
                           .arg(tp.riskPct, 0, 'f', 2)      // 风险空间
                           .arg(tp.riskReward, 0, 'f', 2);  // 保守盈亏比
    }

    return tp;                            // 返回完整交易计划
}

} // namespace Plan                     // 命名空间结束

// ---------------------------------------------------------------------------
// 小资金集中持仓口径：候选池只推荐综合分最高的 kConcentratedPositionCount 只
//
// 规则：
//   · 只有「可执行且有建议仓位」的标的才有资格占用推荐名额；
//   · 名额按**综合评分**从高到低分配（与列表显示顺序解耦，排序变化不影响名额归属）；
//   · 落选的可执行标的降级为备选：actionable=false、positionPct=0，
//     并在 note 里写明原因，让用户知道「不是不能买，而是名额只有 2 个」。
//
// 该函数是「仅推荐两只」的唯一口径来源：决策台（决定中栏详情 / 仓位）与
// 候选池面板（决定「推荐 N」角标）都调用它，杜绝两处各算一套导致的名额漂移。
// ---------------------------------------------------------------------------
void applyConcentratedPicks(QVector<TradePlan>& pool)
{
    // 1) 收集有资格占用名额的标的，按综合评分降序（同分保持原顺序，稳定）
    QVector<int> eligible;
    for (int i = 0; i < pool.size(); ++i) {
        const TradePlan& p = pool.at(i);
        if (p.actionable && p.positionPct > 0.0) eligible.append(i);
    }
    std::stable_sort(eligible.begin(), eligible.end(), [&pool](int a, int b) {
        return pool.at(a).compositeScore > pool.at(b).compositeScore;
    });

    // 2) 名额内：保留推荐资格；名额外：降级为备选
    for (int k = 0; k < eligible.size(); ++k) {
        TradePlan& p = pool[eligible.at(k)];
        if (k < kConcentratedPositionCount) continue;
        p.actionable = false;
        p.positionPct = 0.0;
        if (!p.note.startsWith(QStringLiteral("备选")))
            p.note = QStringLiteral("备选：小资金集中持仓仅推荐综合分前 %1 只")
                         .arg(kConcentratedPositionCount);
    }
}
