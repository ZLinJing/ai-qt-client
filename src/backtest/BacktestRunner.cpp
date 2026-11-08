#include "backtest/BacktestRunner.h" // 回测运行器头文件

#include "data/HttpClient.h"        // HTTP网络请求客户端
#include "data/KlineSource.h"       // K线数据源
#include "data/LimitPoolSource.h"   // 涨跌停池数据源
#include "sentiment/LongSignalEngine.h" // 做多信号生成引擎
#include "sentiment/SentimentEngine.h"  // 市场情绪评估引擎
#include "strategy/FilterPipeline.h"    // 策略筛选管道
#include "strategy/MultiFactorEngine.h" // 与实盘共用的多因子评分/排名引擎

#include <QDateTime>   // 日期时间处理
#include <QDir>        // 目录操作
#include <QEventLoop>  // 事件循环（用于异步转同步）
#include <QFile>       // 文件读写
#include <QSet>        // 集合容器
#include <QTextStream> // 文本流输出
#include <QTimer>      // 定时器（超时控制）
#include <algorithm>   // 标准算法库

namespace {

// 将信号动作枚举转换为中文层级名称
QString tierName(SignalAction a)
{
    switch (a) {
    case SignalAction::Attack: return QStringLiteral("主攻"); // 高置信度进攻信号
    case SignalAction::Build:  return QStringLiteral("建仓"); // 中等置信度建仓
    case SignalAction::Probe:  return QStringLiteral("试仓"); // 低置信度试探
    default:                   return QStringLiteral("观察"); // 仅观察不操作
    }
}

// 结算单笔交易：次日开盘买入，T+2日按规则卖出
BacktestTrade settle(const TradePlan& plan, const QString& tier, const KlineSeries& full,
                     const QDate& planDate, double fillProb)
{
    BacktestTrade t;               // 初始化回测交易记录
    t.planDate = planDate;         // 计划制定日期
    t.code = plan.code;            // 股票代码
    t.name = plan.name;            // 股票名称
    t.tier = tier;                 // 信号层级
    t.buyLow = plan.buyLow;        // 买入区间下限
    t.buyHigh = plan.buyHigh;      // 买入区间上限
    t.stop = plan.stopLossHigh;    // 止损价上限
    t.take = plan.takeProfitLow;   // 止盈价下限
    t.fillProbability = fillProb;  // 预估成交概率
    t.positionPct = plan.positionPct; // 仓位百分比

    // 使用不复权K线避免前复权导致的历史价格失真
    const QVector<Candle>& bars = full.hasRaw() ? full.rawCandles : full.candles;
    int idx = -1;                  // 计划日在K线序列中的索引
    for (int i = 0; i < bars.size(); ++i) {
        if (bars[i].date == planDate) { idx = i; break; } // 定位计划日
    }
    // 检查是否有足够的后续K线完成T+2结算
    if (idx < 0 || idx + 2 >= bars.size()) {
        t.outcome = QStringLiteral("数据不足（未走完 T+2）");
        return t;
    }

    const Candle& d1 = bars[idx + 1]; // T+1日K线（买入日）
    const Candle& d2 = bars[idx + 2]; // T+2日K线（卖出日）

    // 一字板判定：最高价≈最低价且收盘价有效则无法买入
    if (d1.high > 0 && qAbs(d1.high - d1.low) < 1e-6 && d1.close > 0) {
        t.outcome = QStringLiteral("一字板，买不到");
        return t;
    }
    // 高开超限：开盘价超过追高上限则放弃
    if (d1.open > plan.buyHigh * 1.005) {
        t.outcome = QStringLiteral("高开超过追高上限，未成交");
        return t;
    }
    // 低开破位：开盘价低于承接位则放弃
    if (d1.open < plan.buyLow * 0.98) {
        t.outcome = QStringLiteral("低开跌破承接位，放弃");
        return t;
    }
    t.filled = true;               // 标记已成交
    t.entry = qBound(plan.buyLow, d1.open, plan.buyHigh); // 成交价限制在计划区间内

    // T+2日结算逻辑（A股T+1制度，买入当日不可卖出）
    if (d2.open <= plan.stopLossHigh) {
        t.exit = d2.open;          // 跳空低开直接以开盘价止损
        t.outcome = QStringLiteral("跳空止损");
    } else if (d2.low <= plan.stopLossHigh) {
        t.exit = plan.stopLossHigh; // 盘中触及止损价
        t.outcome = QStringLiteral("止损");
    } else if (d2.high >= plan.takeProfitLow) {
        t.exit = plan.takeProfitLow; // 盘中触及止盈价
        t.outcome = QStringLiteral("止盈");
    } else {
        t.exit = d2.close;         // 未触发止盈止损，以收盘价平仓
        t.outcome = QStringLiteral("到期平仓");
    }

    // 计算净盈亏百分比，扣除0.22%双边成本（佣金+印花税+冲击）
    t.pnlPct = (t.exit / t.entry - 1.0) * 100.0 - 0.22;
    return t;
}

} // namespace

