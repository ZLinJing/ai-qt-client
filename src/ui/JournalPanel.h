#pragma once

#include "analysis/TradeAdvisor.h"
#include "core/TradePlan.h"
#include "storage/JournalStore.h"
#include <QHash>
#include <QWidget>
#include <QVector>

class QTableWidget;
class QLabel;
class QLineEdit;
class QTextBrowser;
class QPushButton;
class QSplitter;
class QButtonGroup;
class TableEmptyHint;

namespace Comp { class KpiTile; }   // 通用指标块（见 ui/Components.h）

// ============================================================================
// JournalPanel：交易日志（成交流水 + 复盘统计 + 下一步动作）
//
// 定位：策略「能不能用」的证据链。记录每笔真实成交（买入价，及可选的卖出
// 信息），据此算胜率 / 每笔期望 / 累计盈亏 / 最大回撤 / 止损命中率。
// 本模块只负责「记录与展示」，不提供平仓操作：卖出信息通过「编辑记录」补录。
//
// ★ 界面结构（自上而下）：
//   复盘统计卡（4 主 KPI + 6 次指标）→ 持仓下一步动作卡 → 工具栏
//   → 明细表 / 可折叠详情栏（QSplitter 可拖动）；
//   颜色一律配文字或底色，不靠颜色单独承载语义。
//
// ★ 存储后端：通过 storage/JournalStore 接口读写，MySQL 优先、SQLite 兜底。
//
// ★ 持仓中标的的盈亏：
//   已了结 = (卖出价 × 数量 − 成本 − 双边费用)
//   持仓中 = (现价 × 数量 − 成本 − 双边费用)，现价取自全市场快照。
//   两者共用 TradeRecord::pnlAt()；现价缺失时明确显示「无行情」，不估算。
//
// ★ AI 诊断（analysis/TradeAdvisor）：本地规则推理，无额外网络请求；
//   结论随行情与 K 线更新，表格「下一步动作」列与 CSV 导出共用同一判定。
// ============================================================================

class JournalPanel : public QWidget
{
    Q_OBJECT
public:
    explicit JournalPanel(QWidget* parent = nullptr);

    void setStore(JournalStore* store);       // 注入交易日志存储后端（MySQL / SQLite）
    void importPlan(const TradePlan& plan);   // 从荐股计划一键记录买入
    void refresh();

    // 注入实时行情：code -> 现价（含昨收，用于「无涨跌幅」时兜底判断）
    void setSnapshots(const QHash<QString, Snapshot>& snapshots,
                      const QString& stamp = QString());

    // 注入日 K（供 AI 诊断计算均线 / ATR / 筹码压力；缺失时自动降级）
    void setKlines(const QHash<QString, KlineSeries>& klines);

    // 注入情绪阶段（回本可行性会据此调整修复周期）
    void setMarketPhase(MarketPhase phase, bool known);

    // 当前日志涉及的代码（主窗口据此补齐 K 线缓存）
    QStringList codes() const;

    // 已注入的日 K（主窗口据此判断是否需要再读本地缓存）
    const QHash<QString, KlineSeries>& klines() const { return m_klines; }

    // 最近一次行情时间（供状态栏 / 统计卡展示）
    QString quoteStamp() const { return m_quoteStamp; }

    // 上下文行动条：挂到顶栏右上角，由主窗口在切页时挂载 / 摘下
    QWidget* pageActions() const { return m_pageActions; }

Q_SIGNALS:
    void statusMessage(const QString& text);
    void quoteRefreshRequested();             // 用户在日志页要求拉一次最新行情

private:
    void buildUi();
    QWidget* buildPageActions();              // 上下文行动条（顶栏右上角）
    void rebuildTable();
    void updateStats();
    void updateActionList();                  // ⑥ 持仓逐只给一个明确动作，替代长篇诊断阅读
    void applyFilter();                       // 按视图 + 关键字过滤
    void updateEmptyHint();                   // 区分「无记录」与「筛选无结果」
    void updateSelectionActions();            // 未选中记录时禁用编辑 / 删除
    void toggleDetail();                      // 折叠 / 展开底部详情栏
    void showDetail();                        // 底部详情栏
    void onAdd();
    void onEdit();
    void onDelete();
    void onExport();
    void openRecordDialog(const TradePlan* plan, const TradeRecord* existing);
    const TradeRecord* selectedRecord() const;   // 按选中行的 id 反查（排序后仍正确）
    double quoteOf(const QString& code, bool* ok = nullptr) const;

