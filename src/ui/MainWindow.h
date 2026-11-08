#pragma once

#include "core/Types.h"
#include "core/TradePlan.h"
#include "data/DataHub.h"
#include "storage/Database.h"
#include "strategy/MarketRegime.h"
#include "strategy/FilterPipeline.h"
#include "strategy/RecommendEngine.h"
#include "strategy/FactorConfig.h"
#include "ui/Theme.h"
#include "sentiment/SentimentEngine.h"
#include <QMainWindow>
#include <QSet>
#include <QDate>
#include <QStringList>

class AiStockPanel;
class JournalPanel;
class ReviewPanel;
class ReviewAssistant;
class PushService;
class JournalStore;
class MySqlStore;
struct ReviewInput;
class QLabel;
class QResizeEvent;
class QStatusBar;
class QPushButton;
class QComboBox;
class QStackedWidget;
class QTimer;
class QDialog;
class ChartPanel;

namespace Comp { class FeedbackCenter; }

// ============================================================================
// MainWindow：做多量化选股系统主窗口
//  布局：顶部品牌条（产品标识 + 实际仓位读数）→ 顶部一级导航 +
//        上下文行动条 → 全局结论条 → 页面内容 → 底部状态栏
//  策略口径：版本管理移除后全局只保留一套「均衡做多」因子权重，
//  实盘评分、缓存与推送使用同一份配置（app_setting.factor.config）。
//  3 个导航页：AI选股结果（三页合一：今日计划+决策台+图表分析） / 交易日志 / 复盘助手
//
//  展示口径：界面只有一种「简洁」视图，只保留当日执行所需信息；
//  研究型入口（因子权重 / 判定依据 / 得分由来 / 归因抽屉 /
//  邮件推送 / AI 分析）已整体移除。情绪引擎的「复合情绪指数 / 有效总仓
//  上限 / 闸门状态」属于研究员口径，同样不在主界面展示；「今天能不能出手」
//  由结论卡与结论条的文字结论、可执行 / 观察数量与最晚决策时点表达。
// ============================================================================

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    static constexpr int kNavBarHeight = 44;
    static constexpr int kCompactWidth = 1120;
    static constexpr int kNarrowWidth = 900;
    void applyCompactMode(bool compact, bool narrow);

private slots:
    void onDataStatus(const QString& text);
    void onDataError(const QString& text);
    void onPoolsUpdated(const QDate& date, const QVector<LimitEntry>& up,
                        const QVector<LimitEntry>& broken, const QVector<LimitEntry>& down);
    void onSnapshotUpdated(const QVector<Snapshot>& list);
    void onKlineUpdated(const QString& code, const KlineSeries& series);
    void onPrevLimitPoolUpdated(const QDate& date, const QVector<LimitEntry>& limitUp);
    void onScreenRequested(const QDate& date);
    void onRefreshRequested();
    void onNavChanged(int index);
    void onAutoRefresh();
    void openKline(const QString& code, const QString& name);  // 卡片「看K线」

