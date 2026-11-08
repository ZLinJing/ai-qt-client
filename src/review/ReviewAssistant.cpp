#include "review/ReviewAssistant.h"      // 本模块声明

#include "data/SectorFlowSource.h"       // 板块资金流客观数据
#include "notify/PushService.h"          // 发送简报邮件
#include "storage/Database.h"            // 复盘/预判/规则/决策复盘持久化

#include <QDateTime>                     // 日期时间
#include <QTime>                         // 时段判断
#include <algorithm>                     // 通用算法

// 构造：恢复规则、偏差与上次复盘/简报状态
ReviewAssistant::ReviewAssistant(Database* db, PushService* push, QObject* parent)
    : QObject(parent), m_db(db), m_push(push)  // 记录依赖
{
    refreshRules();                                // 加载规则表
    if (m_db && m_db->isOpen()) {                  // 库可用才恢复状态
        m_bias = m_db->getSetting(QStringLiteral("assistant.bias"), QStringLiteral("0")).toDouble();  // 偏差校正项
        const QString lr = m_db->getSetting(QStringLiteral("assistant.lastReview"));  // 上次复盘日期
        if (!lr.isEmpty()) {
            const QDate d = QDate::fromString(lr, Qt::ISODate);  // 解析日期
            if (d.isValid()) m_lastReview = m_db->loadReview(d); // 载入该日复盘
        }
        m_lastBriefDate = QDate::fromString(
            m_db->getSetting(QStringLiteral("assistant.lastBrief")), Qt::ISODate);   // 上次简报日
        m_lastForecast = m_db->loadForecast(QDate::currentDate());                   // 今日预判

        if (!m_lastReview.valid) {                 // 内存里没有复盘时
            const QDate latest = m_db->latestReviewDate();  // 取库里最新一条（无头模式生成的）
            if (latest.isValid()) m_lastReview = m_db->loadReview(latest);  // 以此为准
        }
    }

    // 板块轮动：只取客观数值（涨幅 / 主力净流入 / 净占比），不推断题材持续性；
    // 成功与失败都落库，界面按「可用 / 数据缺失及原因」展示。
    m_sectorFlow = new SectorFlowSource(this);
    connect(m_sectorFlow, &SectorFlowSource::ready,
            this, &ReviewAssistant::onSectorFlowsReady);
    connect(m_sectorFlow, &SectorFlowSource::failed,
            this, &ReviewAssistant::onSectorFlowsFailed);
}

// 加载规则：库里为空则用默认规则并落库
void ReviewAssistant::refreshRules()
{
    m_rules = m_db && m_db->isOpen() ? m_db->loadRules() : QVector<AssistantRule>();  // 读库
    if (m_rules.isEmpty()) {                       // 无记录
        m_rules = ReviewEngine::defaultRules();    // 用内置规则
        if (m_db && m_db->isOpen()) m_db->saveRules(m_rules);  // 落库
    }
}

// 注入分析结果
void ReviewAssistant::setInput(const ReviewInput& in)
{
    m_input = in;                                  // 保存输入
    m_hasInput = in.valid();                       // 记录是否有效
}

// 是否已有该日输入
bool ReviewAssistant::hasInputFor(const QDate& date) const
{
    return m_hasInput && m_input.date == date;     // 有效且日期匹配
}

// 运行态持久化
void ReviewAssistant::persistState()
{
    if (!m_db || !m_db->isOpen()) return;          // 库不可用则跳过
    m_db->setSetting(QStringLiteral("assistant.bias"), QString::number(m_bias, 'f', 6));  // 偏差项
    if (m_lastReview.date.isValid())               // 有复盘才写
        m_db->setSetting(QStringLiteral("assistant.lastReview"),
                         m_lastReview.date.toString(Qt::ISODate));
    if (m_lastBriefDate.isValid())                 // 有简报才写
        m_db->setSetting(QStringLiteral("assistant.lastBrief"),
                         m_lastBriefDate.toString(Qt::ISODate));
}

// 定时驱动：按时间窗与去重标记决定触发哪个流程
void ReviewAssistant::tick()
{
    const QDate today = QDate::currentDate();      // 今天
    if (today.dayOfWeek() > Qt::Friday) return;      // 周末不跑（节假日由「无数据」自然拒绝）
    const QTime now = QTime::currentTime();        // 当前时间

    if (now >= QTime(15, 0) && m_lastReview.date != today   // 15:00 后且今日未复盘
        && m_hasInput && m_input.date == today) {           // 且当日分析结果已就绪
        runCloseReview(false);                     // 生成收盘复盘
    }

    if (now >= QTime(9, 25) && now < QTime(15, 0) && m_lastBriefDate != today) {  // 9:25~15:00 且今日未发
        runMorningBrief(false);                    // 生成并发送早盘简报
    }

    // 板块轮动客观数据：盘中定期留一份当日快照，收盘复盘时才有数值可核对。
    // 与「预判 / 规则权重」完全解耦 —— 这份数据只用于陈述当日事实。
    if (now >= QTime(9, 30) && now <= QTime(15, 30)) refreshSectorFlows(false);
}

