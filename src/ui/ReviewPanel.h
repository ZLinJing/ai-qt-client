#pragma once

#include "review/DecisionReview.h"           // 结构化复盘报告（含条件证据 / 偏差 / 风险 / 板块）
#include <QDate>
#include <QHash>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QTextBrowser;
class Database;
class JournalStore;
class ReviewAssistant;

// ============================================================================
// ReviewPanel：收盘后回答「我当时到底是怎么想的、这个想法站不站得住脚」
//
// 交互设计（2026-11-08 易用性改造）：
//   · 一条控制行完成全部高频操作：选交易日、关键词筛选当前页签、Ctrl+R 重算；
//   · 摘要徽章行给出「信号 / 跳过 / 风险触发 / 偏差窗口 / 板块 / 缺口」一屏概览，
//     缺口徽章可点击直达判定条件页核对明细；
//   · 非交易日自动定位最近有留档的交易日，新用户不再面对空表发呆；
//   · 首次使用有可关闭的三步引导（记忆「不再提示」）；
//   · 每个页签的空态都是「结论 + 原因 + 下一步动作」三段式；
//   · 异常有恢复路径：库未就绪整页给出建议；板块数据失败可在页签内一键重试。
//
// 五个决策复盘视图（默认第一页），每个数值都附判定式、阈值、数据窗口与来源：
//   ① 信号溯源      —— 当时买入 / 离场条件的核心条件逐条核对
//   ② 预期 vs 实际  —— 信号后走势与计划预期的偏差（MFE / MAE / 目标偏差 / 是否触及）
//   ③ 跳过的机会    —— 未执行信号的事后表现（只统计，不评价当时决策）
//   ④ 持仓隐性风险  —— RSI 极端超买 / 低点下移假企稳 / 上方多层阻力
//   ⑤ 板块轮动数据  —— 当日领涨领跌板块的涨幅与主力资金流（不猜题材持续性）
//
// 原有「市场复盘 / 早盘简报 / 预判准确率 / 经验教训」保留为辅助页签，
// 不参与决策复盘的任何结论，也不在这里给出新的买卖或仓位指令。
// ============================================================================

class ReviewPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ReviewPanel(QWidget* parent = nullptr);

    void setAssistant(ReviewAssistant* assistant);
    // 历史回溯所需的数据源：本地库（计划 / K 线 / 复盘留档）、交易日志、内存日 K
    void setContext(Database* db, JournalStore* journal,
                    const QHash<QString, KlineSeries>* klines);
    void refresh();

    // 上下文行动条：挂到顶栏右上角，由主窗口在切页时挂载 / 摘下
    QWidget* pageActions() const { return m_pageActions; }

    // 供 UI 冒烟 / 回归核对
    int tabCount() const;
    QString currentTabText() const;
    const DecisionReview::Report& report() const { return m_report; }

private:
    QWidget* buildPageActions();
    QWidget* buildGuideCard();
    QWidget* buildDbEmpty();
    QSplitter* buildRecordPage(const QStringList& headers, QTableWidget** outTable,
                               QTextBrowser** outDetail);
    void fillDates();
    void rebuildDecisionReview(bool persist);
    void renderKpis();
    void applyFilter();
    QTableWidget* activeTable() const;
    void setTabCount(int index, const QString& suffix);
    void renderSignals();
    void renderOutcomes();
    void renderOutcomeDetail(int row);
    void renderSkipped();
    void renderSkippedDetail(int row);
    void renderRisks();
    void renderRiskDetail(int row);
    void renderSectors();
    void renderLegacy();
    void renderCriteria(QTextBrowser* view);
    QString evidenceHtml(const QVector<DecisionReview::Evidence>& list) const;
    static void showEmptyDetail(QTextBrowser* view, const QString& title,
                                const QString& reason, const QString& action);

    ReviewAssistant* m_assistant = nullptr;
    Database* m_db = nullptr;
    JournalStore* m_journal = nullptr;
    const QHash<QString, KlineSeries>* m_klines = nullptr;

    QWidget* m_pageActions = nullptr;
    QComboBox* m_dateBox = nullptr;
    QLineEdit* m_filterBox = nullptr;
    QLabel* m_status = nullptr;
    QWidget* m_kpiRow = nullptr;
    QHBoxLayout* m_kpiLay = nullptr;
    QFrame* m_guideCard = nullptr;
    QWidget* m_dbEmpty = nullptr;
    QTabWidget* m_tabs = nullptr;
    QStringList m_tabTitles;

    QTableWidget* m_signalTable = nullptr;
    QTextBrowser* m_criteriaView = nullptr;
    QTableWidget* m_outcomeTable = nullptr;
    QTextBrowser* m_outcomeDetail = nullptr;
    QTableWidget* m_skippedTable = nullptr;
    QTextBrowser* m_skippedDetail = nullptr;
    QTableWidget* m_riskTable = nullptr;
    QTextBrowser* m_riskDetail = nullptr;
    QTableWidget* m_sectorTable = nullptr;
    QLabel* m_sectorStatus = nullptr;
    QPushButton* m_btnSectorRetry = nullptr;
    QTextBrowser* m_sectorDetail = nullptr;

    // 辅助页签（原有市场复盘闭环）
    QTextBrowser* m_reviewView = nullptr;
    QTextBrowser* m_briefView = nullptr;
    QTableWidget* m_evalTable = nullptr;
    QTableWidget* m_ruleTable = nullptr;
    QLabel* m_strategyLabel = nullptr;
    QListWidget* m_lessonList = nullptr;

    QPushButton* m_btnReview = nullptr;
    QPushButton* m_btnBrief = nullptr;

    DecisionReview::Report m_report;
    QDate m_date;
    QString m_journalError;      // 交易日志后端不可达的原因（空表示正常）
    bool m_updatingDates = false;
};