    // ⑥ 持仓下一步动作：表格列与动作清单**共用同一份判定**（单一事实来源）。
    // 返回动作短标签（如「止损离场」），why 给一句依据，color 给语义色。
    struct NextAction { QString label; QString why; QColor color; };
    NextAction nextActionFor(const TradeRecord& r) const;

    // ---- AI 诊断（本地规则推理；结论随 CSV 导出逐笔输出）----
    void ensureInsights();                        // 为全部记录（含被筛掉的）生成/复用诊断
    const TradeAI::Insight* insightOf(int id) const;

    JournalStore* m_store = nullptr;          // 交易日志存储后端（MySQL 优先，SQLite 兜底）
    QVector<TradeRecord> m_rows;   // 全量记录（按买入日倒序）
    QVector<int> m_view;           // 过滤后的视图：m_view[i] = m_rows 下标

    QHash<QString, Snapshot> m_snaps;  // code -> 快照（现价 + 当日量价，供盈亏与诊断共用）
    QString m_quoteStamp;              // 行情时间（如 "10-31 14:52"）

    QHash<QString, KlineSeries> m_klines;   // code -> 日 K（AI 诊断可选输入）
    MarketPhase m_phase = MarketPhase::Unknown;
    bool m_phaseKnown = false;

    // 诊断缓存：同一条记录在同一行情 / 同一上下文下不重复计算
    struct AiCache {
        TradeAI::Insight insight;
        int    version = -1;
        double price = -1.0;
    };
    QHash<int, AiCache> m_aiCache;
    int m_contextVersion = 0;

    QTableWidget* m_table = nullptr;
    QLineEdit* m_search = nullptr;
    QTextBrowser* m_detail = nullptr;
    QTextBrowser* m_actions = nullptr;        // ⑥ 持仓下一步动作清单（冒烟契约：ActionList）
    QWidget* m_pageActions = nullptr;         // 上下文行动条（主窗口挂到顶栏右上角）
    QPushButton* m_viewOpen = nullptr;        // 视图分段开关：持仓中
    QPushButton* m_viewAll = nullptr;         // 视图分段开关：全部流水
    QPushButton* m_viewClosed = nullptr;      // 视图分段开关：已了结
    QButtonGroup* m_viewButtons = nullptr;    // 互斥分组，保证分段开关可键盘操作
    int m_viewMode = 1;                       // 0 持仓中 / 1 全部 / 2 已了结

    // ---- 布局与交互 ----
    QSplitter* m_bodySplit = nullptr;         // 表格 | 详情（可拖动 / 折叠）
    QWidget* m_detailPane = nullptr;
    QPushButton* m_btnEdit = nullptr;
    QPushButton* m_btnDel = nullptr;
    QPushButton* m_btnToggleDetail = nullptr;
    QLabel* m_countLabel = nullptr;           // 「显示 N / M 笔」
    QLabel* m_actionCount = nullptr;          // 动作清单条数
    TableEmptyHint* m_emptyHint = nullptr;
    bool m_detailCollapsed = false;

    // ---- 统计卡：4 个主 KPI（22px 数字）+ 6 个次指标（15px 数字）----
    // 原先是 10 个同等大小的数字并排 —— 等于没有重点。
    Comp::KpiTile* m_kpiWinRate = nullptr;
    Comp::KpiTile* m_kpiPnl = nullptr;
    Comp::KpiTile* m_kpiFloat = nullptr;
    Comp::KpiTile* m_kpiDrawdown = nullptr;
    Comp::KpiTile* m_kpiTotal = nullptr;
    Comp::KpiTile* m_kpiExpect = nullptr;
    Comp::KpiTile* m_kpiHold = nullptr;
    Comp::KpiTile* m_kpiFollow = nullptr;
    Comp::KpiTile* m_kpiStopHit = nullptr;
    Comp::KpiTile* m_kpiQuotes = nullptr;
    QLabel* m_lbStore = nullptr;   // 当前生效的存储后端（MySQL / 本地 SQLite）
};