// 收盘复盘：生成报告 + 复核当日早盘预判 + 更新规则权重
void ReviewAssistant::runCloseReview(bool force)
{
    const QDate today = QDate::currentDate();      // 今天
    if (!m_hasInput || m_input.date != today) {    // 无当日分析结果
        Q_EMIT log(QStringLiteral("复盘跳过：今天的分析结果尚未就绪"));
        return;                                    // 结束
    }
    if (!force && m_lastReview.date == today) return;  // 已生成过则跳过

    ReviewReport report = ReviewEngine::buildReview(m_input);  // 生成复盘

    Forecast fc;                                   // 当日早盘预判
    if (m_db && m_db->isOpen()) fc = m_db->loadForecast(today);  // 从库读取

    QVector<RuleUpdate> updates;                   // 规则更新明细
    if (fc.valid) {                                // 有预判才可复核
        const ForecastEval ev = ReviewEngine::evaluate(fc, m_input.sentiment, m_input);  // 评估
        if (ev.valid) {                            // 评估有效
            updates = ReviewEngine::updateRules(m_rules, fc, ev);       // 调整规则权重
            m_bias = ReviewEngine::updateBias(m_bias, fc, ev);          // 更新偏差校正
            if (m_db && m_db->isOpen()) {          // 落库
                m_db->saveRules(m_rules);          // 保存规则
                m_db->saveForecastEval(ev);        // 保存评估
                for (const QString& d : ev.deviations)                        // 逐条偏差
                    m_db->addLesson(today, QStringLiteral("偏差"), d);        // 记为教训
                for (const QString& l : ev.lessons)                           // 逐条反思
                    m_db->addLesson(today, QStringLiteral("反思"), l);
                m_db->touchStrategy(fc.stance, ev.hit);                       // 统计策略命中
            }
            ReviewEngine::appendEvalSection(report, ev, updates);   // 把评估追加到报告
            Q_EMIT log(QStringLiteral("预判复核 %1 分（%2）：%3")
                           .arg(ev.score)                                          // 得分
                           .arg(ev.hit ? QStringLiteral("命中") : QStringLiteral("偏差"))  // 是否命中
                           .arg(ev.lessons.value(0)));                             // 首要教训
        }
    } else {
        Q_EMIT log(QStringLiteral("今日无早盘预判，复盘只做当日分析"));  // 无预判说明
    }

    if (m_db && m_db->isOpen()) m_db->saveReview(report);  // 保存复盘
    m_lastReview = report;                          // 更新内存状态
    persistState();                                 // 持久化运行态

    Q_EMIT log(QStringLiteral("收盘复盘已生成：%1").arg(report.headline));  // 日志
    Q_EMIT updated();                               // 通知 UI 刷新
}

// 早盘简报：基于昨日复盘出预判并发送
void ReviewAssistant::runMorningBrief(bool force)
{
    const QDate today = QDate::currentDate();      // 今天
    if (!force && m_lastBriefDate == today) return;  // 今日已发则跳过
    if (!m_db || !m_db->isOpen()) {                // 库不可用
        Q_EMIT log(QStringLiteral("早盘简报跳过：数据库未就绪"));
        return;                                    // 结束
    }

    const QDate yDate = m_db->latestReviewDate();  // 最近可用的复盘日
    if (!yDate.isValid() || yDate >= today) {      // 没有更早的复盘
        Q_EMIT log(QStringLiteral("早盘简报跳过：没有可用的昨日复盘"));
        return;                                    // 结束
    }
    const ReviewReport yesterday = m_db->loadReview(yDate);  // 载入昨日复盘
    if (!yesterday.valid) {                        // 读取失败
        Q_EMIT log(QStringLiteral("早盘简报跳过：昨日复盘读取失败"));
        return;                                    // 结束
    }

    Forecast f = ReviewEngine::buildForecast(yesterday, m_rules, m_bias);  // 生成预判
    if (!f.valid) {                                // 预判失败
        Q_EMIT log(QStringLiteral("早盘简报跳过：预判生成失败"));
        return;                                    // 结束
    }
    f.forDate = today;   // 简报永远针对"今天"
    m_db->saveForecast(f);                         // 落库
    m_lastForecast = f;                            // 更新内存状态

    if (m_push) {                                  // 有推送通道
        const QString subject = QStringLiteral("早盘简报 %1 ｜ %2")
                                    .arg(today.toString(QStringLiteral("MM-dd")), f.stance);  // 主题
        m_push->sendCustom(subject, f.html);       // 作为自定义邮件发送（不参与荐股去重）
    } else {
        Q_EMIT log(QStringLiteral("简报已生成，但未接推送通道（未发送）"));  // 无通道提示
    }

    m_lastBriefDate = today;                       // 记录已发日期
    persistState();                                // 持久化
    Q_EMIT log(QStringLiteral("早盘简报已生成：%1（涨停 %2~%3）")
                   .arg(f.stance).arg(f.limitUpLow).arg(f.limitUpHigh));  // 日志
    Q_EMIT updated();                              // 通知 UI 刷新
}