private:
    void buildUi();
    QWidget* buildBrandBar();                    // 顶部品牌条
    void buildNavBar();                          // 顶部一级导航 + 上下文行动条
    void setPageActions(QWidget* actions);       // 切换当前页的主动作区
    void updateGlobalVerdict(const RecommendBundle& bundle);  // 跨页常驻的一句话结论
    QWidget* buildPage(QWidget* content, const QString& title, const QString& desc);
    void renderChartKline(const QString& code, const KlineSeries& series);
    void persistFactorConfig();

    // 决策台：状态基调与实际仓位
    void updateUsedPct();
    void runScreen();
    void refreshRegime();
    MarketRegime computeRegime() const;
    void raiseAlert(const QString& text);
    void checkStopLossAlerts();
    void updateStatusBar();

    // 荐股计划：装配 → 展示 → 落库
    void updateRecommend();
    void loadCachedRecommend();
    void fillCurrentPrices(RecommendBundle& bundle) const;  // 旧缓存缺现价时用实时快照补齐
    void restoreCachedKlines(const RecommendBundle& bundle); // 从 SQLite 恢复日 K，首屏即可画缩略图
    void buildReviewInput(ReviewInput& out, const RecommendBundle& bundle) const;

    // 消息推送（自动触发：新结果落定后推送当日结论；手动入口已随研究型入口移除）
    void pushCurrentResult(bool manual);

    void pushQuotesToJournal();
    void publishReviewInput(const RecommendBundle& bundle);

    // ⑤ 到价 / 到时提醒：盘中每分钟检查一次，只提醒不代下单
    void checkReminders();
    void fireReminder(const QString& id, const QString& text);
    void requestPoolWithFallback(const QDate& date, int depth = 0);
    void setupJournalStore();                 // 交易日志存储后端：MySQL 优先，SQLite 兜底

    // 模块
    DataHub* m_data = nullptr;
    Database* m_db = nullptr;
    RegimeEngine* m_regime = nullptr;
    FilterPipeline* m_pipeline = nullptr;
    RecommendEngine* m_recEngine = nullptr;

    // UI
    QWidget* m_brandBar = nullptr;
    QLabel* m_brandSub = nullptr;
    QLabel* m_brandStatus = nullptr;
    QLabel* m_usedLabel = nullptr;               // 当日计划仓位合计与持仓只数

    QPushButton* m_globalVerdict = nullptr;      // 跨页常驻的今日结论条（点击直达今日计划）
    QWidget* m_globalVerdictBar = nullptr;       // 结论条容器（含数据状态角标）
    QLabel* m_globalFreshness = nullptr;         // 结论条右侧的数据状态角标
    QWidget* m_navBar = nullptr;                 // 顶部一级导航条
    QWidget* m_pageActionHost = nullptr;         // 上下文行动条宿主（当前页主动作）
    QWidget* m_currentPageActions = nullptr;     // 当前挂载的行动区（切换时隐藏旧的）
    Comp::FeedbackCenter* m_feedback = nullptr;  // 全局提示中心（info/progress/warn/error）
    bool m_compact = false;
    QStackedWidget* m_stack = nullptr;
    AiStockPanel* m_recPanel = nullptr;          // AI选股结果（今日计划）
    QDialog* m_klineDialog = nullptr;            // 卡片「看K线」弹窗（复用，不反复创建）
    ChartPanel* m_klinePanel = nullptr;
    QLabel* m_klineStatus = nullptr;
    QString m_klineCode;
    QString m_klineName;
    JournalPanel* m_journalPanel = nullptr;
    ReviewPanel* m_reviewPanel = nullptr;
    ReviewAssistant* m_assistant = nullptr;

    QLabel* m_statusLabel = nullptr;
    QList<QPushButton*> m_navButtons;

    PushService* m_push = nullptr;
    bool m_pushManual = false;

    // 交易日志存储：MySQL（服务器集中存储）优先，连接不可用时回退本地 SQLite
    JournalStore* m_journalStore = nullptr;
    MySqlStore*   m_mySqlStore = nullptr;

    // 状态
    FactorConfig m_factorCfg;                    // 全局唯一因子权重（「均衡做多」为出厂默认）
    mutable SentimentState m_sentimentState;
    QVector<Candidate> m_candidates;
    QVector<LimitEntry> m_limitUp;
    QVector<LimitEntry> m_broken;
    QVector<LimitEntry> m_limitDown;
    QVector<LimitEntry> m_prevLimitUp;
    QHash<QString, KlineSeries> m_klines;
    QDate m_currentDate;
    bool m_screenDirty = true;
    QString m_quoteStamp;

    int m_fallbackDepth = 0;
    int m_prevPoolDepth = 0;
    QDate m_prevRequestDate;

    QTimer* m_autoTimer = nullptr;
    int m_autoTick = 0;
    QSet<QString> m_lastZtCodes;
    QSet<QString> m_stopAlerted;
    QSet<QString> m_priceAlerted;    // ⑤ 价格类提醒按「代码:类型」去重
    QStringList m_firedReminders;    // 当日已触发的提醒标识
    QDate m_reminderDate;            // 跨交易日自动清空去重集合
};
