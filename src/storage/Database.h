#pragma once

#include "core/Types.h"
#include "core/TradePlan.h"
#include "review/ReviewTypes.h"
#include "review/DecisionReview.h"
#include "sentiment/SentimentTypes.h"
#include "storage/JournalStore.h"
#include "storage/TradeRecord.h"
#include <QObject>
#include <QSqlDatabase>
#include <QVector>
#include <QDate>
#include <QPair>

// ============================================================================
// Database：本地存储（SQLite）
//  表设计：
//    trade_journal    交易日志（成交流水，买入 + 可选的了结信息）
//    recommend_plan   荐股计划快照（买入/止盈/止损区间，离线兜底用）
//    daily_kline      日 K 缓存（避免重复请求行情接口）
//    sentiment_daily  情绪序列留档（趋势 / 退潮确认）
//    review_*         复盘助手（日报 / 预判 / 评估 / 规则 / 策略 / 教训）
//    app_setting      键值配置（去重指纹等跨会话小状态）
//
//  交易日志同时实现了 storage/JournalStore 接口，因此既能被 ui/JournalPanel
//  直接使用（SQLite 兜底），也能与 MySQL 后端并列切换（见 storage/MySqlStore）。
// ============================================================================

class Database : public QObject, public JournalStore
{
    Q_OBJECT
public:
    explicit Database(QObject* parent = nullptr);
    ~Database() override;

    bool open(const QString& dbPath = QString());
    void close();
    bool isOpen() const;
    QString path() const { return m_path; }
    QString lastError() const { return m_lastError; }

    // ---------- 可靠性：结构版本 / 完整性 / 备份 ----------
    int  schemaVersion() const;                  // 0 = 未标记的历史库
    bool backup(const QString& destPath);        // 一致性备份（VACUUM INTO）
    void autoBackup();                           // 每日一次，保留最近 7 份

    // ---------- 荐股计划（离线兜底：打开即见已算好的结果） ----------
    bool saveRecommend(const RecommendBundle& bundle);
    RecommendBundle loadRecommend(const QDate& date) const;
    QVector<QDate> recommendDates() const;
    QDate latestRecommendDate() const;

    // ---------- 情绪序列（复合情绪指数按日留档，用于趋势 / 退潮确认） ----------
    // 新增表 sentiment_daily，不触碰既有 5 张表与接口
    bool saveSentiment(const SentimentState& state);
    // 返回升序 (日期, 复合指数)；until 生效则只取 <= until 的样本
    QVector<QPair<QDate, double>> sentimentHistory(const QDate& until, int days = 10) const;
    SentimentState loadSentiment(const QDate& date) const;

    // ---------- K线缓存 ----------
    bool saveKline(const KlineSeries& k);
    KlineSeries loadKline(const QString& code) const;

    // ---------- 交易日志（成交流水：记录买入 + 可选的了结信息，无独立平仓操作） ----------
    // 以下 5 个方法实现 storage/JournalStore 接口（SQLite 后端）
    bool addTrade(TradeRecord& rec) override;              // 回填 rec.id
    bool updateTrade(const TradeRecord& rec) override;     // 编辑已有记录（含补录了结信息）
    bool deleteTrade(int id) override;
    QVector<TradeRecord> trades() const override;
    QString describe() const override;                     // 展示用：本地库路径

    // ---------- 键值配置（推送去重指纹等跨会话小状态） ----------
    bool setSetting(const QString& key, const QString& value);
    QString getSetting(const QString& key, const QString& def = QString()) const;

    // ---------- 复盘助手（收盘复盘 / 早盘预判 / 偏差评估 / 规则迭代） ----------
    bool saveReview(const ReviewReport& r);
    ReviewReport loadReview(const QDate& date) const;
    QDate latestReviewDate() const;

    bool saveForecast(const Forecast& f);
    Forecast loadForecast(const QDate& forDate) const;

    bool saveForecastEval(const ForecastEval& e);
    QVector<ForecastEval> recentEvals(int limit = 20) const;

    bool saveRules(const QVector<AssistantRule>& rules);
    QVector<AssistantRule> loadRules() const;

    bool addLesson(const QDate& date, const QString& category, const QString& text);
    QStringList recentLessons(int limit = 10) const;

    // 有效策略库：同一「立场」反复命中即累积，命中率用于展示
    bool touchStrategy(const QString& stance, bool hit);
    QVector<QPair<QString, QPair<int, int>>> strategies() const;

    // ---------- 决策复盘（信号溯源 / 偏差 / 跳过机会 / 隐性风险） ----------
    // 按交易日整包 JSON 留档：复盘结论可回看、可逐条核对数值与判定式
    bool saveDecisionReview(const DecisionReview::Report& r);
    DecisionReview::Report loadDecisionReview(const QDate& date) const;

    // ---------- 板块轮动客观数据（当日快照，含数据缺失原因） ----------
    bool saveSectorFlows(const QDate& date, const QDateTime& fetchedAt,
                         const QVector<DecisionReview::SectorFlowRow>& rows,
                         const QString& source);
    bool saveSectorFlowFailure(const QDate& date, const QString& reason);
    QVector<DecisionReview::SectorFlowRow> loadSectorFlows(
        const QDate& date, QDateTime* fetchedAt = nullptr,
        QString* source = nullptr, QString* reason = nullptr) const;

private:
    bool tryOpen(const QString& path);
    bool ensureSchema();
    bool migrateSchema(int fromVersion);
    bool exec(const QString& sql);

    static constexpr int kCurrentSchemaVersion = 2;  // v2：决策复盘 + 板块资金流快照

    QSqlDatabase m_db;
    QString m_path;
    QString m_lastError;
    bool m_autoBackupEnabled = true;   // 显式指定库路径（自检/冒烟）时关闭自动备份
};