// 构造函数：初始化HTTP客户端和数据源
BacktestRunner::BacktestRunner(QObject* parent) : QObject(parent)
{
    m_http = new HttpClient(this); // 创建HTTP客户端
    m_http->setMaxConcurrent(3);   // 低并发避免触发免费接口限流
    m_http->setRateLimitPerHost(4); // 单主机每秒最多4次请求
    m_http->setRetryCount(2);      // 失败自动重试2次
    m_poolSrc = new LimitPoolSource(m_http, this); // 涨跌停池数据源
    m_klineSrc = new KlineSource(m_http, this);    // K线数据源
    // K线就绪时存入缓存
    connect(m_klineSrc, &KlineSource::klineReady, this,
            [this](const QString& code, const KlineSeries& s) { m_klines.insert(code, s); });
    // K线批次完成时报告进度
    connect(m_klineSrc, &KlineSource::allDone, this,
            [this](int ok, int fail) {
                Q_EMIT progress(QStringLiteral("  K 线批次完成：成功 %1 / 失败 %2").arg(ok).arg(fail));
            });
    // HTTP请求失败时报告错误
    connect(m_http, &HttpClient::requestFailed, this,
            [this](const QString& url, const QString& err) {
                Q_EMIT progress(QStringLiteral("  HTTP 失败 %1 → %2").arg(url.left(70), err));
            });
}

// 同步获取指定日期的涨跌停池数据
bool BacktestRunner::fetchPools(const QDate& date, Pools& out)
{
    out = Pools();                 // 重置输出结构
    QEventLoop loop;               // 局部事件循环实现异步转同步
    bool settled = false;          // 完成标志

    // 连接数据就绪信号
    const auto c1 = connect(m_poolSrc, &LimitPoolSource::poolsReady, &loop,
                            [&](const QDate&, const QVector<LimitEntry>& up,
                                const QVector<LimitEntry>& broken,
                                const QVector<LimitEntry>& down) {
        out.up = up;               // 涨停列表
        out.broken = broken;       // 炸板列表
        out.down = down;           // 跌停列表
        out.ok = true;             // 标记成功
        settled = true;
        loop.quit();               // 退出事件循环
    });
    // 连接失败信号
    const auto c2 = connect(m_poolSrc, &LimitPoolSource::failed, &loop,
                            [&](const QString&) { settled = true; loop.quit(); });

    m_poolSrc->fetch(date);        // 发起异步请求
    QTimer::singleShot(20000, &loop, &QEventLoop::quit); // 20秒超时保护
    loop.exec();                   // 阻塞等待结果
    disconnect(c1);                // 断开临时连接
    disconnect(c2);
    Q_UNUSED(settled);
    return out.ok;                 // 返回是否成功获取
}

// 确保指定代码的K线已缓存，缺失则批量拉取
bool BacktestRunner::ensureKlines(const QStringList& codes)
{
    QStringList need;              // 需要拉取的代码列表
    for (const QString& c : codes) {
        if (!m_klines.contains(c)) need << c; // 筛选未缓存的代码
    }
    if (need.isEmpty()) return true; // 全部已缓存则直接返回

    QEventLoop loop;               // 局部事件循环
    bool done = false;             // 完成标志
    // 连接批次完成信号
    const auto c1 = connect(m_klineSrc, &KlineSource::allDone, &loop,
                            [&](int, int) { done = true; loop.quit(); });
    m_klineSrc->fetch(need, 120);  // 批量拉取120根K线
    QTimer::singleShot(40000, &loop, &QEventLoop::quit); // 40秒超时保护
    loop.exec();                   // 阻塞等待完成
    disconnect(c1);                // 断开临时连接
    Q_UNUSED(done);
    return true;                   // 始终返回true，缺失由调用方检测
}

