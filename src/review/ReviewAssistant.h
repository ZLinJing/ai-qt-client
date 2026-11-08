#pragma once                                     // 防止头文件被重复包含

#include "review/ReviewEngine.h"                 // 依赖 ReviewInput 与引擎接口
#include "review/ReviewTypes.h"                  // 依赖 ReviewReport / Forecast / AssistantRule
#include <QDate>                                 // 日期
#include <QDateTime>                             // 板块资金流抓取时间
#include <QObject>                               // 基类（信号槽）
#include <QStringList>                           // 教训列表
#include <QVector>                               // 规则与评估列表

class Database;                                  // 前向声明：持久化
class PushService;                               // 前向声明：发简报邮件
class SectorFlowSource;                          // 前向声明：板块资金流数据源

// ReviewAssistant：把「复盘 → 预判 → 校验 → 迭代」串成自动流程
// 15:00 后 runCloseReview 生成当日复盘并复核早盘预判；09:25~15:00 runMorningBrief 出预判并发简报
// 约束：只读写自己的表，不碰交易阈值；所有输出落库可回溯；缺数据宁可不出预判

class ReviewAssistant : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit ReviewAssistant(Database* db, PushService* push, QObject* parent = nullptr);  // 注入依赖并恢复状态

    void setInput(const ReviewInput& in);        // 注入当日分析结果（每次分析完成后由 MainWindow 调用）
    bool hasInputFor(const QDate& date) const;   // 是否已有该日的分析结果

    void tick();                                 // 定时驱动（建议 30s 一次），内部按时间与去重决定是否执行

    void runCloseReview(bool force = false);     // 收盘复盘
    void runMorningBrief(bool force = false);    // 早盘简报

    QString latestReviewHtml() const;            // 最近一次复盘正文
    QString latestForecastHtml() const;          // 最近一次预判正文
    QDate   latestReviewDate() const { return m_lastReview.date; }  // 最近复盘日期
    QDate   lastBriefDate() const { return m_lastBriefDate; }       // 最近简报日期
    double  bias() const { return m_bias; }                          // 当前幅度偏差校正项
    QVector<AssistantRule> rules() const { return m_rules; }         // 规则表副本
    QVector<ForecastEval> evals(int n = 20) const;                   // 最近 n 次评估
    QStringList lessons(int n = 10) const;                           // 最近 n 条教训
    QVector<QPair<QString, QPair<int, int>>> strategies() const;     // 各策略的（次数, 命中）
    QString statusLine() const;                                      // 一行状态摘要

    // ---- 决策复盘：当日事实与板块轮动客观数据 ----
    Database* database() const { return m_db; }                       // 供界面读历史计划 / K 线
    const ReviewInput& input() const { return m_input; }              // 当日实时事实（历史日不从这里取）
    bool hasInput() const { return m_hasInput; }

    void refreshSectorFlows(bool force = false);                     // 拉一次当日板块资金流并落库
    QVector<DecisionReview::SectorFlowRow> sectorFlows(
        const QDate& date, QDateTime* fetchedAt = nullptr,
        QString* source = nullptr, QString* reason = nullptr) const;  // 读当日留档
    static QString sectorFlowSourceText();                           // 字段与单位口径

Q_SIGNALS:
    void log(const QString& line);               // 过程日志
    void updated();                              // 数据已更新（UI 刷新）

private:
    void refreshRules();                         // 从库加载规则（空则用默认值并落库）
    void persistState();                         // 把运行态写回库
    void onSectorFlowsReady(const QVector<DecisionReview::SectorFlowRow>& rows,
                            const QDateTime& fetchedAt);
    void onSectorFlowsFailed(const QString& reason);

    Database* m_db = nullptr;                    // 数据库（可为空）
    PushService* m_push = nullptr;               // 推送通道（可为空）

    ReviewInput m_input;                         // 最近注入的分析结果
    bool        m_hasInput = false;              // 是否有可用的分析结果

    QVector<AssistantRule> m_rules;              // 规则表
    double      m_bias = 0.0;                    // 幅度偏差校正项
    ReviewReport m_lastReview;                   // 最近复盘
    Forecast     m_lastForecast;                 // 最近预判
    QDate        m_lastBriefDate;                // 最近简报日

    SectorFlowSource* m_sectorFlow = nullptr;   // 板块资金流（当日快照，失败明确留档原因）
    QDateTime    m_lastSectorFetch;              // 上次发起时间（用于节流）
};

Q_DECLARE_METATYPE(ReviewInput)                  // 注册元类型（可跨线程信号传递）