// 最近一次复盘正文
QString ReviewAssistant::latestReviewHtml() const
{
    return m_lastReview.valid ? m_lastReview.html : QString();  // 无效则空串
}

// 最近一次预判正文
QString ReviewAssistant::latestForecastHtml() const
{
    return m_lastForecast.valid ? m_lastForecast.html : QString();  // 无效则空串
}

// 最近 n 次评估（转发到库）
QVector<ForecastEval> ReviewAssistant::evals(int n) const
{
    return m_db && m_db->isOpen() ? m_db->recentEvals(n) : QVector<ForecastEval>();
}

// 最近 n 条教训（转发到库）
QStringList ReviewAssistant::lessons(int n) const
{
    return m_db && m_db->isOpen() ? m_db->recentLessons(n) : QStringList();
}

// 策略命中统计（转发到库）
QVector<QPair<QString, QPair<int, int>>> ReviewAssistant::strategies() const
{
    return m_db && m_db->isOpen()
               ? m_db->strategies()                        // 库可用则读真实统计
               : QVector<QPair<QString, QPair<int, int>>>();  // 否则空
}

// 一行状态摘要
QString ReviewAssistant::statusLine() const
{
    int active = 0;                                // 启用规则数
    for (const AssistantRule& r : m_rules) if (r.active) ++active;  // 计数
    return QStringLiteral("复盘 %1 ｜ 简报 %2 ｜ 规则 %3 条（启用 %4）｜ 幅度校正 %5")
        .arg(m_lastReview.date.isValid() ? m_lastReview.date.toString(QStringLiteral("MM-dd"))
                                         : QStringLiteral("—"))     // 复盘日期或占位
        .arg(m_lastBriefDate.isValid() ? m_lastBriefDate.toString(QStringLiteral("MM-dd"))
                                       : QStringLiteral("—"))       // 简报日期或占位
        .arg(m_rules.size())                       // 规则总数
        .arg(active)                               // 启用数
        .arg(m_bias, 0, 'f', 3);                   // 偏差校正项
}

// ---------------------------------------------------------------- 板块轮动客观数据

QString ReviewAssistant::sectorFlowSourceText()
{
    return SectorFlowSource::sourceText();
}

void ReviewAssistant::refreshSectorFlows(bool force)
{
    if (!m_sectorFlow) return;
    const QDateTime now = QDateTime::currentDateTime();
    // 10 分钟节流：复盘助手 30s tick 一次，不能把公开接口当成轮询靶子
    if (!force && m_lastSectorFetch.isValid()
        && m_lastSectorFetch.secsTo(now) < 600) return;
    m_lastSectorFetch = now;
    m_sectorFlow->fetch();
}

void ReviewAssistant::onSectorFlowsReady(
    const QVector<DecisionReview::SectorFlowRow>& rows, const QDateTime& fetchedAt)
{
    if (m_db && m_db->isOpen()) {
        m_db->saveSectorFlows(QDate::currentDate(), fetchedAt, rows,
                              SectorFlowSource::sourceText());
    }
    Q_EMIT log(QStringLiteral("板块资金流已留档：%1 个板块 · %2")
                   .arg(rows.size()).arg(fetchedAt.toString(QStringLiteral("HH:mm:ss"))));
    Q_EMIT updated();
}

void ReviewAssistant::onSectorFlowsFailed(const QString& reason)
{
    // 失败也必须留痕：界面显示「数据缺失 + 具体原因」，不以题材涨幅反推资金流
    if (m_db && m_db->isOpen()) m_db->saveSectorFlowFailure(QDate::currentDate(), reason);
    Q_EMIT log(QStringLiteral("板块资金流数据缺失：%1").arg(reason));
    Q_EMIT updated();
}

QVector<DecisionReview::SectorFlowRow> ReviewAssistant::sectorFlows(
    const QDate& date, QDateTime* fetchedAt, QString* source, QString* reason) const
{
    if (m_db && m_db->isOpen())
        return m_db->loadSectorFlows(date, fetchedAt, source, reason);
    if (reason) *reason = QStringLiteral("数据库未就绪，无法读取板块资金流留档");
    return QVector<DecisionReview::SectorFlowRow>();
}
