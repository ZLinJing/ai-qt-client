#pragma once

#include "core/Types.h"
#include "core/TradePlan.h"
#include "data/StockDetailTypes.h"
#include <QWidget>
#include <QStringList>
#include <QHash>

class QTableWidget;
class QLabel;
class QPushButton;
class QComboBox;
class QLineEdit;
class QFrame;
class QResizeEvent;
class QScrollArea;
class QGridLayout;
class QDialog;
class TableEmptyHint;
class StockCard;
class KlineThumbnail;

namespace Comp { class ColumnPicker; }

class AiStockPanel : public QWidget
{
    Q_OBJECT
public:
    explicit AiStockPanel(QWidget* parent = nullptr);

    // 卡片列表列数随面板宽度自适应
    void resizeEvent(QResizeEvent* e) override;

    // 卡片网格当前列数：响应式断言的唯一事实来源（避免冒烟脚本去数控件坐标）
    int cardColumnCount() const { return m_cardColumns; }

    // 列结构的唯一事实来源：冒烟测试等外部校验一律走这里，避免硬编码漂移
    static constexpr int columnCount() { return int(ColCount); }
    static constexpr int buyColumn()  { return int(ColBuy); }
    static constexpr int takeColumn() { return int(ColTake); }
    static constexpr int stopColumn() { return int(ColStop); }
    static constexpr int positionColumn() { return int(ColPosition); }
    static constexpr int riskRewardColumn() { return int(ColRiskReward); }
    static constexpr int riskPctColumn() { return int(ColRiskPct); }
    static constexpr int fillProbColumn() { return int(ColFillProb); }
    static constexpr int reasonColumn() { return int(ColReason); }

    // 数据状态摘要（缓存 / 回退 / 保守估计）—— 主窗口的全局结论条复用同一口径，
    // 避免两处各算一套导致措辞漂移
    static QString freshnessSummary(const RecommendBundle& bundle);
    // 数据状态摘要（短版）：顶栏角标用，只给关键词，完整说明进 tooltip
    static QString freshnessShort(const RecommendBundle& bundle);

    // 注入一次完整荐股结果
    void setBundle(const RecommendBundle& bundle);

    // 加载中占位（启动瞬间先给状态，避免白屏）
    void setLoading(const QString& text);

    // 网络/数据失败提示（仍保留已有结果）
    void setError(const QString& text);

    // 上下文行动条：挂到顶栏右上角，由主窗口在切页时挂载 / 摘下
    QWidget* pageActions() const { return m_pageActions; }

    // 恢复上次保存的列可见性（主窗口在启动时调用）
    void restoreColumnVisibility(const QString& encoded);

    // 详情弹窗的数据来源：由主窗口注入日 K / 全市场快照 / 东财补充数据
    void setMarketData(const QHash<QString, KlineSeries>& klines,
                       const QHash<QString, Snapshot>& snapshots);
    void setSnapshots(const QHash<QString, Snapshot>& snapshots);
    void setStockKline(const QString& code, const KlineSeries& series);
    void setStockDetail(const QString& code, const StockDetailData& data);

    const RecommendBundle& bundle() const { return m_bundle; }

Q_SIGNALS:
    void refreshRequested();
    void recordTradeRequested(const TradePlan& plan);
    void columnVisibilityChanged(const QString& encoded);   // 列可见性存档（主窗口落库）
    void klineRequested(const QString& code, const QString& name);  // 卡片「看K线」
    void detailDataRequested(const QString& code, bool force);      // 按需拉取详情补充数据

private slots:
    void onRefreshClicked();
    void onRowSelected();
    void applyFilters();

private:
    enum Column {
        ColCode = 0, ColName, ColReason, ColBuy, ColTake, ColStop, ColPosition,
        ColRiskReward,   // 盈亏比（保守口径）：<1 红 / 1~1.5 黄 / ≥1.5 绿
        ColRiskPct,      // 单笔止损幅度：结构止损过远标红
        ColFillProb,     // 成交概率：<30% 红 / <60% 黄 / 其余绿
        ColStatus,
        ColCount
    };