// 模拟单笔交易（封装settle供外部调用）
BacktestTrade BacktestRunner::simulate(const TradePlan& plan, const QString& tier,
                                       const KlineSeries& series, const QDate& planDate,
                                       double fillProb)
{
    return settle(plan, tier, series, planDate, fillProb);
}

// 执行完整回测流程
void BacktestRunner::run(const BacktestConfig& cfg)
{
    BacktestResult result;         // 回测结果汇总
    // 维度贡献度按交易日累计，最后取均值：评估各维度对最终选股结果的影响
    QVector<double> sumContribution(kFactorDimensionCount, 0.0);
    QVector<double> sumShare(kFactorDimensionCount, 0.0);
    int factorDays = 0;
    const QDate end = cfg.endDate.isValid() ? cfg.endDate : QDate::currentDate(); // 回测结束日期
    const int wantDays = qMax(3, cfg.days); // 至少回测3个交易日

    // ===== 阶段1：收集可用交易日 =====
    QHash<QDate, Pools> cache;     // 日期→涨跌停池缓存
    QVector<QDate> days;           // 有效交易日列表
    const QDate floor = end.addDays(-(wantDays + 2) * 3); // 搜索下界（预留周末余量）
    for (QDate d = end; d >= floor && days.size() < wantDays + 2; d = d.addDays(-1)) {
        if (d.dayOfWeek() > Qt::Friday) continue; // 跳过周末
        Pools p;
        if (!fetchPools(d, p) || p.up.isEmpty()) continue; // 跳过无数据或无涨停日
        cache.insert(d, p);        // 缓存该日数据
        days.prepend(d);           // 正序插入
        Q_EMIT progress(QStringLiteral("载入 %1：涨停 %2 / 炸板 %3")
                            .arg(d.toString(QStringLiteral("MM-dd")))
                            .arg(p.up.size()).arg(p.broken.size()));
    }
    result.days = days.size();     // 记录实际回测天数
    if (days.size() < 2) {         // 交易日不足则终止
        result.notes << QStringLiteral("可用交易日不足（%1 天），无法回测").arg(days.size());
        Q_EMIT finished(result);
        return;
    }

    FilterPipeline pipeline;       // 策略筛选管道实例
    QVector<QPair<QDate, double>> history; // 情绪历史序列

    // ===== 阶段2：逐日重放 =====
    for (int i = 0; i < days.size(); ++i) {
        const QDate D = days[i];   // 当前回测日
        const Pools cur = cache.value(D); // 当日涨跌停池
        const Pools prev = (i > 0) ? cache.value(days[i - 1]) : Pools(); // 前一日数据

        // ---- 2.1 构建情绪事实 ----
        SentimentFact f;           // 情绪因子结构体
        f.date = D;                // 日期
        f.limitUpCount = cur.up.size(); // 涨停数
        f.brokenCount = cur.broken.size(); // 炸板数
        f.limitDownCount = cur.down.size(); // 跌停数
        // 炸板率 = 炸板数 / (涨停数 + 炸板数)
        f.brokenRate = (f.limitUpCount + f.brokenCount) > 0
                           ? double(f.brokenCount) / (f.limitUpCount + f.brokenCount) : 0.0;

        double turnoverSum = 0.0, sealSum = 0.0, breakSum = 0.0; // 换手/封单/开板累计
        int early = 0, late = 0, firstBoard = 0, multi = 0, high = 0; // 时段/板型计数
        QSet<QString> curCodes;    // 当日涨停代码集合（用于晋级率计算）
        for (const LimitEntry& e : cur.up) {
            curCodes.insert(e.code); // 记录涨停代码
            f.maxBoardHeight = qMax(f.maxBoardHeight, e.boardCount); // 更新最高连板
            if (e.boardCount <= 1) ++firstBoard; else ++multi; // 首板/连板分类
            if (e.boardCount >= 3) ++high; // 高位板计数
            turnoverSum += e.turnover;     // 累加换手率
            sealSum += e.sealToAmountRatio(); // 累加封单比
            breakSum += e.breakCount;      // 累加开板次数
            if (e.firstSealTime.isValid()) { // 统计封板时段分布
                const int m = e.firstSealTime.hour() * 60 + e.firstSealTime.minute();
                if (m <= 630) ++early;     // 10:30前封板
                if (m >= 840) ++late;      // 14:00后封板
            }
        }
        const int n = qMax(1, f.limitUpCount); // 除数保护
        f.firstBoardCount = firstBoard;    // 首板数量
        f.multiBoardCount = multi;         // 连板数量
        f.highBoardCount = high;           // 高位板数量
        f.avgTurnover = turnoverSum / n;   // 平均换手率
        f.avgSealToAmount = sealSum / n;   // 平均封单比
        f.avgBreakCount = breakSum / n;    // 平均开板次数
        f.sealBefore1030 = early;          // 早盘封板数
        f.sealAfter1400 = late;            // 尾盘封板数

        // 计算晋级率：昨日涨停今日仍涨停的比例
        if (i > 0 && !prev.up.isEmpty()) {
            int prevMulti = 0, prevFirst = 0, advMulti = 0, advFirst = 0;
            for (const LimitEntry& e : prev.up) {
                const bool hit = curCodes.contains(e.code); // 今日是否仍涨停
                if (e.boardCount >= 2) { ++prevMulti; if (hit) ++advMulti; } // 连板晋级
                else { ++prevFirst; if (hit) ++advFirst; } // 首板晋级
            }
            f.prevLimitUpCount = prev.up.size(); // 昨日涨停总数
            f.advanceRate = prevMulti > 0 ? double(advMulti) / prevMulti : 0.0; // 连板晋级率
            f.firstBoardAdvanceRate = prevFirst > 0 ? double(advFirst) / prevFirst : 0.0; // 首板晋级率
        }
        f.premiumKnown = false;    // 历史回测无溢价快照
        f.breadthKnown = false;    // 历史回测无市场宽度
        f.themeKnown = false;      // 历史回测无题材数据
        f.quotesAligned = false;   // 历史回测无行情对齐

        // 评估当日情绪状态
        const SentimentState state = SentimentEngine::evaluate(f, history);
        history.append(qMakePair(D, state.composite)); // 追加到历史序列

        // ---- 2.2 首轮筛选（无K线）确定需拉取的标的 ----
        FilterContext ctx;         // 筛选上下文
        ctx.regime = state.regime; // 市场体制
        ctx.sentiment = &state;    // 情绪状态
        ctx.sectorCount = FilterPipeline::buildSectorCounts(cur.up); // 板块计数

        const QVector<Candidate> pre = pipeline.run(cur.up, ctx); // 无K线初筛

        // 合并通过初筛和闸门前的候选代码，确保观察名单也能获取K线
        QStringList codes;
        for (const Candidate& c : pre) codes << c.entry.code;
        for (const Candidate& c : pipeline.lastPassedPreGate()) {
            if (!codes.contains(c.entry.code)) codes << c.entry.code;
        }
        ensureKlines(codes);       // 批量拉取所需K线
        Q_EMIT progress(QStringLiteral("  需要 K 线 %1 只 · 缓存 %2 只")
                            .arg(codes.size()).arg(m_klines.size()));

        // ---- 2.3 截断K线至计划日后重新筛选（防未来函数）----
        QHash<QString, KlineSeries> sliced; // 截断后的K线映射
        for (const QString& code : codes) {
            const auto it = m_klines.constFind(code);
            if (it != m_klines.constEnd()) sliced.insert(code, it.value().sliceTo(D)); // 仅保留D日及之前
        }
        ctx.klines = sliced;       // 注入截断K线
        const QVector<Candidate> cands = pipeline.run(cur.up, ctx); // 带K线精筛
        const QVector<Candidate>& preGate = pipeline.lastPassedPreGate(); // 闸门前名单
        // 闸门拦截时使用闸门前名单，否则使用精筛结果
        const QVector<Candidate>& pool = preGate.isEmpty() ? cands : preGate;
        if (!pool.isEmpty() && sliced.size() < pool.size()) ++result.klineMissingDays; // 记录K线缺失日

        // ---- 2.3b 多因子评分：与实盘 RecommendEngine 调用同一个 rank() ----
        // 历史快照未落库，snapshot 传空；引擎按可用子项重新归一权重并显式标注
        static const QHash<QString, Snapshot> emptySnapshots;
        const QVector<FactorInput> factorInputs =
            MultiFactorEngine::makeInputs(pool, emptySnapshots, sliced, D);
        const FactorReport factorReport =
            MultiFactorEngine::rank(factorInputs, cfg.factorConfig);
        QHash<QString, FactorEvaluation> factorByCode;
        for (const FactorEvaluation& ev : factorReport.ranking)
            factorByCode.insert(ev.code, ev);      // 被过滤 / 未入 TopN 的标的不参与回测
        result.factorDigest = factorReport.configDigest;
        for (int d = 0; d < kFactorDimensionCount; ++d) {
            sumContribution[d] += factorReport.avgContribution[d];
            sumShare[d] += factorReport.avgShare[d];
        }
        ++factorDays;

        // ---- 2.4 生成信号并结算 ----
        const QVector<LongSignal> sigs =
            LongSignalEngine::generate(pool, sliced, state); // 生成做多信号
        QHash<QString, LongSignal> sigByCode; // 代码→信号映射
        for (const LongSignal& s : sigs) sigByCode.insert(s.code, s);

        int daySignals = 0;        // 当日有效信号计数
        for (const Candidate& c : pool) {
            const auto fit = factorByCode.constFind(c.entry.code);
            if (fit == factorByCode.constEnd()) continue; // 多因子过滤 / 未入 TopN：与实盘一致
            const auto it = sigByCode.constFind(c.entry.code);
            if (it == sigByCode.constEnd()) continue; // 无信号则跳过
            const LongSignal& sig = it.value();
            // 观察/无信号时统计拦截原因
            if (sig.action == SignalAction::Watch || sig.action == SignalAction::None) {
                QString reason = QStringLiteral("其他"); // 默认拦截原因
                for (const QString& b : sig.blockers) {
                    if (!b.contains(QStringLiteral("未过·"))) continue;
                    const int s = b.indexOf(QStringLiteral("未过·")) + 3; // 提取门名称起始位置
                    const int e = b.indexOf(QChar(0xFF1A), s); // 查找全角冒号作为结束
                    reason = b.mid(s, e > s ? e - s : -1); // 截取拦截门名称
                    break;
                }
                result.blockerStats[reason] += 1; // 累计拦截统计
                continue;
            }

            const auto kIt = m_klines.constFind(c.entry.code);
            if (kIt == m_klines.constEnd()) continue; // K线缺失则跳过

            const KlineSeries& slice = sliced.value(c.entry.code); // 获取截断K线
            const TradePlan plan = Plan::build(c, &slice, state.regime, true); // 构建交易计划

            // 模拟成交并记录交易
            BacktestTrade t = simulate(plan, tierName(sig.action), kIt.value(), D,
                                       sig.fillProbability);
            t.board = c.entry.boardCount; // 记录连板数
            t.compositeScore = fit->composite;   // 与实盘同一引擎产出的综合分
            t.factorRank = fit->rank;
            t.dimensionShares.fill(0.0, kFactorDimensionCount);
            for (const FactorContribution& fc : fit->contributions) {
                if (fc.available && fc.dimension < kFactorDimensionCount)
                    t.dimensionShares[fc.dimension] = fc.share;
            }
            result.trades.append(t);     // 追加到交易列表
            ++daySignals;                // 有效信号+1
        }
        result.signalCount += daySignals; // 累计总信号数

        // 报告当日回测进度
        Q_EMIT progress(QStringLiteral("%1 情绪 %2（%3）· 候选 %4 · K线 %5 · 信号 %6 · 成交 %7")
                            .arg(D.toString(QStringLiteral("MM-dd")))
                            .arg(state.composite, 0, 'f', 1)
                            .arg(sentimentTierName(state.tier))
                            .arg(cands.size())
                            .arg(sliced.size())
                            .arg(sigs.size())
                            .arg(daySignals));
    }

    // ===== 阶段3：统计汇总 =====
    double cum = 0.0, peak = 0.0;  // 累计收益与峰值
    for (const BacktestTrade& t : result.trades) {
        if (!t.filled) continue;   // 跳过未成交记录
        ++result.filled;           // 成交笔数+1
        if (t.pnlPct > 0) ++result.wins; // 盈利笔数+1
        cum += t.pnlPct;           // 累加收益
        peak = qMax(peak, cum);    // 更新收益峰值
        result.maxDrawdown = qMax(result.maxDrawdown, peak - cum); // 更新最大回撤
        result.byTier[t.tier].append(t.pnlPct); // 按层级分组收益
    }
    result.totalPnlPts = cum;      // 总收益点数
    result.winRate = result.filled > 0 ? double(result.wins) / result.filled * 100.0 : 0.0; // 胜率
    result.avgPnl = result.filled > 0 ? cum / result.filled : 0.0; // 平均盈亏
    result.expectancy = result.signalCount > 0 ? cum / result.signalCount : 0.0; // 信号期望值

    // ---- 维度贡献度聚合：评估各维度对最终选股结果的影响 ----
    const int fDays = qMax(1, factorDays);
    result.avgDimensionContribution.fill(0.0, kFactorDimensionCount);
    result.avgDimensionShare.fill(0.0, kFactorDimensionCount);
    for (int d = 0; d < kFactorDimensionCount; ++d) {
        result.avgDimensionContribution[d] = sumContribution[d] / fDays;
        result.avgDimensionShare[d] = sumShare[d] / fDays;
    }
    result.factorNotes = QStringLiteral(
        "多因子配置 %1；历史快照未落库，基本面仅用市值/换手等可得子项，"
        "资金流向为代理口径；引擎按可用子项自动重分配权重（与实盘同一套配置与评分函数）")
                              .arg(result.factorDigest);

    // K线缺失警告
    if (result.klineMissingDays > 0) {
        result.notes << QStringLiteral("⚠ 有 %1 个交易日 K 线缺失（该日信号被跳过），样本不完整；"
                                       "若缺失占比高，结论不可用")
                            .arg(result.klineMissingDays);
    }
    // 添加口径说明备注
    result.notes << QStringLiteral("情绪口径：无历史快照/题材 → S1 溢价、S4 宽度、S6 主线缺失，"
                                   "复合指数按可用维度归一（等同线上降级态）");
    result.notes << QStringLiteral("板块效应：历史题材不可得（未来函数），改用行业口径兜底");
    result.notes << QStringLiteral("成交口径：次日开盘价买入（超追高上限/低开破位/一字板则放弃），"
                                   "T+2 按日线结算止盈止损，双边成本 0.22%");

    // ===== 阶段4：导出CSV =====
    if (!cfg.csvPath.isEmpty()) {
        QFile file(cfg.csvPath);   // 打开输出文件
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&file); // 创建文本流
            ts.setEncoding(QStringConverter::Utf8); // UTF-8编码
            ts << "\xEF\xBB\xBF";  // 写入UTF-8 BOM头
            ts << "plan_date,code,name,board,tier,buy_low,buy_high,stop,take,fill_prob,"
                  "position,filled,entry,exit,pnl_pct,outcome,composite,factor_rank,"
                  "share_daban,share_trend,share_momentum,share_capital,"
                  "share_fundamental,share_volatility\n"; // CSV表头（含维度贡献占比）
            for (const BacktestTrade& t : result.trades) {
                // 逐行写入交易记录
                ts << t.planDate.toString(Qt::ISODate) << ',' << t.code << ','
                   << t.name << ',' << t.board << ',' << t.tier << ','
                   << QString::number(t.buyLow, 'f', 2) << ','
                   << QString::number(t.buyHigh, 'f', 2) << ','
                   << QString::number(t.stop, 'f', 2) << ','
                   << QString::number(t.take, 'f', 2) << ','
                   << QString::number(t.fillProbability, 'f', 2) << ','
                   << QString::number(t.positionPct, 'f', 1) << ','
                   << (t.filled ? 1 : 0) << ','
                   << QString::number(t.entry, 'f', 2) << ','
                   << QString::number(t.exit, 'f', 2) << ','
                   << QString::number(t.pnlPct, 'f', 2) << ','
                   << t.outcome << ','
                   << QString::number(t.compositeScore, 'f', 2) << ','
                   << t.factorRank;
            for (int d = 0; d < kFactorDimensionCount; ++d) {
                const double share = d < t.dimensionShares.size() ? t.dimensionShares.at(d) : 0.0;
                ts << ',' << QString::number(share, 'f', 2);
            }
            ts << '\n';
            }
        }
    }

    Q_EMIT finished(result);       // 发射回测完成信号
}