    QWidget* buildSummaryBar();        // 今日结论卡 + 数据新鲜度角标
    QWidget* buildPageActions();       // 上下文行动条（顶栏右上角）
    QWidget* buildToolbar();
    QWidget* buildCardView();          // 卡片列表（滚动区 + 自适应列网格）
    void rebuildTable();               // 内部数据源：排序 / 选中 / 兼容性校验仍走表格
    void rebuildCards();               // 按表格当前行序重建卡片列表
    void reflowCards();                // 按可用宽度切换 1 / 2 / 3 列
    void updateCardSelection();        // 表格选中态 → 卡片高亮
    void showCardDetail(const QString& code);  // 「查看详情」弹窗
    void refreshCardDetail();          // 补充数据到达后重绘弹窗内容
    QString buildDetailHtml(const TradePlan& p) const;

    static constexpr int kCardMinWidth = 320;   // 单张卡片的目标最小宽度
    static constexpr int kCardSpacing = 16;     // 卡片之间的间距（4px 栅格）
    static constexpr int kCardMaxColumns = 3;   // 桌面宽屏最多 3 列
    void updateKindFilterCounts();     // 筛选下拉框带实时数量（全部/可执行/观察）
    TradePlan currentPlan() const;
    QString currentCode() const;       // 当前选中行的代码（排序后依然正确）
    void selectCode(const QString& code);
    void updateActionTips();           // 按钮提示随选中态联动
    void warnNoSelection(const QString& action);

    void updateDecisionCard();         // 白话结论 / 原因 / 新鲜度

    QLabel* m_verdict = nullptr;      // 判定原文（次要小字，供核对）
    QLabel* m_status = nullptr;       // 状态行
    QPushButton* m_btnTrade = nullptr;
    QWidget* m_pageActions = nullptr; // 上下文行动条（主窗口挂到顶栏右上角）
    QPushButton* m_btnColumns = nullptr;      // 「列」：列可见性菜单
    Comp::ColumnPicker* m_columnPicker = nullptr;

    // ---- 今日结论卡 ----
    QLabel* m_headline = nullptr;     // 白话结论（大字）
    QLabel* m_reason = nullptr;       // 一句人话原因
    QLabel* m_meta = nullptr;         // 可执行数 / 观察数 / 最晚决策时点
    QLabel* m_freshness = nullptr;    // 数据新鲜度角标
    QPushButton* m_reasonToggle = nullptr;  // 「▸ 判定说明」折叠开关（判定原文）

    QComboBox* m_kindFilter = nullptr;
    QLineEdit* m_keyword = nullptr;
    QTableWidget* m_table = nullptr;      // 隐藏的数据源：排序 / 选中 / 冒烟校验沿用
    TableEmptyHint* m_emptyHint = nullptr;

    // ---- 卡片列表（可见的结果视图）----
    QScrollArea* m_cardScroll = nullptr;
    QGridLayout* m_cardGrid = nullptr;
    QLabel* m_cardEmpty = nullptr;
    QDialog* m_detailDialog = nullptr;   // 卡片「查看详情」复用窗口（非模态）
    QLabel* m_detailBody = nullptr;
    KlineThumbnail* m_thumb = nullptr;   // 详情弹窗里的 K 线缩略图
    QString m_detailCode;                // 当前弹窗展示的标的
    bool m_detailLoading = false;        // 补充数据是否正在拉取
    QHash<QString, KlineSeries> m_klines;        // 主窗口注入的日 K（120 日）
    QHash<QString, Snapshot> m_snapshots;        // 主窗口注入的全市场快照
    QHash<QString, StockDetailData> m_details;   // 财报 / 资金流 / 机构持仓
    QVector<StockCard*> m_cards;
    int  m_cardColumns = 0;
    int  m_cardAreaWidth = 0;        // 布局算出的卡片区目标宽度（不依赖滚动视口重排时机）
    bool m_cardLayoutDirty = false;

    RecommendBundle m_bundle;
    QVector<TradePlan> m_shownPool;       // 候选池 = plans + 备选（原决策台候选池并入，按代码去重）
    QVector<TradePlan> m_filtered;        // m_shownPool 经筛选 / 搜索后的表格显示集

    // 排序状态不落地到成员：直接读表头的排序指示器
    // （QHeaderView::sortIndicatorSection / sortIndicatorOrder），
    // 它是「当前按哪一列、哪个方向排」的权威来源，重建表格后天然保持一致。
};
